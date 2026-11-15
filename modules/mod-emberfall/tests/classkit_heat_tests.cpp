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
// Sigils' own instances, Eternal Ember's floor, Endless Inferno's uncapped lengthening and Final Incineration's tick. The
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
    // Flashpoint out of a fight (feedback #592, Nathanael 2026-11-15): Heat to 30 at most (300 stored, ooc_max 30 x 10);
    // pressed every 10 s while walking (the bar drains 1 a second: 100 stored between presses) it never passes 30
    {
        int32_t const ceiling = 30 * 10;
        int32_t heat = 0, most = 0;
        for (int press = 0; press < 4; ++press)
        {
            heat += Logic::GainBelowCeiling(300, heat, ceiling);
            most = std::max(most, heat);
            heat = std::max(0, heat - 100);
        }
        check(most == 300, "four presses out of a fight: Heat ends at 30, never above");
        check(Logic::GainBelowCeiling(300, 0, ceiling) == 300, "from empty: the opener's full 30");
        check(Logic::GainBelowCeiling(400, 0, ceiling) == 300, "Empowered Flashpoint's 40: still 30 at most");
        check(Logic::GainBelowCeiling(300, 250, ceiling) == 50, "at 25: up to 30");
        check(Logic::GainBelowCeiling(300, 300, ceiling) == 0 && Logic::GainBelowCeiling(300, 900, ceiling) == 0,
              "at 30 or more: no Heat (the charge and the cooldown are the spell's own, unchanged)");
        // in a fight the module passes no ceiling: 30 each press, up to the bar's maximum
        int32_t fight = 0;
        for (int press = 0; press < 4; ++press)
            fight += Logic::GainBelowCeiling(300, fight, 0);
        check(fight == 1200, "in a fight: 30 a press, four presses 120");
        check(Logic::GainBelowCeiling(0, 0, ceiling) == 0 && Logic::GainBelowCeiling(-5, 0, 0) == 0, "nothing to give: nothing");
    }
    // Final Incineration: the final tick 100 % more, the rest as they were
    {
        check(Logic::RampTick(100, 10, 6, true, 100) == 2 * Logic::RampTick(100, 10, 6, false, 0), "the final tick doubled");
        check(Logic::RampTick(100, 10, 5, false, 100) == Logic::RampTick(100, 10, 5, false, 0), "the others unchanged");
    }

    std::cout << "classkit heat tests: " << (g_checks - g_failed) << "/" << g_checks << " passed\n";
    return g_failed ? 1 : 0;
}
