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

// The dungeon logic that needs no core (Emberfall docs/design/dungeon-barrows.md §7-8): the dev warden's gating and
// menu, and the barrows' seals. emberfall_dungeons.cpp uses it; tests/dungeon_tests.cpp (tests/run_tests.sh) checks it,
// among others that with the test tools off the warden has nothing to offer and never stays in the world.

#ifndef EMBERFALL_DUNGEON_LOGIC_H
#define EMBERFALL_DUNGEON_LOGIC_H

#include <cstdint>
#include <string_view>
#include <vector>

namespace Emberfall::Dungeons
{
    // --- the Barrows of Cairnmoor (content/data/instances.yaml, barrows/triggers.yaml) --------------------------------
    constexpr std::uint32_t kBarrowsMap = 751;
    constexpr std::uint8_t kBarrowsMinLevel = 55;     // dungeon_access_template: the door's level
    constexpr std::uint8_t kTestLevel = 60;           // what "Make me level 60" gives (clamped to the realm's cap)

    struct Spot
    {
        std::uint32_t map;
        float x, y, z, o;
    };
    // Just inside the mound door (layout.yaml `spawn`, design (245, 466)), facing north into the barrow.
    constexpr Spot kBarrowsInside{ kBarrowsMap, 47.33f, 268.33f, 30.1f, 0.0f };
    // Beside the mound door on Cairnmoor's Barrow Field (map 0), facing west.
    constexpr Spot kBarrowsDoor{ 0, -5092.0f, -1295.0f, 59.0f, 1.5708f };
    // `.tele millbrook` (content/data/world.yaml, design (146, 275) on the green), and the same place should game_tele
    // have no row of that name.
    constexpr std::string_view kMillbrookTele = "millbrook";
    constexpr Spot kMillbrook{ 0, 23.33f, 230.33f, 46.8f, 3.14159f };

    // The creatures (content/data/barrows/creatures.yaml).
    constexpr std::uint32_t kFirstInterred = 97015;
    constexpr std::uint32_t kHonourGuard = 97008;
    constexpr std::uint32_t kWardenMillbrook = 97050;
    constexpr std::uint32_t kWardenBarrows = 97051;

    // --- the dev warden ---------------------------------------------------------------------------------------------
    // Gated on Emberfall.TestTools.Enable: a test realm has it, a live realm never does. Off, the warden leaves the world
    // as soon as it is in it (and on every respawn) and its gossip does nothing.
    inline bool WardenStays(bool testToolsOn) { return testToolsOn; }

    enum class WardenOption : std::uint32_t
    {
        ToBarrows = 1,      // "Take me to the Barrows of Cairnmoor"
        Level60 = 2,        // "Make me level 60"
        ResetBarrows = 3,   // "Reset the Barrows of Cairnmoor" (outside only: the core resets an instance nobody is in)
        ToMillbrook = 4,    // "Take me back to Millbrook"
        ToDoor = 5,         // "Take me to the mound door on Cairnmoor"
    };

    struct WardenAsk
    {
        bool testToolsOn = false;
        std::uint32_t map = 0;        // where the player (and the warden) stand
        std::uint8_t level = 1;
        std::uint8_t maxLevel = 60;   // the realm's MaxPlayerLevel
        bool inCombat = false;
    };

    // The options, in menu order. Nothing at all with the test tools off.
    inline std::vector<WardenOption> WardenOptions(WardenAsk const& a)
    {
        std::vector<WardenOption> out;
        if (!a.testToolsOn || a.inCombat)
            return out;
        std::uint8_t const target = a.maxLevel < kTestLevel ? a.maxLevel : kTestLevel;
        if (a.map == kBarrowsMap)
        {
            out.push_back(WardenOption::ToMillbrook);
            out.push_back(WardenOption::ToDoor);
            if (a.level < target)
                out.push_back(WardenOption::Level60);
            return out;
        }
        out.push_back(WardenOption::ToBarrows);
        if (a.level < target)
            out.push_back(WardenOption::Level60);
        out.push_back(WardenOption::ToDoor);
        out.push_back(WardenOption::ResetBarrows);
        return out;
    }

    inline bool Offers(WardenAsk const& a, WardenOption o)
    {
        for (WardenOption x : WardenOptions(a))
            if (x == o)
                return true;
        return false;
    }

    inline char const* WardenOptionText(WardenOption o)
    {
        switch (o)
        {
            case WardenOption::ToBarrows: return "Take me to the Barrows of Cairnmoor.";
            case WardenOption::Level60: return "Make me level 60 (test realm).";
            case WardenOption::ResetBarrows: return "Reset the Barrows of Cairnmoor for me (leave your group first).";
            case WardenOption::ToMillbrook: return "Take me back to Millbrook.";
            case WardenOption::ToDoor: return "Take me to the mound door on Cairnmoor.";
        }
        return "";
    }

    // --- the seals ----------------------------------------------------------------------------------------------------
    // The barrows' bosses report their deaths through their scripts (emberfall_barrows_bosses.cpp, SetData): the three
    // barrow bosses break a seal each (data 1-3); when all three are broken the First Interred wakes (selectable, no longer
    // immune) and its honour guard comes up the vault stair. 4 = the First Interred, 5 = the Post-Keeper.
    enum BarrowsData : std::uint32_t
    {
        DATA_SEAL_KEENING = 1,
        DATA_SEAL_BROOD = 2,
        DATA_SEAL_PYRE = 3,
        DATA_FIRST_INTERRED = 4,
        DATA_POST_KEEPER = 5,
        DATA_SEALS_BROKEN = 10,   // GetData: how many seals are broken (0-3)
    };
    constexpr std::uint32_t kEncounterDone = 3;   // the core's EncounterState DONE

    inline std::uint32_t SealBit(std::uint32_t type)
    {
        return type >= DATA_SEAL_KEENING && type <= DATA_SEAL_PYRE ? 1u << (type - DATA_SEAL_KEENING) : 0u;
    }
    inline bool AllSealsBroken(std::uint32_t mask) { return (mask & 7u) == 7u; }
    inline std::uint32_t SealsBroken(std::uint32_t mask) { return (mask & 1u) + ((mask >> 1) & 1u) + ((mask >> 2) & 1u); }

    // What breaking `type` does to `mask`: the new mask and whether this break is the one that wakes the First Interred.
    struct SealResult
    {
        std::uint32_t mask;
        bool wakes;
    };
    inline SealResult BreakSeal(std::uint32_t mask, std::uint32_t type, std::uint32_t data)
    {
        if (data != kEncounterDone || !SealBit(type))
            return { mask, false };
        std::uint32_t const next = mask | SealBit(type);
        return { next, AllSealsBroken(next) && !AllSealsBroken(mask) };
    }

    // The honour guard's arrival points: the south end of the Honour Hall, at the foot of the vault stair (layout.yaml,
    // design (235-255, 126), floor Z 15).
    constexpr Spot kHonourGuard1{ kBarrowsMap, 387.33f, 278.33f, 15.2f, 3.14159f };
    constexpr Spot kHonourGuard2{ kBarrowsMap, 387.33f, 268.33f, 15.2f, 3.14159f };
    constexpr Spot kHonourGuard3{ kBarrowsMap, 387.33f, 258.33f, 15.2f, 3.14159f };
}

#endif
