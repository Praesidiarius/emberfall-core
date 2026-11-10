/*
 * This file is part of Emberfall's AzerothCore fork (modules/mod-emberfall). See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

// The living waters' server logic that needs no core (Emberfall docs/design/fishing.md §4.3, §7.4): the loaded data
// (cells, waters, styles, species, baits) and the bite roll at the hook click. The rules themselves (activity, gates,
// sizes) are the shared EfFishRules.h; the fight and its bounds the shared EfFishFight.h. emberfall_fishing.cpp uses
// it; tests/fishing_tests.cpp checks it.

#ifndef EMBERFALL_FISH_LOGIC_H
#define EMBERFALL_FISH_LOGIC_H

#include "EfFishFight.h"
#include "EfFishRules.h"
#include "../sun/EfSunTime.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Emberfall::Fishing
{
    using effish::Rarity;
    using effish::Species;

    // The rules' time-of-day bit for a part of the day by the sun (src/sun/EfSunTime.h: fishing.md §4.2's windows are
    // read on the sun clock, by the server's roll and by the client's water clues alike).
    constexpr std::uint8_t TimeBitOf(efsun::Part part) { return static_cast<std::uint8_t>(1u << static_cast<unsigned>(part)); }
    static_assert(TimeBitOf(efsun::Part::Dawn) == effish::kDawn && TimeBitOf(efsun::Part::Day) == effish::kDay &&
                  TimeBitOf(efsun::Part::Dusk) == effish::kDusk && TimeBitOf(efsun::Part::Night) == effish::kNight);

    // An ef_fish_cell row.
    struct Cell
    {
        std::uint32_t water = 0;
        std::uint8_t mask = 0;
        std::uint16_t depthDm = 0;
    };

    // An ef_fish_water row.
    struct Water
    {
        std::uint32_t id = 0;
        std::uint32_t map = 0;
        std::uint32_t zone = 0;
        std::uint8_t kind = 0;
        std::string name;
    };

    // Everything the world tables give, by key.
    struct Data
    {
        std::unordered_map<std::uint64_t, Cell> cells;          // by effish::CellKey
        std::map<std::uint32_t, Water> waters;
        std::map<std::uint32_t, effish::Style> styles;
        std::vector<Species> species;                           // in id order
        std::map<std::uint32_t, std::string> names;             // species id -> name (logs, GM commands)
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> likes;   // (species, bait item) -> permil
        std::map<std::uint32_t, std::uint32_t> baitByAura;      // bait aura -> bait item
        std::uint64_t hash = 0;

        Cell const* CellAt(std::uint32_t map, float x, float y) const
        {
            auto it = cells.find(effish::CellKey(map, effish::CellOf(x), effish::CellOf(y)));
            return it == cells.end() ? nullptr : &it->second;
        }
        Species const* SpeciesById(std::uint32_t id) const
        {
            for (Species const& s : species)
                if (s.id == id)
                    return &s;
            return nullptr;
        }
        effish::Style StyleOf(std::uint32_t id) const
        {
            auto it = styles.find(id);
            if (it != styles.end())
                return it->second;
            effish::Style st;
            st.id = id;
            return st;
        }
        // The bait like (ef_fish_species_bait): -1 no bait on the hook, 1000 a bait the species does not list.
        int Like(std::uint32_t species, std::uint32_t baitItem) const
        {
            if (baitItem == 0)
                return -1;
            auto it = likes.find({species, baitItem});
            return it == likes.end() ? 1000 : int(it->second);
        }
    };

    // The legendary's state for this angler (fishing.md §7.4).
    enum class Legend : std::uint8_t
    {
        Closed,     // the gate quest neither in the log nor done, or no ground-bait near: never bites
        Questing,   // the gate quest in the log, incomplete, ground-bait near: about one bite in three is him
        Open,       // the chain done (and not landed today): a rare-like candidate of weight 2
    };

    // What the hook click knows.
    struct RollInput
    {
        std::uint32_t water = 0;
        std::uint8_t mask = 0;
        std::uint8_t timeBit = effish::kDay;
        std::uint8_t weatherBits = effish::kClear;
        int skill = 0;
        int zoneBase = 0;
        bool shoal = false;              // the bobber is in a fishing pool's radius: no junk, its own fish x3
        std::uint32_t shoalItem = 0;     // the pool's own fish
        std::uint32_t baitItem = 0;      // the bait on the hook (0: none)
        std::function<Legend(Species const&)> legend;   // per legendary species; empty: Closed
        std::uint32_t forced = 0;        // `.effish force`: this species bites (if it lives in the water at all)
    };

    enum class RollKind : std::uint8_t { Junk, Fish };

    struct RollResult
    {
        RollKind kind = RollKind::Junk;
        Species const* species = nullptr;
        std::uint32_t sizeMm = 0;
        int chance = 0;     // the junk roll's chance in percent (the core's numbers, for the skill-up hook)
        int roll = 0;
    };

    // The core's junk chance (GameObject::Use, FISHINGNODE): certain from base + 95, else (skill / (base + 95))^2.
    inline int CatchChance(int skill, int zoneBase)
    {
        int const noMiss = zoneBase + 95;
        if (skill >= noMiss)
            return 100;
        if (noMiss <= 0)
            return 100;
        int chance = int(std::pow(double(skill) / noMiss, 2) * 100);
        return chance < 1 ? 1 : chance;
    }

    // The weight of a species for this bite (0: not a candidate).
    inline std::uint64_t Weight(Data const& data, Species const& s, RollInput const& in)
    {
        if (s.rarity == Rarity::Legendary)
        {
            // only through his chain; Questing is picked apart from the weights (Roll)
            Legend const l = in.legend ? in.legend(s) : Legend::Closed;
            if (l != Legend::Open)
                return 0;
        }
        if (!effish::PassesGate(s.rarity, in.skill, in.zoneBase))
            return 0;
        std::uint64_t w = effish::Activity(s, in.water, in.mask, in.timeBit, in.weatherBits);
        if (w == 0)
            return 0;
        w = w * effish::BaitPermil(s.rarity, data.Like(s.id, in.baitItem)) / 1000;
        if (in.shoal && in.shoalItem != 0 && s.item == in.shoalItem)
            w *= 3;
        return w;
    }

    // The bite (fishing.md §4.3): junk or a species and its size.
    inline RollResult Roll(Data const& data, RollInput const& in, effish::Rng& rng)
    {
        RollResult out;
        out.chance = in.shoal ? 100 : CatchChance(in.skill, in.zoneBase);
        out.roll = int(rng.Range(1, 100));

        auto pick = [&](Species const& s) {
            out.kind = RollKind::Fish;
            out.species = &s;
            out.sizeMm = effish::PickSizeMm(rng, s.sizeTypMm, s.sizeMaxMm);
            return out;
        };

        if (in.forced)
            if (Species const* s = data.SpeciesById(in.forced))
                if (effish::InWater(*s, in.water))
                    return pick(*s);

        if (out.chance < out.roll)
            return out;   // junk

        // the legendary on his quest: one bite in three while everything is right
        for (Species const& s : data.species)
        {
            if (s.rarity != Rarity::Legendary || !in.legend || in.legend(s) != Legend::Questing)
                continue;
            if (!effish::PassesGate(s.rarity, in.skill, in.zoneBase))
                continue;
            if (effish::Activity(s, in.water, in.mask, in.timeBit, in.weatherBits) == 0)
                continue;
            if (effish::BaitPermil(s.rarity, data.Like(s.id, in.baitItem)) == 0)
                continue;
            if (rng.Below(3) == 0)
                return pick(s);
        }

        std::uint64_t total = 0;
        std::vector<std::pair<Species const*, std::uint64_t>> candidates;
        for (Species const& s : data.species)
        {
            std::uint64_t const w = Weight(data, s, in);
            if (w == 0)
                continue;
            candidates.emplace_back(&s, w);
            total += w;
        }
        if (total == 0)
            return out;   // nothing lives here now: the line brings junk
        std::uint64_t r = rng.Next() % total;
        for (auto const& [s, w] : candidates)
        {
            if (r < w)
                return pick(*s);
            r -= w;
        }
        return pick(*candidates.back().first);
    }

    // `.effish stats`: a character's fights since the server started.
    struct Stats
    {
        std::uint32_t fights = 0;
        std::uint32_t landed = 0;
        std::uint32_t rejected = 0;
        std::uint64_t landedMs = 0;
        std::uint64_t hourStart = 0;     // the rate check: fights in the current hour
        std::uint32_t hourFights = 0;
        bool hourLogged = false;

        // One more fight at `nowSec`; true when it is the 151st of the hour (log once).
        bool Count(std::uint64_t nowSec)
        {
            ++fights;
            if (nowSec - hourStart >= 3600)
            {
                hourStart = nowSec;
                hourFights = 0;
                hourLogged = false;
            }
            if (++hourFights > 150 && !hourLogged)
            {
                hourLogged = true;
                return true;
            }
            return false;
        }
    };
}

#endif
