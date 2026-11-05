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

// Hunter taming's logic that needs no core (Emberfall docs/design/hunter-taming.md §4-§9, §14): the bars' sizes, the
// arrow pick, Drowse and its decay, the claim, Out Cold's sleep and the draughts, the food's Trust by family and band,
// the eating order, the slots' moves. emberfall_taming.cpp drives one State per beast; tests/taming_tests.cpp checks it.
// Bars are kept x1000 (milli-points) so a quarter arrow and a 2 % decay stay exact.

#ifndef EMBERFALL_TAME_LOGIC_H
#define EMBERFALL_TAME_LOGIC_H

#include "EfEncounterLogic.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Emberfall::Taming
{
    // ef_tame_rule (the defaults are the doc's §6).
    struct Rule
    {
        std::uint32_t drowseBase = 20, drowsePerLevel = 4;
        std::uint32_t trustBase = 100, trustPerLevel = 2;
        std::uint32_t drowseElite = 2500, drowseRare = 4000, drowseRareElite = 6000;
        std::uint32_t trustElite = 1500, trustRare = 2000, trustRareElite = 2500;
        std::uint32_t outColdMs = 60000, eatEveryMs = 12000;
        std::uint32_t decayAfterMs = 6000, decayPermil = 20;
        std::uint32_t draughtUnderMs = 15000, sleepCapMs = 120000;
        std::uint32_t aboveLevelPermil = 250, bandStepPermil = 500;
    };

    enum class Rank { Normal, Elite, Rare, RareElite };

    // creature_template.rank: 0 normal, 1 elite, 2 rare elite, 3 boss (as an elite), 4 rare.
    inline Rank RankOf(std::uint32_t creatureRank)
    {
        switch (creatureRank)
        {
            case 1: case 3: return Rank::Elite;
            case 2: return Rank::RareElite;
            case 4: return Rank::Rare;
            default: return Rank::Normal;
        }
    }

    // Beastcraft points for a tame (§3.1): one, three for an elite, five for a rare (a rare elite is a rare).
    inline std::uint32_t TameSkillPoints(Rank r)
    {
        switch (r)
        {
            case Rank::Elite: return 3;
            case Rank::Rare: case Rank::RareElite: return 5;
            default: return 1;
        }
    }

    // The bars' maxima, x1000.
    inline std::uint32_t DrowseMax(Rule const& r, std::uint32_t level, Rank rank)
    {
        std::uint64_t const base = std::uint64_t(r.drowseBase + r.drowsePerLevel * level) * 1000;
        std::uint32_t const mul = rank == Rank::Elite ? r.drowseElite : rank == Rank::Rare ? r.drowseRare
                                : rank == Rank::RareElite ? r.drowseRareElite : 1000;
        return std::uint32_t(base * mul / 1000);
    }

    // A normal beast's Trust bar of this level, x1000: trust_base + trust_per_level x level, so an older beast takes more
    // feeding (feedback #415: a level-10 tutorial beast about 1.5 min, a level-60 one about 3).
    inline std::uint32_t TrustNormal(Rule const& r, std::uint32_t level) { return (r.trustBase + r.trustPerLevel * level) * 1000; }

    inline std::uint32_t TrustMax(Rule const& r, std::uint32_t level, Rank rank)
    {
        std::uint32_t const mul = rank == Rank::Elite ? r.trustElite : rank == Rank::Rare ? r.trustRare
                                : rank == Rank::RareElite ? r.trustRareElite : 1000;
        return std::uint32_t(std::uint64_t(TrustNormal(r, level)) * mul / 1000);
    }

    // The level bands of the item groups (mini-encounters.md §3): 1-11, 12-21, 22-31, 32-41, 42-60.
    inline std::uint8_t BandOfLevel(std::uint32_t level)
    {
        if (level <= 11)
            return 1;
        if (level <= 21)
            return 2;
        if (level <= 31)
            return 3;
        if (level <= 41)
            return 4;
        return 5;
    }

    // --- the arrows and draughts -------------------------------------------------------------------------------------

    struct Arrow
    {
        std::uint32_t item = 0, drowse = 0, maxLevel = 0, skill = 0;
    };

    struct Draught
    {
        std::uint32_t item = 0, sleepMs = 0, maxLevel = 0;
    };

    // The arrow a Drowse Shot uses (§4.1): the strongest the hunter carries that the beast's level allows; when none
    // does, the strongest he carries (it counts a quarter). `has(item)` says whether the bags hold one.
    template <typename Has>
    inline std::optional<Arrow> PickArrow(std::vector<Arrow> const& arrows, std::uint32_t beastLevel, Has has)
    {
        std::optional<Arrow> allowed, any;
        for (Arrow const& a : arrows)
        {
            if (!has(a.item))
                continue;
            if (a.maxLevel >= beastLevel && (!allowed || a.drowse > allowed->drowse))
                allowed = a;
            if (!any || a.drowse > any->drowse)
                any = a;
        }
        return allowed ? allowed : any;
    }

    // An arrow's Drowse on this beast, x1000.
    inline std::uint32_t ArrowDrowse(Rule const& r, Arrow const& a, std::uint32_t beastLevel)
    {
        std::uint64_t const full = std::uint64_t(a.drowse) * 1000;
        return std::uint32_t(beastLevel > a.maxLevel ? full * r.aboveLevelPermil / 1000 : full);
    }

    inline std::uint32_t DraughtMs(Rule const& r, Draught const& d, std::uint32_t beastLevel)
    {
        return beastLevel > d.maxLevel ? std::uint32_t(std::uint64_t(d.sleepMs) * r.aboveLevelPermil / 1000) : d.sleepMs;
    }

    // --- the families' tastes ----------------------------------------------------------------------------------------

    struct Like
    {
        std::string group;
        std::uint32_t permil = 0;   // a share of a normal beast's Trust bar; 0 refuses
    };

    struct Family
    {
        std::uint32_t otherPermil = 0;
        std::vector<Like> likes;    // in `ord` order: the first whose group holds the item counts
    };

    // What a beast makes of an item (before the band): its share x1000, and the group that matched ("" for other).
    struct Taste
    {
        std::uint32_t permil = 0;
        std::string group;
        std::uint8_t band = 0;      // the item's band in that group (0: other, no band)
    };

    class Tastes
    {
    public:
        void Clear() { m_families.clear(); }
        void SetFamily(std::uint32_t family, std::uint32_t otherPermil) { m_families[family].otherPermil = otherPermil; }
        void AddLike(std::uint32_t family, std::string group, std::uint32_t permil) { m_families[family].likes.push_back({std::move(group), permil}); }
        bool Has(std::uint32_t family) const { return m_families.count(family) != 0; }
        std::size_t Size() const { return m_families.size(); }

        Taste Resolve(std::uint32_t family, std::uint32_t item, Encounters::ItemGroups const& groups) const
        {
            auto f = m_families.find(family);
            if (f == m_families.end())
                return {};
            for (Like const& l : f->second.likes)
                if (auto band = groups.Band(l.group, item))
                    return {l.permil, l.group, *band};
            return {f->second.otherPermil, std::string(), 0};
        }

        // The highest share a family gives anything (its favourite; the taste letters are relative to it).
        std::uint32_t BestPermil(std::uint32_t family) const
        {
            auto f = m_families.find(family);
            std::uint32_t best = 0;
            if (f != m_families.end())
                for (Like const& l : f->second.likes)
                    best = std::max(best, l.permil);
            return best;
        }

    private:
        std::map<std::uint32_t, Family> m_families;
    };

    // The Trust an item gives this beast, x1000 (§5): its share of trust_base (the same points at every level, while the
    // bar grows with it), halved (band_step) for each band the item is below the beast's.
    inline std::uint32_t FoodTrust(Rule const& r, Taste const& t, std::uint32_t beastLevel)
    {
        std::uint64_t v = std::uint64_t(r.trustBase) * 1000 * t.permil / 1000;
        std::uint8_t const beastBand = BandOfLevel(beastLevel);
        if (t.band)
            for (std::uint8_t b = t.band; b < beastBand; ++b)
                v = v * r.bandStepPermil / 1000;
        return std::uint32_t(v);
    }

    // The EAT taste letter (EfTameWire.h) of a share: C a cake (160 and up), F favourite (120), L liked (60), P plain
    // (anything more than 0), R refused. families.yaml's shares (cake 0.18, favourite 0.14, liked 0.07, plain 0.035)
    // sit just above these steps (hunter-taming.md §5).
    inline char TasteLetter(std::uint32_t permil)
    {
        if (permil == 0)
            return 'R';
        if (permil >= 160)
            return 'C';
        if (permil >= 120)
            return 'F';
        if (permil >= 60)
            return 'L';
        return 'P';
    }

    // --- one beast's state -------------------------------------------------------------------------------------------

    enum class Phase : std::uint32_t { Drowsing = 1, OutCold = 2, WaitStable = 3, WaitHunter = 4 };

    constexpr std::size_t kSlots = 6;

    struct Slot
    {
        std::uint32_t item = 0;
        std::uint32_t count = 0;
    };

    struct State
    {
        std::uint64_t beast = 0;
        std::uint32_t family = 0, level = 0;
        Rank rank = Rank::Normal;
        Phase phase = Phase::Drowsing;
        std::uint64_t startMs = 0;

        std::uint32_t drowse = 0, drowseMax = 0;            // x1000
        std::uint64_t lastDrowseMs = 0;
        std::vector<std::pair<std::uint64_t, std::uint32_t>> drowsers;   // who added Drowse, in order, and how much
        std::uint32_t arrows = 0;

        std::uint64_t sleepEndMs = 0;
        std::uint32_t sleepLenMs = 0;                       // the Sleep bar's max: the longest sleep so far
        std::uint32_t trust = 0, trustMax = 0;              // x1000
        std::array<Slot, kSlots> slots{};
        std::array<bool, kSlots> sniffed{};                 // a refused item's EAT R was sent (once per item put)
        std::uint64_t nextEatMs = 0;
        std::uint32_t eaten = 0;
        std::uint64_t claimant = 0;                         // fixed at Out Cold
    };

    inline State Begin(Rule const& r, std::uint64_t beast, std::uint32_t family, std::uint32_t level, Rank rank, std::uint64_t now)
    {
        State s;
        s.beast = beast;
        s.family = family;
        s.level = level;
        s.rank = rank;
        s.startMs = now;
        s.lastDrowseMs = now;
        s.drowseMax = DrowseMax(r, level, rank);
        s.trustMax = TrustMax(r, level, rank);
        return s;
    }

    // The hunter with the most Drowse; ties to the first (§4.2).
    inline std::uint64_t Claimant(State const& s)
    {
        std::uint64_t who = 0;
        std::uint32_t most = 0;
        for (auto const& [guid, amount] : s.drowsers)
            if (!who || amount > most)
            {
                who = guid;
                most = amount;
            }
        return who;
    }

    // A Drowse hit (x1000). True when the bar is full (the caller puts the beast Out Cold).
    inline bool AddDrowse(State& s, std::uint64_t who, std::uint32_t amount, std::uint64_t now)
    {
        if (s.phase != Phase::Drowsing)
            return false;
        auto it = std::find_if(s.drowsers.begin(), s.drowsers.end(), [who](auto const& p) { return p.first == who; });
        if (it == s.drowsers.end())
            s.drowsers.emplace_back(who, amount);
        else
            it->second += amount;
        s.drowse = std::min<std::uint64_t>(std::uint64_t(s.drowse) + amount, s.drowseMax);
        s.lastDrowseMs = now;
        ++s.arrows;
        return s.drowse >= s.drowseMax;
    }

    // Drowse decays decay_permil of its max a second after decay_after without a hit, or at once while another crowd
    // control holds the beast (§7). `elapsedMs` since the last call.
    inline void Decay(Rule const& r, State& s, std::uint64_t now, std::uint32_t elapsedMs, bool held)
    {
        if (s.phase != Phase::Drowsing || s.drowse == 0)
            return;
        if (!held && now < s.lastDrowseMs + r.decayAfterMs)
            return;
        std::uint64_t const loss = std::uint64_t(s.drowseMax) * r.decayPermil / 1000 * elapsedMs / 1000;
        s.drowse = loss >= s.drowse ? 0 : std::uint32_t(s.drowse - loss);
    }

    // Drowse full: Out Cold for out_cold_ms, the claim fixed.
    inline void StartOutCold(Rule const& r, State& s, std::uint64_t now)
    {
        s.phase = Phase::OutCold;
        s.claimant = Claimant(s);
        s.sleepEndMs = now + r.outColdMs;
        s.sleepLenMs = r.outColdMs;
        s.nextEatMs = now + r.eatEveryMs;
    }

    inline bool Asleep(State const& s) { return s.phase != Phase::Drowsing; }

    inline std::uint32_t SleepLeft(State const& s, std::uint64_t now) { return now >= s.sleepEndMs ? 0 : std::uint32_t(s.sleepEndMs - now); }

    // Adds sleep (a draught), never past sleep_cap left.
    inline std::uint32_t AddSleep(Rule const& r, State& s, std::uint32_t ms, std::uint64_t now)
    {
        std::uint64_t const end = std::min<std::uint64_t>(std::max(s.sleepEndMs, now) + ms, now + r.sleepCapMs);
        std::uint32_t const added = end > s.sleepEndMs ? std::uint32_t(end - std::max(s.sleepEndMs, now)) : 0;
        s.sleepEndMs = end;
        s.sleepLenMs = std::max(s.sleepLenMs, SleepLeft(s, now));
        return added;
    }

    // --- the slots ---------------------------------------------------------------------------------------------------

    // Why a PUT is refused (the EFN ERR codes are the caller's).
    enum class PutError { None, Bounds, Occupied };

    // Puts `count` of `item` into slot `i` (an empty slot, or one holding the same item, up to `stack`).
    inline PutError CanPut(State const& s, std::uint32_t i, std::uint32_t item, std::uint32_t count, std::uint32_t stack)
    {
        if (i >= kSlots || !item || !count)
            return PutError::Bounds;
        Slot const& slot = s.slots[i];
        if (slot.item && slot.item != item)
            return PutError::Occupied;
        if (slot.count + count > std::max<std::uint32_t>(stack, 1))
            return PutError::Bounds;
        return PutError::None;
    }

    inline void Put(State& s, std::uint32_t i, std::uint32_t item, std::uint32_t count)
    {
        s.slots[i].item = item;
        s.slots[i].count += count;
        s.sniffed[i] = false;
    }

    // Empties slot `i`; what it held.
    inline Slot Take(State& s, std::uint32_t i)
    {
        if (i >= kSlots)
            return {};
        Slot const out = s.slots[i];
        s.slots[i] = {};
        s.sniffed[i] = false;
        return out;
    }

    // Takes one from slot `i`.
    inline void Consume(State& s, std::uint32_t i)
    {
        if (i < kSlots && s.slots[i].count && --s.slots[i].count == 0)
            s.slots[i].item = 0;
    }

    // The slot the beast eats from next (§4.2): the food worth the most Trust; refused food and draughts stay. -1: none.
    template <typename IsDraught>
    inline int PickFood(Rule const& r, State const& s, Tastes const& tastes, Encounters::ItemGroups const& groups, IsDraught isDraught)
    {
        int best = -1;
        std::uint32_t bestTrust = 0;
        for (std::size_t i = 0; i < kSlots; ++i)
        {
            Slot const& slot = s.slots[i];
            if (!slot.item || !slot.count || isDraught(slot.item))
                continue;
            std::uint32_t const t = FoodTrust(r, tastes.Resolve(s.family, slot.item, groups), s.level);
            if (t > bestTrust)
            {
                best = int(i);
                bestTrust = t;
            }
        }
        return best;
    }

    // The slot holding the draught to drink (the strongest), -1: none.
    template <typename SleepOf>
    inline int PickDraught(State const& s, SleepOf sleepOf)
    {
        int best = -1;
        std::uint32_t most = 0;
        for (std::size_t i = 0; i < kSlots; ++i)
            if (s.slots[i].item && s.slots[i].count)
                if (std::uint32_t const ms = sleepOf(s.slots[i].item); ms > most)
                {
                    best = int(i);
                    most = ms;
                }
        return best;
    }

    // The refused items in the slots (sniffed once each, so the hunter learns): the first slot not sniffed yet whose food
    // gives no Trust and is not a draught; -1: none.
    template <typename IsDraught>
    inline int FirstRefused(Rule const& r, State const& s, Tastes const& tastes, Encounters::ItemGroups const& groups, IsDraught isDraught)
    {
        for (std::size_t i = 0; i < kSlots; ++i)
        {
            Slot const& slot = s.slots[i];
            if (slot.item && slot.count && !s.sniffed[i] && !isDraught(slot.item) &&
                FoodTrust(r, tastes.Resolve(s.family, slot.item, groups), s.level) == 0)
                return int(i);
        }
        return -1;
    }

    inline bool TrustFull(State const& s) { return s.trust >= s.trustMax; }

    inline void AddTrust(State& s, std::uint32_t amount) { s.trust = std::min<std::uint64_t>(std::uint64_t(s.trust) + amount, s.trustMax); }
}

#endif
