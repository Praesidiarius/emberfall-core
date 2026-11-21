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

// Standalone tests of the Pyromancer's DEV handoff rules (2026-11-15, revised 2026-11-20; src/classkit/EfHeatLogic.h with
// the shared EfLustLogic.h SpendCutMs and EfClassKitLogic.h TierReached / RampTick / PowerGain / BuildCount;
// tests/run_tests.sh builds and runs them), grouped by the revision's "high-priority tests" (its §6, numbered 1-8):
// 1 Flashfire's Heat on a successful cast hit or miss, none without one, once a cast; 2 its empowerment; 3 Solar Flare's
// zero splash, Supernova's 50 % and Concentrated Supernova's single 150 % hit; 4 the Heat economy and Phoenix's Fury's
// cut for the Heat really spent, in steps of 10 with the remainder carried; 5 Sunlance's ticks and its doubled last
// tick, Endless Inferno; 6 Burning Chains once per enemy per sigil; 7 Eternal Ember at 20, 50, 70 and 100 and
// Overheat's reset; 8 the tree is the game repo's (tools/py/tests/test_talents*.py, the compiler's rules). The numbers
// are examples fed to the functions, not the kit's values (those: the game repo's content/data/classes/mage.yaml).
// Solar Terror's cower is a hold (mod_stun with the fear mechanic), never a fear that flees: data, checked by the game
// repo's tools/py/tests/test_classkit_pyromancer.py.

#include "EfClassKitLogic.h"
#include "EfHeatLogic.h"
#include "EfLustLogic.h"

#include <iostream>
#include <string>

using namespace Emberfall::ClassKit;

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
    // Test 1: Flashfire builds on a successful cast, hit or miss (the cast starts the fight), nothing without one, and once
    // a cast however many enemies it strikes (Forked Flashfire's bolts are another spell with no build of their own)
    {
        auto build = [](int32_t value, uint32_t struck, uint32_t reached, bool inFight)
        { return Logic::PowerGain(value, 0, Logic::BuildCount(true, struck, reached, inFight), true, false); };
        check(build(30, 1, 1, true) == 30, "1: a Flashfire that hits: 30 Heat");
        check(build(30, 0, 1, true) == 30, "1: a Flashfire that misses, is dodged or resisted: 30 Heat all the same");
        check(build(40, 0, 1, true) == 40, "1: Empowered Flashfire 2/2 (40), hit or miss");
        check(build(30, 0, 0, true) == 0, "1: at an evading enemy (reached nobody, no fight): nothing");
        check(build(30, 1, 1, false) == 0, "1: a cast that left its caster out of a fight (failed, cancelled): no Heat");
        check(build(30, 4, 4, true) == 30, "1: one gain a cast, also when four enemies were struck (no misc B: once)");
        check(Logic::BuildCount(false, 0, 1, true) == 0 && Logic::BuildCount(false, 2, 3, true) == 2,
              "1: without hit_or_miss: the enemies struck, never those only reached");
        check(Logic::PowerGain(10, 0, Logic::BuildCount(false, 0, 1, true), true, false) == 0, "1: a missed Cinderbolt: nothing");
        // no ceiling out of a fight (Flashpoint's 30 of feedback #592 is gone): four pulls opened with Flashfire
        int32_t heat = 0;
        for (int press = 0; press < 4; ++press)
        {
            heat += 10 * build(30, 1, 1, true);
            heat = std::max(0, heat - 100);    // walking 10 s between pulls: the drain of 1 a second
        }
        check(heat == 800, "1: four pulls opened with Flashfire: 30 each, the slow drain between them (80 left)");
    }
    // Test 2: Flashfire's empowerment: one charge for the next Heat spender, 30 %, 40 % with Potent Flashfire at any Heat
    {
        check(Logic::Empowered(1000, 30) == 1300, "2: the empowered spender: 30 % more");
        check(Logic::Empowered(1000, 30 + 10) == 1400, "2: Potent Flashfire: 40 % more, whatever the bar holds");
        check(Logic::Empowered(1000, 0) == 1000, "2: no empowerment (spent, or Flashfire's own hit): as it is");
    }
    // Test 3: Solar Flare strikes one enemy; Supernova (its talent, 100+ before the cost) hits the target as Solar Flare
    // does and 50 % of that line on the others (the splash leaves the target out); Concentrated Supernova one hit at 150 %
    {
        check(Heat::ReachedBeforeCost(100, 100) && !Heat::ReachedBeforeCost(100 - 50, 100),
              "3: Solar Flare at 100: Supernova, though the bar after is 50");
        check(!Heat::ReachedBeforeCost(99, 100) && !Logic::TierReached(990, 100, 10), "3: 99 Heat: no Supernova");
        check(Logic::PctOf(1000, 50) == 500, "3: Supernova's other enemies: 50 % of the line");
        check(Logic::Empowered(1000, 50) == 1500, "3: Concentrated Supernova: 150 % of Supernova's direct hit, one hit");
        check(Logic::Empowered(Logic::Empowered(1000, 50), 10) == 1650, "3: with Empowered Solar Flare 2/2 on top: 165 %");
    }
    // Test 4: the Heat economy. Falling Sun consumes all once its cost was paid (nothing in Overheat); Phoenix's Fury's
    // cut (Undying Flame: -1000 ms a point in steps of 10, the remainder carried; free casts in Overheat spend nothing)
    {
        check(Heat::SpendAllExtra(1000, 370) == 370, "4: Falling Sun at 137 Heat: the 37 beyond the 100 go too");
        check(Heat::SpendAllExtra(1000, 0) == 0, "4: exactly 100: nothing more");
        check(Heat::SpendAllExtra(0, 1500) == 0, "4: in Overheat (cost 0): the bar stays");
        auto cut = [](int32_t points, int32_t& carry)
        {
            Heat::StepCount const c = Heat::StepPoints(points, carry, 10);
            carry = c.carry;
            return LustLogic::SpendCutMs(c.points, -1000);
        };
        int32_t carry = 0;
        check(cut(50, carry) == 50000 && carry == 0, "4: a 50-Heat Solar Flare: 50 s off");
        check(cut(40, carry) == 40000 && carry == 0, "4: with Solar Efficiency (40): 40 s");
        check(cut(70, carry) == 70000, "4: a 70-Heat Sunlance: 70 s off");
        check(cut(Heat::PointsSpent(100, 37), carry) == 130000 && carry == 7, "4: Falling Sun at 137: 130 s, 7 carried");
        check(cut(Heat::PointsSpent(0, 0), carry) == 0 && carry == 7, "4: a free cast in Overheat: nothing, the 7 kept");
        check(cut(50, carry) == 50000 && carry == 7, "4: the next 50: 50 s, still 7 over");
        check(cut(3, carry) == 10000 && carry == 0, "4: 3 more make the carried 7 a whole 10");
        check(Heat::StepPoints(137, 0, 1).points == 137 && Heat::StepPoints(137, 0, 1).carry == 0,
              "4: a rule without steps (the Barbarian's): every point, nothing carried");
        check(Logic::CooldownCut(LustLogic::SpendCutMs(50, -1000), 25000) == 25000, "4: never more than the cooldown left");
    }
    // Test 5: Sunlance: each tick 10 % more than the last, Final Incineration doubles the final tick only; Endless
    // Inferno +4 s for each channel run to its last tick in Overheat, no cap (10 s Overheat: about 14 s with one)
    {
        check(Logic::RampTick(100, 10, 6, true, 100) == 2 * Logic::RampTick(100, 10, 6, false, 0), "5: the final tick doubled");
        check(Logic::RampTick(100, 10, 5, false, 100) == Logic::RampTick(100, 10, 5, false, 0), "5: the others unchanged");
        check(Heat::LastTick(6, 6) && !Heat::LastTick(5, 6), "5: only the last of six ticks counts");
        Heat::Lengthened l = Heat::Lengthen(7000, 10000, 4000);
        check(l.leftMs == 11000 && l.maxMs == 14000, "5: a full Sunlance 3 s into a 10 s Overheat: 14 s in all");
        l = Heat::Lengthen(l.leftMs, l.maxMs, 4000);
        l = Heat::Lengthen(l.leftMs, l.maxMs, 4000);
        check(l.maxMs == 22000, "5: three channels, three extensions: no cap");
        check(Heat::Lengthen(0, 10000, 4000).leftMs == 0, "5: Overheat already gone: nothing");
        check(Heat::ReachedBeforeCost(100, 100) && Logic::TierReached(1000, 100, 10), "5: Sunlance at 100 (1000 stored): Sunpiercer");
    }
    // Test 6: Burning Sigil's bonuses set at placement (Inferno Sigil +20 % at any Heat) and Burning Chains' root once per
    // enemy per sigil
    {
        Heat::FieldSnapshot sigil;
        sigil.pct = 20;                    // Inferno Sigil
        sigil.triggers.push_back(94420);   // Burning Chains' root
        int32_t const tick = Logic::Empowered(100, sigil.pct);
        check(tick == 120, "6: Inferno Sigil: every tick 20 % more");
        check(Heat::FirstReach(sigil, 7) && !Heat::FirstReach(sigil, 7), "6: an enemy is rooted once per sigil");
        check(Heat::FirstReach(sigil, 8), "6: each enemy once");
        Heat::FieldSnapshot next;          // the next sigil, 25 s later
        check(Heat::FirstReach(next, 7), "6: the next sigil may root the same enemy once more");
        check(Heat::FieldInstance(false, 0, false, 0) == 0, "6: one sigil at a time: its Heat in the first instance");
    }
    // Test 7: Eternal Ember: out of a fight Heat never cools at or below 70; above it, down to 70 (the corrected rule);
    // without it, to 0. Overheat's end sets the bar to 0 (an explicit effect), and 0 stays 0
    {
        int32_t const floor = 700;         // 70 Heat stored
        check(Heat::CooledTo(200, floor) == 200, "7: left the fight at 20: stays 20");
        check(Heat::CooledTo(500, floor) == 500, "7: left at 50: stays 50");
        check(Heat::CooledTo(700, floor) == 700, "7: left at 70: stays 70");
        check(Heat::CooledTo(1000, floor) == 700, "7: left at 100: cools to 70");
        check(Heat::CooledTo(1500, floor) == 700, "7: left at 150: cools to 70");
        check(Heat::CooledTo(0, floor) == 0, "7: left at 0 (or Overheat's reset to 0): stays 0");
        check(Heat::CooledTo(1000, 0) == 0, "7: without the capstone: to 0");
        check(Heat::DrainAboveFloor(1200, 10, floor) == 10, "7: above 70 it cools at the normal rate");
        check(Heat::DrainAboveFloor(705, 10, floor) == 5, "7: never below 70");
        check(Heat::DrainAboveFloor(700, 10, floor) == 0 && Heat::DrainAboveFloor(200, 10, floor) == 0,
              "7: at or below 70 nothing cools (not only after a fight that ended at 70+)");
        check(Heat::DrainAboveFloor(200, 10, 0) == 10, "7: without the capstone, 20 cools");
        check(Heat::DrainAboveFloor(0, 10, floor) == 0, "7: Overheat's reset to 0 first: it stays 0");
    }

    std::cout << "classkit heat tests: " << (g_checks - g_failed) << "/" << g_checks << " passed\n";
    return g_failed ? 1 : 0;
}
