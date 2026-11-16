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

// Emberfall: the secondary professions on the primaries' steps (Emberfall docs/design/secondary-professions.md §10,
// Nathanael 2026-11-16, feedback #596). Cooking, First Aid and Beastcraft capped at 75 / 150 / 225 / 300 and now at
// the primaries' 75 / 175 / 325 / 500. Every skill number moves by one rule, Emberfall's
// tools/py/emberfall_content/secondary_ranks.py `remap` (its tests and tests/secondaryranks_tests.cpp run the same
// examples): piecewise linear between the rank caps, rounded down, 0-75 staying; above 300 the last band's slope goes
// on. Pure: the rule, a character's skill after the move and the login line in the client's language. No core types,
// so tests/secondaryranks_tests.cpp runs it without a server.

#ifndef EF_SECONDARY_RANKS_H
#define EF_SECONDARY_RANKS_H

#include "EfOneProfession.h"   // Lang, LangOf

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace Emberfall::Professions::SecondaryRanks
{
    inline constexpr std::array<uint32_t, 4> kOldCaps{ 75, 150, 225, 300 };
    inline constexpr std::array<uint32_t, 4> kNewCaps{ 75, 175, 325, 500 };
    // Cooking, First Aid, Beastcraft (stock IDs 185 and 129; ours 900)
    inline constexpr std::array<uint32_t, 3> kSkills{ 185, 129, 900 };
    // ef_character_migration.name of the login migration
    inline constexpr char const* kMigration = "secondary_ranks_v2";

    // A skill number on the old steps at the same place on the new ones (0 -> 0, 75 -> 75, 141 -> 163, 150 -> 175,
    // 151 -> 177, 225 -> 325, 300 -> 500). Every slope is at least 1: strictly rising.
    inline uint32_t Remap(uint32_t value)
    {
        uint32_t loOld = 0, loNew = 0;
        for (size_t i = 0; i < kOldCaps.size(); ++i)
        {
            if (value <= kOldCaps[i])
                return loNew + (value - loOld) * (kNewCaps[i] - loNew) / (kOldCaps[i] - loOld);
            loOld = kOldCaps[i];
            loNew = kNewCaps[i];
        }
        uint32_t const o0 = kOldCaps[kOldCaps.size() - 2], n0 = kNewCaps[kNewCaps.size() - 2];
        return kNewCaps.back() + (value - kOldCaps.back()) * (kNewCaps.back() - n0) / (kOldCaps.back() - o0);
    }

    // The rank (0-based) whose cap is `cap` on `caps`, or -1.
    template <size_t N>
    inline int RankOfCap(std::array<uint32_t, N> const& caps, uint32_t cap)
    {
        for (size_t i = 0; i < N; ++i)
            if (caps[i] == cap)
                return static_cast<int>(i);
        return -1;
    }

    struct Moved
    {
        uint32_t value = 0, cap = 0, step = 0;   // step: the rank, 1-4 (the skill's PLAYER_SKILL_INDEX step)
    };

    // A character's skill after the move, from what its save holds (`storedValue`/`storedCap`: character_skills as
    // the last save wrote it) and the cap it has after the load (`loadedCap`: the core's load already raised the cap to
    // the new one of the rank spells the character knows, SpellLearnSkill from SkillTiers 1, but not the value). None
    // when the save is not on the old steps: not learned, Apprentice (nothing moves), or on the new steps already. The
    // cap is the new cap of the rank held (the loaded one when it is higher: never lower in rank), the value the old
    // one moved by Remap (never above the cap: never between two ranks' caps).
    inline std::optional<Moved> Migrate(uint32_t storedValue, uint32_t storedCap, uint32_t loadedCap = 0)
    {
        int const rank = RankOfCap(kOldCaps, storedCap);
        if (!storedValue || rank < 0 || kNewCaps[rank] == storedCap)
            return std::nullopt;
        Moved m;
        m.cap = kNewCaps[rank];
        if (int const loaded = RankOfCap(kNewCaps, loadedCap); loaded > rank)
            m.cap = loadedCap;
        m.step = static_cast<uint32_t>(RankOfCap(kNewCaps, m.cap) + 1);
        m.value = Remap(storedValue);
        if (m.value > m.cap)
            m.value = m.cap;
        return m;
    }

    // The line the character reads at the login that moved a skill ("Cooking: 141/150 is now 163/175."), the
    // skill's name in the language.
    inline std::string SkillName(uint32_t skill, Lang lang)
    {
        switch (skill)
        {
            case 185: return lang == Lang::De ? "Kochkunst" : lang == Lang::Fr ? "Cuisine" : "Cooking";
            case 129: return lang == Lang::De ? "Erste Hilfe" : lang == Lang::Fr ? "Secourisme" : "First Aid";
            case 900: return lang == Lang::De ? "Tierkunde" : lang == Lang::Fr ? "Art des bêtes" : "Beastcraft";
            default: return std::to_string(skill);
        }
    }

    inline std::string LoginLine(Lang lang, std::string const& name, uint32_t oldValue, uint32_t oldCap, uint32_t value,
                                 uint32_t cap)
    {
        std::string const was = std::to_string(oldValue) + "/" + std::to_string(oldCap);
        std::string const now = std::to_string(value) + "/" + std::to_string(cap);
        switch (lang)
        {
            case Lang::De:
                return name + " steigt jetzt wie die Hauptberufe bis 500. Deine Fertigkeit " + was + " ist jetzt " + now +
                       ", Rang und Rezepte bleiben.";
            case Lang::Fr:
                return name + " monte désormais jusqu'à 500, comme les métiers principaux. Votre compétence " + was +
                       " devient " + now + ", votre rang et vos recettes restent.";
            default:
                return name + " now goes up to 500, like the main professions. Your skill " + was + " is now " + now +
                       "; your rank and recipes stay.";
        }
    }
}

#endif
