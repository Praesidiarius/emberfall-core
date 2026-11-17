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

// Standalone tests of the veins worked in swings (src/veins/EfVeinSwingsLogic.h; tests/run_tests.sh builds and runs
// them): which chests are veins, the swings' range, what a release does (a vein always ends: no swing count keeps it
// forever), and the merge of a swing's stacks of the same ore.

#include "EfVeinSwingsLogic.h"

#include <iostream>
#include <string>

using namespace Emberfall::VeinSwings;

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

    uint32_t Twenty(uint32_t) { return 20; }

    void Veins()
    {
        check(!IsVein(0) && !IsVein(1), "a chest of max_opens 0 or 1 opens once (herbs, chests)");
        check(IsVein(2) && IsVein(4), "max_opens above one: a vein");
        Range r = Swings(2, 3);
        check(r.lo == 2 && r.hi == 3, "2-3 swings");
        r = Swings(0, 3);
        check(r.lo == 1 && r.hi == 3, "min_opens 0 counts as one");
        r = Swings(4, 2);
        check(r.lo == 4 && r.hi == 4, "a top under the bottom is the bottom");
    }

    void Releases()
    {
        check(AfterRelease(false, 0, 3) == Release::PassOn, "no swing's loot out (a despawn): the core goes on");
        check(AfterRelease(false, 1, 3) == Release::PassOn, "a window on loot already taken counts no swing");
        check(AfterRelease(true, 0, 3) == Release::Keep, "first of three: kept");
        check(AfterRelease(true, 1, 3) == Release::Keep, "second of three: kept");
        check(AfterRelease(true, 2, 3) == Release::Spent, "third of three: spent");
        check(AfterRelease(true, 1, 2) == Release::Spent, "second of two: spent");
        check(AfterRelease(true, 5, 3) == Release::Spent, "past its swings: spent");
        check(AfterRelease(true, 0, 0) == Release::Spent, "no total: spent at once (never kept forever)");
        // every vein of every range ends after at most its top swings, whatever happens between
        for (uint32_t lo = 0; lo <= 5; ++lo)
            for (uint32_t hi = 0; hi <= 5; ++hi)
            {
                Range const rr = Swings(lo, hi);
                for (uint32_t total = rr.lo; total <= rr.hi; ++total)
                {
                    uint32_t swings = 0;
                    while (swings < 100 && AfterRelease(true, swings, total) == Release::Keep)
                        ++swings;
                    check(swings + 1 == total, "a vein of " + std::to_string(total) + " swings ends at its last");
                }
            }
    }

    void Merges()
    {
        std::vector<Stack> s = {{90611, 0, 1}, {90611, 0, 1}};
        check(Merge(s, Twenty) == 1, "the sure ore and the second: one fold");
        check(s[0].count == 2 && s[1].looted && s[1].count == 0, "one stack of two");
        check(Merge(s, Twenty) == 0 && s[0].count == 2, "idempotent");

        s = {{90611, 0, 1}, {90612, 0, 1}};
        check(Merge(s, Twenty) == 0, "two ores stay two stacks");

        s = {{90611, 0, 1}, {90611, 0, 1}};
        s[1].plain = false;
        check(Merge(s, Twenty) == 0, "a quest or free-for-all stack is left alone");

        s = {{90611, 0, 19}, {90611, 0, 2}};
        check(Merge(s, Twenty) == 0, "never past a full stack");

        s = {{90611, 0, 1}, {90611, 0, 1}};
        s[0].looted = true;
        check(Merge(s, Twenty) == 0 && !s[1].looted, "a taken stack takes nothing");
    }
}

int main()
{
    Veins();
    Releases();
    Merges();
    std::cout << "veins: " << (g_checks - g_failed) << "/" << g_checks << " checks passed\n";
    return g_failed ? 1 : 0;
}
