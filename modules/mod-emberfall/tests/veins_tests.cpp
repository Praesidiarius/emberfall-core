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
// forever), a gathering cast at a node as it stands (gone, going, the last swing, another's swing), and the merge of a
// swing's stacks of the same ore.

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

    NodeState Node(bool spawned, bool goingAway, bool vein, bool swingOut, bool casters, uint32_t before, uint32_t total)
    {
        NodeState n;
        n.spawned = spawned;
        n.goingAway = goingAway;
        n.vein = vein;
        n.swingOut = swingOut;
        n.swingIsCasters = casters;
        n.swingsBefore = before;
        n.total = total;
        return n;
    }

    void Casts()
    {
        check(CheckCastAt(Node(true, false, false, false, false, 0, 0)) == CastAt::Ok, "an herb or chest that is there");
        check(CheckCastAt(Node(true, false, true, false, false, 0, 3)) == CastAt::Ok, "a ready vein");
        check(CheckCastAt(Node(false, false, false, false, false, 0, 0)) == CastAt::Gone, "a despawned herb: gone");
        check(CheckCastAt(Node(false, false, true, false, false, 2, 3)) == CastAt::Gone, "a despawned vein: gone");
        check(CheckCastAt(Node(true, true, false, false, false, 0, 0)) == CastAt::Gone,
              "an herb whose load was just taken (it goes at the next update): gone");
        check(CheckCastAt(Node(true, true, true, false, false, 3, 3)) == CastAt::Gone, "a vein just spent: gone");
        check(CheckCastAt(Node(true, false, true, true, true, 0, 3)) == CastAt::Ok,
              "the next swing while the own window of the first of three is open");
        check(CheckCastAt(Node(true, false, true, true, true, 1, 3)) == CastAt::Ok,
              "the next swing while the own window of the second of three is open");
        check(CheckCastAt(Node(true, false, true, true, true, 2, 3)) == CastAt::Gone,
              "the own window of the last swing is open: nothing comes after it");
        check(CheckCastAt(Node(true, false, true, true, true, 1, 2)) == CastAt::Gone, "the last of two in the own window");
        check(CheckCastAt(Node(true, false, true, true, false, 0, 3)) == CastAt::InUse, "another's swing out: theirs first");
        check(CheckCastAt(Node(true, false, true, true, false, 2, 3)) == CastAt::InUse,
              "another's last swing out: in use (it goes when they close it)");
        check(CheckCastAt(Node(true, false, true, false, false, 2, 3)) == CastAt::Ok,
              "a swing left with loot in it, nobody's window open: anyone takes the rest");
        // every vein of every size: while a swing's own window is open, a click is refused exactly at the last swing
        for (uint32_t total = 1; total <= 5; ++total)
            for (uint32_t before = 0; before < total; ++before)
            {
                bool const last = AfterRelease(true, before, total) == Release::Spent;
                check((CheckCastAt(Node(true, false, true, true, true, before, total)) == CastAt::Gone) == last,
                      "the own window refuses a click exactly when its release spends the vein (" +
                          std::to_string(before + 1) + " of " + std::to_string(total) + ")");
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
    Casts();
    Merges();
    std::cout << "veins: " << (g_checks - g_failed) << "/" << g_checks << " checks passed\n";
    return g_failed ? 1 : 0;
}
