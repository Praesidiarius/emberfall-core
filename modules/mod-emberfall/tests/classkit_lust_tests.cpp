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

// Standalone tests of the Barbarian's arithmetic (src/classkit/EfLustLogic.h; tests/run_tests.sh builds and runs them):
// stacks with a timer each, the heal from the hardest hits, the damage by the number of enemies, a builder's percent;
// the finalized tree (2026-10-29): the overheal shield, the capped gain, the linear count scale, the strongest ward, the
// spend cut.
// The numbers are ankarna's examples (docs/design/classes/warrior-barbarian.md), fed to the functions.

#include "EfLustLogic.h"

#include <iostream>
#include <string>

using namespace Emberfall::ClassKit::LustLogic;

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
}

int main()
{
    // timed stacks: each its own 8 s, never refreshed by new ones, the shortest make room at 5
    std::vector<uint64_t> s;
    check(AddStacks(s, 1000, 1, 8000, 5) == 1, "Savage Cleave: 1 stack");
    check(AddStacks(s, 2000, 2, 8000, 5) == 3, "Bonecrusher: 3 stacks");
    check(LongestLeft(s, 2000) == 8000, "the aura lasts until the newest runs out");
    check(PruneStacks(s, 9000) == 2, "the first ran out at 9 s, the others stay");
    check(AddStacks(s, 9500, 3, 8000, 5) == 5, "Worldshaker: 2 + 3 = 5");
    check(AddStacks(s, 9600, 2, 8000, 5) == 5, "at 5 the two with the least time left make room");
    check(s.front() == 17500, "the oldest left is one of the Worldshaker stacks");
    check(PruneStacks(s, 17500) == 2, "three ran out together, two stay");
    check(PruneStacks(s, 17600) == 0 && LongestLeft(s, 17600) == 0, "all gone");
    check(AddStacks(s, 0, 7, 8000, 5) == 5, "more than the cap at once: 5");
    check(AddStacks(s, 0, 1, 8000, 0) == 5 && AddStacks(s, 0, 1, 0, 5) == 5, "no cap or no duration: nothing added");

    // the heal: 30 % of the 5 enemies that took the most, crits in; nothing struck, nothing healed
    check(TopHeal({ 100, 300, 50, 200, 400, 10, 250 }, 5, 30) == 375, "top 5 of 7: 1250 x 30 %");
    check(TopHeal({ 100, 200 }, 5, 30) == 90, "fewer than 5: all");
    check(TopHeal({}, 5, 30) == 0, "no enemies: no heal");
    check(TopHeal({ 100, -5 }, 5, 30) == 30, "nothing negative counts");

    // Ruinous Cleave's table
    std::vector<int32_t> const ruin = { 250, 200, 150, 120, 100 };
    check(CountScalePct(1, ruin) == 250 && CountScalePct(2, ruin) == 200 && CountScalePct(3, ruin) == 150, "1-3 enemies");
    check(CountScalePct(4, ruin) == 120 && CountScalePct(5, ruin) == 100 && CountScalePct(9, ruin) == 100, "4, 5+");
    check(CountScalePct(0, ruin) == 250, "none counted: as one");
    check(CountScalePct(3, {}) == 100, "no table: unchanged");

    // Primal Fury: +50 % from attacks
    check(BuildPct(20, 50) == 30 && BuildPct(30, 50) == 45 && BuildPct(50, 50) == 75, "ankarna's table");
    check(BuildPct(20, 0) == 20 && BuildPct(0, 50) == 0, "no bonus, no gain");

    // Overflowing Fury: overheal into a shield, at most 30 % of max health
    check(OverhealShield(200, 50, 0, 300) == 150, "150 overheal from nothing");
    check(OverhealShield(200, 50, 100, 300) == 250, "on top of the shield left");
    check(OverhealShield(200, 50, 250, 300) == 300, "capped at 30 %");
    check(OverhealShield(200, 200, 100, 300) == 0 && OverhealShield(200, 400, 0, 300) == 0, "no overheal: nothing");
    check(OverhealShield(200, 0, 0, 300) == 200, "at full health: the whole heal");

    // Vengeful Fury: 5 a retaliation, 5 a second at most
    uint64_t win = 0; int32_t got = 0;
    check(CappedGain(win, got, 1000, 5, 5) == 5, "the first retaliation: 5");
    check(CappedGain(win, got, 1400, 5, 5) == 0, "the second in that second: none");
    check(CappedGain(win, got, 2000, 5, 5) == 5, "a new second: 5");
    check(CappedGain(win, got, 2100, 3, 0) == 3, "no cap: all");
    win = 0; got = 0;
    check(CappedGain(win, got, 0, 3, 5) == 3 && CappedGain(win, got, 10, 3, 5) == 2, "3 + 2 = the 5 cap");

    // Focused Devastation: 250 % on one, 100 % from 5, linear
    check(LinearCountPct(1, 250, 5) == 250 && LinearCountPct(2, 250, 5) == 212 && LinearCountPct(3, 250, 5) == 175,
          "1, 2, 3 enemies");
    check(LinearCountPct(4, 250, 5) == 137 && LinearCountPct(5, 250, 5) == 100 && LinearCountPct(9, 250, 5) == 100,
          "4, 5+");
    check(LinearCountPct(0, 250, 5) == 250 && LinearCountPct(3, 250, 1) == 100, "none counted; no range");

    check(AtFullStacks(5, 5) && !AtFullStacks(4, 5) && !AtFullStacks(5, 0), "Savage Juggernaut at 5 stacks");

    // the strongest reduction only: Crippling Blow 10 %, Bloodbound Warlord 15 %
    check(StrongestWard({ 10, 15 }) == 15 && StrongestWard({ 10 }) == 10 && StrongestWard({}) == 0, "strongest");
    check(StrongestWard({ 150 }) == 100, "never above 100");
    check(Warded(1000, 15) == 850 && Warded(1000, 0) == 1000 && Warded(1000, 120) == 0 && Warded(-5, 10) == 0, "warded");

    // Bloodthirsty Berserker: 0.1 s a point
    check(SpendCutMs(30, -100) == 3000 && SpendCutMs(70, -100) == 7000 && SpendCutMs(100, -100) == 10000, "examples");
    check(SpendCutMs(0, -100) == 0 && SpendCutMs(30, 0) == 0, "nothing spent or no rule");

    std::cout << "classkit lust tests: " << (g_checks - g_failed) << "/" << g_checks << " passed\n";
    return g_failed ? 1 : 0;
}
