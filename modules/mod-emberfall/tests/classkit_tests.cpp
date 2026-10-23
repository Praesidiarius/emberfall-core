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

// Standalone tests of the class kits' self-stack arithmetic (src/classkit/EfClassKitLogic.h; tests/run_tests.sh builds
// and runs them): the Paladin's Sunblades cap (5, 8 under Crown of Blades), what each builder raises, that a fallen cap
// keeps the blades held, Endless Crown's lengthening and the per-blade refunds.

#include "EfClassKitLogic.h"

#include <iostream>
#include <string>

using namespace Emberfall::ClassKit::Logic;

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
    // the cap: effect 1's value within the aura's stack maximum (Sunblades: StackAmount 8)
    check(StackCap(5, 8) == 5, "Sunblades cap 5");
    check(StackCap(5 + 3, 8) == 8, "Crown of Blades: cap 8");
    check(StackCap(12, 8) == 8, "never above the stack maximum");
    check(StackCap(0, 8) == 8, "no value: the stack maximum");
    check(StackCap(3, 0) == 1, "a broken stack maximum still holds one");

    // builders
    check(BuilderGain(1, 0, 1, true) == 1, "Sacred Brand: +1");
    check(BuilderGain(2, 0, 1, true) == 2, "Sacred Brand under Crown: +2");
    check(BuilderGain(1, 0, 0, true) == 0, "a dodged Sacred Brand raises nothing");
    check(BuilderGain(1, 3, 1, true) == 1, "Sunwheel, one enemy: +1");
    check(BuilderGain(1, 3, 5, true) == 3, "Sunwheel, five enemies: at most 3");
    check(BuilderGain(2, 3, 5, true) == 4, "Sunwheel under Crown, five enemies: 3 + 1");
    check(BuilderGain(1, 3, 0, true) == 0, "Sunwheel striking nobody: nothing");
    check(BuilderGain(3, 0, 1, true) == 3, "Heavenfall with Marked by Heaven: 3");
    check(BuilderGain(2, 0, 0, false) == 2, "a builder without enemy effects always raises");

    // the stacks after a gain
    check(StacksAfter(4, 2, 5) == 5, "4 + 2 at cap 5: 5");
    check(StacksAfter(5, 1, 8) == 6, "Crown: 5 + 1 at cap 8: 6");
    check(StacksAfter(8, 1, 5) == 8, "Crown ended: the 8 held stay");
    check(StacksAfter(2, 0, 5) == 2, "no gain: unchanged");

    // Endless Crown: +2 s per spend at 5, at most 10 s beyond the 20 s (Gilded Crown 2: 26 s)
    check(LengthenBy(2000, 10000, 20000, 20000) == 2000, "first spend: +2 s");
    check(LengthenBy(2000, 10000, 29000, 20000) == 1000, "near the limit: the rest");
    check(LengthenBy(2000, 10000, 30000, 20000) == 0, "at the limit: nothing");
    check(LengthenBy(2000, 10000, 26000, 26000) == 2000, "the base duration carries Gilded Crown");

    // refunds: 1 % of max mana per blade, Ringing Steel 2: 3 %
    check(PctPerStack(4000, 1, 5) == 200, "5 blades: 5 % of 4000 mana");
    check(PctPerStack(4000, 3, 8) == 960, "Ringing Steel 2, 8 blades: 24 %");
    check(PctPerStack(4000, 0, 5) == 0, "no refund");
    check(PctPerStack(3000, 2, 5) == 300, "Wrath's Mercy 2: 10 % of max health");

    // the Druid's Static (a spec power): builders
    check(PowerGain(20, 0, 1, true, false) == 20, "Skyspark: +20");
    check(PowerGain(20, 0, 1, true, true) == 40, "First Flash: the first Skyspark +40");
    check(PowerGain(30, 0, 1, true, false) == 30, "Supercell: Skyspark +30");
    check(PowerGain(20, 0, 0, true, false) == 0, "a missed Skyspark builds nothing");
    check(PowerGain(8, 4, 1, true, false) == 8, "Hailwind, one enemy: 8");
    check(PowerGain(8, 4, 6, true, false) == 32, "Hailwind, six enemies: at most 4 x 8");
    check(PowerGain(12, 4, 6, true, false) == 48, "Hailwind Surge 2, six enemies: 48");
    check(PowerGain(0, 0, 1, true, false) == 0, "no value: nothing");

    // Heavy Rend: up to 40 more of what is held beyond the cost (runic power x 10)
    check(Overspend(40, 1000, 400, 10) == 40, "100 Static, Skyrend at 40: 40 more");
    check(Overspend(40, 650, 400, 10) == 25, "65 Static: 25 more");
    check(Overspend(40, 400, 400, 10) == 0, "exactly the cost: nothing more");
    check(Overspend(40, 0, 0, 10) == 0, "a free Skyrend (Charged Air) at 0: nothing");
    check(OverspendDamage(1000, 40, 15) == 1600, "40 more at 1.5 %: +60 %");
    check(OverspendDamage(1000, 0, 15) == 1000, "no overspend: the damage");

    // echoes: Overcast 50 %, Thunder Roll 25 %
    check(EchoDamage(510, 50) == 255, "Overcast: half");
    check(EchoDamage(3, 25) == 1, "at least 1");
    check(EchoDamage(0, 50) == 0, "no damage: no echo");

    // the drain: 10 a second (100 stored) after 5 s out of combat, Static Memory 2 a quarter of it
    int64_t carry = 0;
    check(Drain(100, 1.0f, 1000, 4000, 5000, carry) == 0, "within the delay: nothing");
    check(Drain(100, 1.0f, 1000, 6000, 5000, carry) == 100, "a second after the delay: 10 Static");
    carry = 0;
    check(Drain(100, 1.0f, 1000, 5500, 5000, carry) == 50, "only the part after the delay counts");
    carry = 0;
    int32_t sum = 0;
    for (int i = 1; i <= 10; ++i)
        sum += Drain(100, 0.25f, 100, 10000 + i * 100, 5000, carry);
    check(sum == 25, "Static Memory 2: a quarter, the fractions carried");
    // Brooding Sky: a larger pool drains in proportion (as long to empty from full); never slower than the spec's own
    check(DrainRate(100, 1000, 1000) == 100, "the spec's own pool: its own rate");
    check(DrainRate(100, 1500, 1000) == 150, "Brooding Sky 1: 150 of 100, 15 a second");
    check(DrainRate(100, 2000, 1000) == 200, "Brooding Sky 2: 200 of 100, 20 a second");
    check(DrainRate(100, 500, 1000) == 100, "a smaller pool keeps the spec's rate");
    check(DrainRate(100, 2000, 0) == 100, "no maximum: the rate as it is");

    // charges: Second Gust, 2
    check(SpendCharge(0, 2).recharging == 1 && SpendCharge(0, 2).left, "first cast: one recharges, one is left");
    check(SpendCharge(1, 2).recharging == 2 && !SpendCharge(1, 2).left, "second cast: both recharge, none left");
    check(SpendCharge(2, 2).recharging == 2 && !SpendCharge(2, 2).left, "never above the maximum");

    // the Warlock's Blood Price: 12 % of max health, at least 1, never the last point
    check(HealthPrice(2000, 12) == 240, "Blood Price: 12 % of 2000");
    check(HealthPrice(2000, 8) == 160, "Thinned Blood 2: 8 %");
    check(HealthPrice(5, 12) == 1, "at least 1");
    check(HealthPrice(2000, 0) == 0, "no price");
    check(CanPay(241, 240), "241 health pays 240");
    check(!CanPay(240, 240), "never the last point");

    // Overflow 15 %, Harvest Tide 40 %
    check(Transfused(200, 15) == 30, "Overflow: 15 % of 200");
    check(Transfused(6, 15) == 0, "below a point: nothing");
    check(Transfused(0, 40) == 0, "no damage: nothing");

    // the most injured ally
    {
        uint32_t const health[] = { 900, 300, 500, 1000 };
        uint32_t const max[] = { 1000, 1000, 600, 1000 };
        check(MostInjured(health, health + 4, max) == 1, "30 % is the most injured");
        uint32_t const full[] = { 1000, 600 };
        uint32_t const fullMax[] = { 1000, 600 };
        check(MostInjured(full, full + 2, fullMax) == -1, "everyone full: nobody");
    }

    // the stacked Frost Shell: 10 % of max health (1000 of 10000) per stack, at most 10 stacks
    check(ShellStacks(0, 1000, 10) == 0, "an empty shell shows no stacks");
    check(ShellStacks(3000, 1000, 10) == 3, "3 full stacks");
    check(ShellStacks(2400, 1000, 10) == 3, "a worn stack still shows (ceil)");
    check(ShellStacks(2001, 1000, 10) == 3, "one point into the third stack");
    check(ShellStacks(25000, 1000, 10) == 10, "never above the maximum");
    check(ShellAfterGain(0, 1000, 3, 10) == 3000, "3 stacks from nothing");
    check(ShellAfterGain(2400, 1000, 1, 10) == 3400, "a gain adds to a worn pool");
    check(ShellAfterGain(9500, 1000, 2, 10) == 10000, "at most 10 stacks' worth");
    check(ShellGain(2, 2, 1) == 1, "Icebreak on one enemy: 2 shattered, 1 shell");
    check(ShellGain(6, 2, 1) == 3, "Icebreak on three enemies: 3 shells");
    check(ShellGain(5 + 6, 2, 1) == 5, "Glacial Rupture 5 + 6 stacks: 11 / 2 = 5 (summed, rounded down)");
    check(ShellGain(1, 2, 1) == 0, "one stack gives no shell");
    check(ShellGain(4, 0, 1) == 0, "no stacks per shell: nothing");

    // Winter's Embrace: 0.5 s / 1 s per stack consumed off Hoarstorm, at most 90 s per cooldown
    check(StackCooldownCut(6, 1000, 0, 90000, 170000) == 6000, "rank 2, 6 stacks: 6 s");
    check(StackCooldownCut(6, 500, 0, 90000, 170000) == 3000, "rank 1, 6 stacks: 3 s");
    check(StackCooldownCut(10, 1000, 85000, 90000, 60000) == 5000, "the last 5 s of the 90");
    check(StackCooldownCut(10, 1000, 90000, 90000, 60000) == 0, "90 s taken: nothing more");
    check(StackCooldownCut(10, 1000, 0, 90000, 4000) == 4000, "never more than is left");
    check(StackCooldownCut(10, 1000, 0, 90000, 0) == 0, "not on cooldown: nothing");

    // the Wrathbringer's cards: Last Sentence +100 % at 20 % health or below, nothing at full health
    check(ExecuteBonusPct(100.0f, 100, 20) == 0, "Last Sentence at full health: no bonus");
    check(ExecuteBonusPct(60.0f, 100, 20) == 50, "at 60 %: half the bonus");
    check(ExecuteBonusPct(20.0f, 100, 20) == 100, "at 20 %: the full bonus");
    check(ExecuteBonusPct(5.0f, 100, 20) == 100, "below 20 %: still the full bonus");
    check(ExecuteBonusPct(50.0f, 0, 20) == 0, "no bonus value: nothing");
    check(PctOf(400, 50) == 200, "half the cost back on a kill");
    check(PctOf(0, 50) == 0, "a free cast refunds nothing");
    check(PctOf(300, 50) == 150, "Sacred Brand's splash: 50 % of the hit");
    // Solenne's Blessing: the main stat by class, doubled while Solenne's Wrath holds
    check(MainStat(2) == 0 && MainStat(1) == 0, "paladins and warriors: strength");
    check(MainStat(3) == 1, "hunters: agility");
    check(MainStat(8) == 3 && MainStat(9) == 3 && MainStat(11) == 3, "mages, warlocks, druids: intellect");
    check(Empowered(20, 100) == 40, "the Blessing doubled: 40 %");
    check(Empowered(20, 0) == 20, "not empowered: 20 %");
    // Crown of Blades: one sword per Sacred Brand, Edict or Oathblade; all of them for Solenne's Wrath
    check(LaunchCount(1, 3) == 1, "one sword at a time");
    check(LaunchCount(0, 3) == 3, "Solenne's Wrath launches all three");
    check(LaunchCount(1, 0) == 0, "no swords held: nothing");
    check(LaunchCount(2, 1) == 1, "never more than are held");
    // cooldown cuts: Oathbound Rush takes 3 s off Oathblade, never more than is left
    check(CooldownCut(3000, 5000) == 3000, "3 s off a 5 s wait");
    check(CooldownCut(3000, 1200) == 1200, "never more than is left");
    check(CooldownCut(3000, 0) == 0, "not on cooldown: nothing");

    // the Pyromancer's Heat: the tiers at 30 / 70 / 100 (runic power x 10)
    check(!TierReached(290, 30, 10) && TierReached(300, 30, 10), "Blazing from 30 Heat");
    check(TierReached(1000, 100, 10) && !TierReached(990, 100, 10), "Maximum at 100 only");
    check(!TierReached(1000, 0, 10), "no threshold: never");
    // Sunlance: six ticks, each 10 % more than the last; Blazing's final tick +25 %
    check(RampTick(100, 10, 1, false, 0) == 100, "first tick: the base");
    check(RampTick(100, 10, 2, false, 0) == 110, "second: +10 %");
    check(RampTick(100, 10, 6, false, 0) == 161, "sixth: 1.1^5");
    check(RampTick(100, 10, 6, true, 25) == 201, "sixth with Blazing: x 1.25");
    check(RampTick(0, 10, 3, false, 0) == 0, "no base: nothing");
    // Sunlance pierces: behind the target on the line, not before it, not off to the side
    check(BehindOnLine(0, 0, 10, 0, 15, 1, 20, 2.5f, 0), "5 yd behind, 1 yd off: struck");
    check(!BehindOnLine(0, 0, 10, 0, 5, 0, 20, 2.5f, 0), "between the caster and the target: not");
    check(!BehindOnLine(0, 0, 10, 0, 15, 4, 20, 2.5f, 0), "4 yd to the side: not");
    check(!BehindOnLine(0, 0, 10, 0, 35, 0, 20, 2.5f, 0), "25 yd behind (20 yd line): not");

    std::cout << "classkit tests: " << (g_checks - g_failed) << "/" << g_checks << " passed\n";
    return g_failed ? 1 : 0;
}
