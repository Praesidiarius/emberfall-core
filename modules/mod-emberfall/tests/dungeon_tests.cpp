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

// Standalone tests of the dungeon logic that needs no core (src/dungeons/EfDungeonLogic.h; tests/run_tests.sh builds and
// runs them): the dev warden's gating (Emberfall docs/design/dungeon-barrows.md §8: with the test tools off it never
// stays in the world and offers nothing, whatever the player), its menu, and the barrows' seals.

#include "EfDungeonLogic.h"

#include <iostream>
#include <string>

using namespace Emberfall::Dungeons;

namespace
{
    int g_failed = 0, g_checks = 0;

    void check(bool ok, std::string const& what)
    {
        ++g_checks;
        if (!ok)
        {
            ++g_failed;
            std::cerr << "FAIL: " << what << "\n";
        }
    }

    // The gating proof: tools off -> the warden leaves, no option for anybody anywhere, no option can be taken.
    void TestOffMeansNothing()
    {
        check(!WardenStays(false) && WardenStays(true), "the warden stays only with the test tools on");
        for (std::uint32_t map : { 0u, kBarrowsMap, 750u })
            for (int level : { 1, 10, 54, 55, 59, 60 })
                for (bool combat : { false, true })
                {
                    WardenAsk a;
                    a.testToolsOn = false;
                    a.map = map;
                    a.level = std::uint8_t(level);
                    a.inCombat = combat;
                    check(WardenOptions(a).empty(), "tools off: no options (map " + std::to_string(map) + ", level " +
                                                        std::to_string(level) + ")");
                    for (std::uint32_t o = 1; o <= 5; ++o)
                        check(!Offers(a, WardenOption(o)), "tools off: option " + std::to_string(o) + " cannot be taken");
                }
    }

    void TestMenu()
    {
        WardenAsk a;
        a.testToolsOn = true;
        a.map = 0;
        a.level = 12;
        auto opts = WardenOptions(a);
        check(opts.size() == 4 && opts[0] == WardenOption::ToBarrows && opts[1] == WardenOption::Level60 &&
                  opts[2] == WardenOption::ToDoor && opts[3] == WardenOption::ResetBarrows,
              "Millbrook at 12: barrows, level 60, the door, reset");
        a.level = 60;
        opts = WardenOptions(a);
        check(opts.size() == 3 && !Offers(a, WardenOption::Level60), "at 60: no level option");
        check(!Offers(a, WardenOption::ToMillbrook), "outside: no way back to Millbrook (you are there)");
        a.map = kBarrowsMap;
        opts = WardenOptions(a);
        check(opts.size() == 2 && opts[0] == WardenOption::ToMillbrook && opts[1] == WardenOption::ToDoor,
              "inside at 60: back to Millbrook, the door");
        check(!Offers(a, WardenOption::ToBarrows) && !Offers(a, WardenOption::ResetBarrows), "inside: no way in, no reset");
        a.level = 58;
        check(Offers(a, WardenOption::Level60), "inside at 58: level 60 offered");
        a.maxLevel = 58;
        check(!Offers(a, WardenOption::Level60), "a realm capped at 58: nothing above the cap");
        a.inCombat = true;
        check(WardenOptions(a).empty(), "in combat: nothing");
        for (std::uint32_t o = 1; o <= 5; ++o)
            check(std::string(WardenOptionText(WardenOption(o))).size() > 10, "option text " + std::to_string(o));
    }

    void TestSeals()
    {
        check(SealBit(DATA_SEAL_KEENING) == 1 && SealBit(DATA_SEAL_BROOD) == 2 && SealBit(DATA_SEAL_PYRE) == 4, "seal bits");
        check(SealBit(DATA_FIRST_INTERRED) == 0 && SealBit(DATA_POST_KEEPER) == 0 && SealBit(0) == 0, "no seal bit for others");
        std::uint32_t m = 0;
        SealResult r = BreakSeal(m, DATA_SEAL_BROOD, kEncounterDone);
        check(r.mask == 2 && !r.wakes, "first seal: no wake");
        r = BreakSeal(r.mask, DATA_SEAL_BROOD, kEncounterDone);
        check(r.mask == 2 && !r.wakes, "the same seal twice: nothing more");
        r = BreakSeal(r.mask, DATA_SEAL_PYRE, 1);
        check(r.mask == 2 && !r.wakes, "not DONE: no seal");
        r = BreakSeal(r.mask, DATA_SEAL_PYRE, kEncounterDone);
        check(r.mask == 6 && !r.wakes && SealsBroken(r.mask) == 2, "second seal");
        r = BreakSeal(r.mask, DATA_SEAL_KEENING, kEncounterDone);
        check(r.mask == 7 && r.wakes && AllSealsBroken(r.mask) && SealsBroken(r.mask) == 3, "third seal wakes the First Interred");
        r = BreakSeal(r.mask, DATA_SEAL_KEENING, kEncounterDone);
        check(r.mask == 7 && !r.wakes, "once awake, never woken again");
        // any order
        std::uint32_t const orders[6][3] = { { 1, 2, 3 }, { 1, 3, 2 }, { 2, 1, 3 }, { 2, 3, 1 }, { 3, 1, 2 }, { 3, 2, 1 } };
        for (auto const& o : orders)
        {
            std::uint32_t mask = 0;
            int wakes = 0;
            for (std::uint32_t t : o)
            {
                SealResult s = BreakSeal(mask, t, kEncounterDone);
                mask = s.mask;
                wakes += s.wakes ? 1 : 0;
            }
            check(mask == 7 && wakes == 1, "every order wakes it exactly once");
        }
    }

    void TestPlaces()
    {
        check(kBarrowsInside.map == kBarrowsMap && kBarrowsDoor.map == 0 && kMillbrook.map == 0, "the places' maps");
        check(kHonourGuard1.map == kBarrowsMap && kHonourGuard2.z == kHonourGuard3.z, "the honour guard on the hall floor");
    }
}

int main()
{
    TestOffMeansNothing();
    TestMenu();
    TestSeals();
    TestPlaces();
    std::cout << "dungeon tests: " << (g_checks - g_failed) << " of " << g_checks << " checks passed\n";
    return g_failed ? 1 : 0;
}
