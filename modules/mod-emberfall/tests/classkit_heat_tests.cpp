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

// Standalone tests of the Pyromancer's DEV handoff rules (2026-11-15; src/classkit/EfHeatLogic.h with the shared
// EfLustLogic.h SpendCutMs and EfClassKitLogic.h TierReached / RampTick; tests/run_tests.sh builds and runs them):
// Undying Flame's cut for the Heat really spent (no remainder lost, nothing for Overheat's free casts), thresholds read
// before the cost, Falling Sun's "consumes all", the field's snapshot at placement, Burning Chains once per enemy, Twin
// Sigils' own instances, Eternal Ember's floor, Endless Inferno's uncapped lengthening and Final Incineration's tick;
// Flashfire (ankarna's notes of 2026-11-17): Heat hit or miss once the fight began, no ceiling out of combat, its
// talents' thresholds before its own Heat. The
// numbers are examples fed to the functions, not the kit's values (those: the game repo's content/data/classes/mage.yaml).
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
    // Undying Flame (spell_ef_kit_rule_spend_cut at -1000 ms a point): every Heat point really spent is a second off
    {
        check(LustLogic::SpendCutMs(40, -1000) == 40000, "a 40-Heat Solar Flare: 40 s off");
        check(LustLogic::SpendCutMs(70, -1000) == 70000, "a 70-Heat Sunlance: 70 s off");
        // Falling Sun at 137 Heat: its 100 and the 37 it consumes beyond them; the 7 below the next 10 count too
        check(LustLogic::SpendCutMs(Heat::PointsSpent(100, 37), -1000) == 137000, "137 Heat: 137 s, no remainder lost");
        check(LustLogic::SpendCutMs(Heat::PointsSpent(0, 0), -1000) == 0, "a free cast in Overheat: nothing");
        check(LustLogic::SpendCutMs(7, -1000) + LustLogic::SpendCutMs(3, -1000) == LustLogic::SpendCutMs(10, -1000),
              "7 then 3 Heat: as much as 10 at once");
        check(Logic::CooldownCut(LustLogic::SpendCutMs(40, -1000), 25000) == 25000, "never more than the cooldown left");
    }
    // thresholds before the cost: Supernova and Sunpiercer at 100, a cast that drops the bar below still counts
    {
        check(Heat::ReachedBeforeCost(100, 100) && !Heat::ReachedBeforeCost(100 - 40, 100), "Solar Flare at 100: Supernova, the bar after is 60");
        check(Heat::ReachedBeforeCost(100, 100) && Logic::TierReached(1000, 100, 10), "Sunlance at 100 (1000 stored): Sunpiercer");
        check(!Heat::ReachedBeforeCost(99, 100) && !Logic::TierReached(990, 100, 10), "99 Heat: no transformation");
        check(Heat::ReachedBeforeCost(70, 70) && Heat::ReachedBeforeCost(70, 30), "a cast at 70: Blazing and Inferno talents both");
    }
    // Falling Sun consumes all once its cost was paid; in Overheat it paid nothing and takes nothing
    {
        check(Heat::SpendAllExtra(1000, 370) == 370, "137 Heat: the 37 beyond the 100 go too");
        check(Heat::SpendAllExtra(1000, 0) == 0, "exactly 100: nothing more");
        check(Heat::SpendAllExtra(0, 1500) == 0, "in Overheat (cost 0): the bar stays");
    }
    // the field's snapshot at placement and Burning Chains once per enemy per field
    {
        Heat::FieldSnapshot sigil;
        sigil.pct = 20;                    // placed at 70+ with Inferno Sigil
        sigil.triggers.push_back(94420);   // placed at 30+ with Burning Chains
        int32_t const tick = Logic::Empowered(100, sigil.pct);
        check(tick == 120, "Inferno Sigil: every tick 20 % more");
        check(Logic::Empowered(100, sigil.pct) == tick, "the bar falling afterwards changes nothing (the snapshot is the field's)");
        check(Heat::FirstReach(sigil, 7) && !Heat::FirstReach(sigil, 7), "an enemy is rooted once per sigil");
        check(Heat::FirstReach(sigil, 8), "each enemy once");
        Heat::FieldSnapshot second;        // Twin Sigils: the second sigil roots again, on its own
        check(Heat::FirstReach(second, 7), "a second sigil may root the same enemy once more");
    }
    // Twin Sigils: each sigil's Heat is its own instance
    {
        check(Heat::FieldInstance(false, 0, false, 0) == 0, "the first sigil: the first instance");
        check(Heat::FieldInstance(true, 6000, false, 0) == 1, "a second while the first burns: the second instance");
        check(Heat::FieldInstance(false, 0, true, 3000) == 0, "the first ran out, the second burns: the first again");
        check(Heat::FieldInstance(true, 2000, true, 7000) == 0, "both burn: the one with less left is renewed");
    }
    // Eternal Ember: leaving a fight at 70+ the bar cools to 70 only; below 70 to 0
    {
        int32_t const floor = Heat::FloorAtFightEnd(1200, 700);
        check(floor == 700, "left the fight at 120: floor 70");
        check(Heat::DrainAboveFloor(1200, 10, floor) == 10, "above 70 it cools at the normal rate");
        check(Heat::DrainAboveFloor(705, 10, floor) == 5, "never below 70");
        check(Heat::DrainAboveFloor(700, 10, floor) == 0, "at 70 it stays");
        check(Heat::FloorAtFightEnd(690, 700) == 0 && Heat::DrainAboveFloor(690, 10, 0) == 10, "left at 69: it cools to 0");
        check(Heat::FloorAtFightEnd(1500, 0) == 0, "without the capstone: no floor");
        check(Heat::DrainAboveFloor(0, 10, floor) == 0, "Overheat's reset to 0 first: it stays 0");
    }
    // Endless Inferno: +4 s per full channel, no cap
    {
        Heat::Lengthened l = Heat::Lengthen(3000, 15000, 4000);
        check(l.leftMs == 7000 && l.maxMs == 19000, "a full Sunlance: 4 s more");
        l = Heat::Lengthen(l.leftMs, l.maxMs, 4000);
        l = Heat::Lengthen(l.leftMs, l.maxMs, 4000);
        check(l.maxMs == 27000, "three channels, three extensions: no cap");
        check(Heat::Lengthen(0, 15000, 4000).leftMs == 0, "Overheat already gone: nothing");
        check(Heat::LastTick(6, 6) && !Heat::LastTick(5, 6), "only the last of six ticks counts");
    }
    // Flashfire (ankarna 2026-11-17): Heat on a successful cast whether or not the attack hits; the cast starts the fight
    // (the core's hostile cast does, a miss or resist included, an evading enemy not), and without a fight no Heat
    {
        auto build = [](int32_t value, uint32_t struck, uint32_t reached, bool inFight)
        { return Logic::PowerGain(value, 0, Logic::BuildCount(true, struck, reached, inFight), true, false); };
        check(build(30, 1, 1, true) == 30, "a Flashfire that hits: 30 Heat");
        check(build(30, 0, 1, true) == 30, "a Flashfire that misses, is dodged or resisted: 30 Heat all the same");
        check(build(40, 0, 1, true) == 40, "Empowered Flashfire 2/2 (40), hit or miss");
        check(build(30, 0, 0, true) == 0, "at an evading enemy (reached nobody, no fight): nothing");
        check(build(30, 1, 1, false) == 0, "a cast that left its caster out of a fight: no Heat before the pull");
        // every other builder keeps the miss rules: only the enemies struck count
        check(Logic::BuildCount(false, 0, 1, true) == 0 && Logic::BuildCount(false, 2, 3, true) == 2,
              "without hit_or_miss: the enemies struck, never those only reached");
        check(Logic::PowerGain(10, 0, Logic::BuildCount(false, 0, 1, true), true, false) == 0, "a missed Cinderbolt: nothing");
    }
    // Heat neither stops nor is capped out of a fight any more (Flashpoint's ceiling of 30, feedback #592, is gone): a
    // Flashfire cast out of combat starts the fight and gives its whole Heat whatever the bar holds, up to the bar's top
    {
        int32_t heat = 0;
        for (int press = 0; press < 4; ++press)
        {
            heat += 10 * Logic::PowerGain(30, 0, Logic::BuildCount(true, 1, 1, true), true, false);
            heat = std::max(0, heat - 100);    // walking 10 s between pulls: the drain of 1 a second
        }
        check(heat == 800, "four pulls opened with Flashfire: 30 each, the slow drain between them (80 left)");
        check(Logic::PowerGain(30, 0, Logic::BuildCount(true, 0, 1, true), true, false) == 30,
              "at 30 Heat or more out of combat: still the full 30 (it gave nothing before)");
    }
    // Flashfire's talents read the bar as Flashfire is cast, before its own Heat (the stage auras follow the bar on the
    // player tick after the cast); its empowerment waits for the next Heat spender, one charge
    {
        check(Logic::TierReached(300, 30, 10) && !Logic::TierReached(290, 30, 10), "Blazing Flashfire: from 30 Heat");
        check(!Heat::ReachedBeforeCost(29, 30), "cast at 29 (59 after its own 30): no Blazing Flashfire");
        check(Logic::TierReached(700, 70, 10) && !Logic::TierReached(690, 70, 10), "Critical Ignition: from 70 Heat");
        check(!Heat::ReachedBeforeCost(69, 70) && Heat::ReachedBeforeCost(70, 70), "cast at 69: no Critical Ignition; at 70: yes");
        check(Logic::Empowered(1000, 30) == 1300, "the empowered spender: 30 % more");
        check(Logic::Empowered(1000, 30 + 20) == 1500, "with Blazing Flashfire at 30+: 50 % more");
        check(Logic::Empowered(1000, 0) == 1000, "no empowerment (spent, or Flashfire's own hit): as it is");
    }
    // Final Incineration: the final tick 100 % more, the rest as they were
    {
        check(Logic::RampTick(100, 10, 6, true, 100) == 2 * Logic::RampTick(100, 10, 6, false, 0), "the final tick doubled");
        check(Logic::RampTick(100, 10, 5, false, 100) == Logic::RampTick(100, 10, 5, false, 0), "the others unchanged");
    }

    std::cout << "classkit heat tests: " << (g_checks - g_failed) << "/" << g_checks << " passed\n";
    return g_failed ? 1 : 0;
}
