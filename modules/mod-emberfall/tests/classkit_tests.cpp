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
// and runs them): the arithmetic of the kits' mechanics (caps, gains, refunds, drains, cuts, ticks, picks). The numbers
// are examples fed to the functions, not the kits' values: those live in the game repo's content/data/classes/*.yaml
// and its tests (tools/py/tests/test_class_invariants.py); each mechanic is checked once per branch.

#include "EfClassKitLogic.h"
#include "EfFlowLogic.h"
#include "EfSealLogic.h"

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
    // the cap: effect 1's value within the aura's stack maximum
    check(StackCap(5, 8) == 5, "the value below the stack maximum");
    check(StackCap(5 + 3, 8) == 8, "a raised value up to the stack maximum");
    check(StackCap(12, 8) == 8, "never above the stack maximum");
    check(StackCap(0, 8) == 8, "no value: the stack maximum");
    check(StackCap(3, 0) == 1, "a broken stack maximum still holds one");

    // builders
    check(BuilderGain(1, 0, 1, true) == 1, "Sacred Brand: +1");
    check(BuilderGain(1, 0, 0, true) == 0, "a dodged Sacred Brand raises nothing");
    check(BuilderGain(1, 3, 1, true) == 1, "Sunwheel, one enemy: +1");
    check(BuilderGain(1, 3, 5, true) == 3, "Sunwheel, five enemies: at most 3");
    check(BuilderGain(2, 3, 5, true) == 4, "Sunwheel under Crown, five enemies: 3 + 1");
    check(BuilderGain(1, 3, 0, true) == 0, "Sunwheel striking nobody: nothing");
    check(BuilderGain(2, 0, 0, false) == 2, "a builder without enemy effects always raises");

    // the stacks after a gain
    check(StacksAfter(4, 2, 5) == 5, "4 + 2 at cap 5: 5");
    check(StacksAfter(5, 1, 8) == 6, "Crown: 5 + 1 at cap 8: 6");
    check(StacksAfter(8, 1, 5) == 8, "Crown ended: the 8 held stay");
    check(StacksAfter(2, 0, 5) == 2, "no gain: unchanged");

    // lengthening: a step per spend, at most a limit beyond the base duration (which talents may raise)
    check(LengthenBy(2000, 10000, 20000, 20000) == 2000, "first spend: +2 s");
    check(LengthenBy(2000, 10000, 29000, 20000) == 1000, "near the limit: the rest");
    check(LengthenBy(2000, 10000, 30000, 20000) == 0, "at the limit: nothing");
    check(LengthenBy(2000, 10000, 26000, 26000) == 2000, "the base duration carries Gilded Crown");

    // refunds: a percent of a maximum per stack
    check(PctPerStack(4000, 1, 5) == 200, "5 blades: 5 % of 4000 mana");
    check(PctPerStack(4000, 0, 5) == 0, "no refund");

    // the Druid's Static (a spec power): builders
    check(PowerGain(20, 0, 1, true, false) == 20, "Skyspark: +20");
    check(PowerGain(20, 0, 1, true, true) == 40, "First Flash: the first Skyspark +40");
    check(PowerGain(20, 0, 0, true, false) == 0, "a missed Skyspark builds nothing");
    check(PowerGain(8, 4, 1, true, false) == 8, "Hailwind, one enemy: 8");
    check(PowerGain(8, 4, 6, true, false) == 32, "Hailwind, six enemies: at most 4 x 8");
    check(PowerGain(0, 0, 1, true, false) == 0, "no value: nothing");

    // Heavy Rend: up to 40 more of what is held beyond the cost (runic power x 10)
    check(Overspend(40, 1000, 400, 10) == 40, "100 Static, Skyrend at 40: 40 more");
    check(Overspend(40, 650, 400, 10) == 25, "65 Static: 25 more");
    check(Overspend(40, 400, 400, 10) == 0, "exactly the cost: nothing more");
    check(Overspend(40, 0, 0, 10) == 0, "a free Skyrend (Charged Air) at 0: nothing");
    check(OverspendDamage(1000, 40, 15) == 1600, "40 more at 1.5 %: +60 %");
    check(OverspendDamage(1000, 0, 15) == 1000, "no overspend: the damage");

    // echoes: a percent of the damage, at least 1
    check(EchoDamage(510, 50) == 255, "Overcast: half");
    check(EchoDamage(3, 25) == 1, "at least 1");
    check(EchoDamage(0, 50) == 0, "no damage: no echo");

    // the drain: a rate after a delay out of combat, a factor on it, the fractions carried
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

    // charges
    check(SpendCharge(0, 2).recharging == 1 && SpendCharge(0, 2).left, "first cast: one recharges, one is left");
    check(SpendCharge(1, 2).recharging == 2 && !SpendCharge(1, 2).left, "second cast: both recharge, none left");
    check(SpendCharge(2, 2).recharging == 2 && !SpendCharge(2, 2).left, "never above the maximum");

    // a health price: a percent of max health, at least 1, never the last point
    check(HealthPrice(2000, 12) == 240, "Blood Price: 12 % of 2000");
    check(HealthPrice(5, 12) == 1, "at least 1");
    check(HealthPrice(2000, 0) == 0, "no price");
    check(CanPay(241, 240), "241 health pays 240");
    check(!CanPay(240, 240), "never the last point");

    // transfusions: a percent, nothing below a point
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

    // the stacked shell: a pool of stacks' worth, the shown stacks rounded up, at most a maximum
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

    // a cut per stack consumed, at most a total per cooldown, never more than is left
    check(StackCooldownCut(6, 1000, 0, 90000, 170000) == 6000, "rank 2, 6 stacks: 6 s");
    check(StackCooldownCut(10, 1000, 85000, 90000, 60000) == 5000, "the last 5 s of the 90");
    check(StackCooldownCut(10, 1000, 90000, 90000, 60000) == 0, "90 s taken: nothing more");
    check(StackCooldownCut(10, 1000, 0, 90000, 4000) == 4000, "never more than is left");
    check(StackCooldownCut(10, 1000, 0, 90000, 0) == 0, "not on cooldown: nothing");

    // an execute bonus: the full bonus at or below the threshold, scaling down to nothing at full health
    check(ExecuteBonusPct(100.0f, 100, 20) == 0, "Last Sentence at full health: no bonus");
    check(ExecuteBonusPct(60.0f, 100, 20) == 50, "at 60 %: half the bonus");
    check(ExecuteBonusPct(20.0f, 100, 20) == 100, "at 20 %: the full bonus");
    check(ExecuteBonusPct(5.0f, 100, 20) == 100, "below 20 %: still the full bonus");
    check(ExecuteBonusPct(50.0f, 0, 20) == 0, "no bonus value: nothing");
    check(PctOf(400, 50) == 200, "half the cost back on a kill");
    check(PctOf(0, 50) == 0, "a free cast refunds nothing");
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
    // cooldown cuts: never more than is left
    check(CooldownCut(3000, 5000) == 3000, "3 s off a 5 s wait");
    check(CooldownCut(3000, 1200) == 1200, "never more than is left");
    check(CooldownCut(3000, 0) == 0, "not on cooldown: nothing");

    // a spec power's tiers (runic power stored x 10)
    check(!TierReached(290, 30, 10) && TierReached(300, 30, 10), "Blazing from 30 Heat");
    check(TierReached(1000, 100, 10) && !TierReached(990, 100, 10), "Maximum at 100 only");
    check(!TierReached(1000, 0, 10), "no threshold: never");
    // a ramping channel: each tick a percent more than the last; a bonus on the final tick
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
    // a class power's regeneration: a rate times haste and the regen-percent auras, the fractions carried
    int64_t focusCarry = 0;
    int32_t focus = 0;
    for (int i = 0; i < 10; ++i)
        focus += Regen(5.0f, 1.0f, 100, focusCarry);
    check(focus == 5, "Focus: 5 in a second, in tenths");
    focusCarry = 0;
    check(Regen(5.0f, 1.5f, 2000, focusCarry) == 15, "Held Breath: 50 % faster");
    focusCarry = 0;
    check(Regen(25.0f, 1.0f, 4000, focusCarry) == 100, "out of a fight: full in 4 s");
    check(Regen(0.0f, 1.0f, 1000, focusCarry) == 0, "no rate: nothing");
    check(HasteFactor(1.0f) == 1.0f, "no haste");
    check(HasteFactor(1.0f / 1.04f) > 1.039f && HasteFactor(1.0f / 1.04f) < 1.041f, "Keen Draw 2: 4 % faster");
    check(HasteFactor(0.0f) == 1.0f, "a bad field: no haste");
    // a percent more per stack
    check(StackedDamage(200, 10, 5) == 300, "5 Beads: half again");
    check(StackedDamage(200, 14, 5) == 340, "Patient Kill: 14 % a Bead");
    check(StackedDamage(200, 10, 0) == 200, "no Beads: the base");
    check(StackedDamage(0, 10, 5) == 0, "a miss stays a miss");

    // a cut of a percent of the BASE cooldown (Q4, 2026-10-23), never more than is left, nothing while ready, the
    // reset, a floor
    check(CutBasePct(5000, 6000, 20) == 1200, "Shieldbrand: Aegis Ricochet -1.2 s");
    check(CutBasePct(1000, 6000, 20) == 1000, "1 s left: never below zero");
    check(CutBasePct(0, 6000, 20) == 0, "ready: nothing");
    check(CutBasePct(5000, 0, 20) == 0, "no base cooldown: nothing");
    check(CutBasePct(18000, 30000, 100) == 18000, "Judgment's Echo: all of it");
    check(CutBasePct(30000, 30000, 150) == 30000, "never more than what is left");
    check(CutBasePct(170000, 180000, 20, 180000, 60000) == 36000, "Aegis of Judgment: the Retribution -36 s");
    check(CutBasePct(100000, 180000, 20, 180000, 60000, 110000) == 10000, "110 s cut already: 10 s left to cut");
    check(CutBasePct(100000, 180000, 20, 180000, 60000, 120000) == 0, "120 s cut already: at the 60 s floor");
    check(CutBasePct(140000, 180000, 20, 150000, 60000, 72000) == 18000, "a 150 s cooldown: the floor caps the cut");
    // an extension per press, capped
    check(ExtendCapped(0, 8000, 20000) == 8000, "the first press: 8 s");
    check(ExtendCapped(5000, 8000, 20000) == 13000, "5 s left: 13 s");
    check(ExtendCapped(15000, 8000, 20000) == 20000, "15 s left: capped at 20 s");
    check(ExtendCapped(0, 8000, 0) == 8000, "no cap: the add");
    // a refresh window in percent of the duration as cast (the Stormbringer's Stormbrand: 130 %)
    check(PctCapMs(18000, 130) == 23400, "18 s at 130 %: 23.4 s");
    check(ExtendCapped(4000, 18000, PctCapMs(18000, 130)) == 22000, "4 s left: all of it kept (22 s)");
    check(ExtendCapped(9000, 18000, PctCapMs(18000, 130)) == 23400, "9 s left: the window caps it at 23.4 s");
    check(ExtendCapped(4000, 36000, PctCapMs(36000, 130)) == 40000, "doubled while the storm cell holds: 4 s + 36 s");
    check(PctCapMs(-5, 130) == 0 && PctCapMs(18000, -1) == 0, "nothing below zero");
    // fields that stack (the Stormbringer's Hailwind): a tick strikes once per field covering the target
    check(StackedFieldDamage(100, 0) == 100, "the ticking field itself at least");
    check(StackedFieldDamage(100, 3) == 300, "three fields: three times");
    check(StackedFieldDamage(0x7FFFFFFFu, 2) == 0x7FFFFFFFu, "never past the 32-bit damage");
    check(InField(3.0f, 4.0f, 0.0f, 5.0f), "on the rim: inside");
    check(!InField(3.0f, 4.1f, 0.0f, 5.0f), "past the rim: outside");
    check(!InField(0.0f, 0.0f, 7.0f, 8.0f), "far above: outside");
    // a charge per gain, capped
    check(ChargesAfter(0, 2) == 1, "the first charge");
    check(ChargesAfter(1, 2) == 2, "the second charge");
    check(ChargesAfter(2, 2) == 2, "never a third");
    // a burst at most once an interval
    check(BurstDue(5000, 0, 1000), "the first blow bursts");
    check(!BurstDue(5500, 5000, 1000), "half a second later: not");
    check(BurstDue(6000, 5000, 1000), "a second later: again");

    // counted points (the Executioner's Openings): gains capped, finishers spend their cost (none with Perfect Execution)
    check(OpeningsAfterGain(0, 1, 5) == 1, "Sundering Blow: +1");
    check(OpeningsAfterGain(4, 3, 5) == 5, "Earthbreaker at 4: capped at 5, the excess lost");
    check(OpeningsAfterGain(5, 2, 5) == 5, "Deathblow at 5: nothing above the cap");
    check(!FinisherCastable(2, 3) && FinisherCastable(3, 3) && FinisherCastable(5, 3), "a finisher needs 3");
    check(FinisherOpeningsLeft(3, 3, false, 0, 5) == 0, "3 -> 0");
    check(FinisherOpeningsLeft(5, 3, false, 0, 5) == 2, "5 -> 2");
    check(FinisherOpeningsLeft(3, 3, true, 0, 5) == 3, "Perfect Execution: 3 -> 3");
    check(FinisherOpeningsLeft(2, 3, true, 0, 5) == 2, "Perfect Execution still needs 3: nothing happens at 2");
    check(FinisherOpeningsLeft(3, 3, false, 1, 5) == 1, "Headsman's Due: 3 -> 1");
    check(FinisherOpeningsLeft(5, 3, true, 1, 5) == 5, "Perfect Execution and Headsman's Due: never above 5");
    // the generators: once a cast, not per enemy
    check(PowerGain(1, 0, 5, true, false) == 1, "Cleaving Edge on 5 enemies: +1");
    check(PowerGain(1, 0, 0, true, false) == 0, "Cleaving Edge on nobody: nothing");
    // a sweep: up to a count of more enemies at a percent
    check(SweepCount(7, 4) == 4, "7 near: 4 swept");
    check(SweepCount(2, 4) == 2, "2 near: 2 swept");
    check(SweepCount(3, 0) == 0, "none allowed: none");
    check(SweepDamage(200, 50) == 100, "a sweep hit: half");
    check(SweepDamage(300, 50) == 150, "Red Harvest: 1.5 x the base, then half: three quarters");
    check(SweepDamage(0, 50) == 0, "a miss stays a miss");
    // a build floor (Murderous Instinct) is a floor, not added on top
    check(BuildWithRules(PowerGain(1, 0, 1, true, false), 0, 0) == 1, "v1.0 Deathblow: +1");
    check(BuildWithRules(PowerGain(1, 0, 1, true, false), 0, 3) == 3, "Murderous Instinct without Deadly Opportunity: 3");
    check(BuildWithRules(PowerGain(2, 0, 1, true, false), 0, 3) == 3, "Murderous Instinct with Deadly Opportunity: 3, not 5");
    check(BuildWithRules(PowerGain(1, 0, 0, true, false), 0, 3) == 0, "a Deathblow parried: nothing, floor or not");
    // a bonus at or below a health threshold
    check(AtOrBelowHealth(30.0f, 30) && AtOrBelowHealth(12.5f, 30) && !AtOrBelowHealth(30.5f, 30), "the 30 % gate");
    check(!AtOrBelowHealth(10.0f, 0), "no gate set: nobody qualifies");
    check(BuildWithRules(1, AtOrBelowHealth(25.0f, 30) ? 1 : 0, 0) == 2, "Sundering Blow at 25 %: 2 Openings");
    check(BuildWithRules(1, AtOrBelowHealth(60.0f, 30) ? 1 : 0, 0) == 1, "Sundering Blow at 60 %: 1");
    // once it hits, a bonus from a count of enemies on, nothing on nobody
    for (uint32_t hits : { 0u, 1u, 2u, 3u, 6u })
    {
        int32_t const base = PowerGain(1, 0, hits, true, false);
        check(BuildWithRules(base, 0, 0) == (hits ? 1 : 0), "Reaping Arc: 1 Opening once it hits, not per enemy");
        check(BuildWithRules(base, CountReached(hits, 3) ? 1 : 0, 0) == (hits >= 3 ? 2 : hits ? 1 : 0),
              "Sweeping Opportunity: 2 at 3 or more, 1 below, 0 on nobody");
    }
    check(!CountReached(5, 0), "no count set: never reached");
    // a crit ramp: a stack per non-crit, a crit takes them all, at most a maximum
    check(CritRampStacks(0, false, 10) == 1, "first non-crit: 1 stack");
    check(CritRampStacks(3, false, 10) == 4, "repeated non-crits accumulate");
    check(CritRampStacks(10, false, 10) == 10, "never above the stack maximum");
    check(CritRampStacks(4, true, 10) == 0, "a crit consumes and resets every stack");
    check(CritRampStacks(0, true, 10) == 0, "a crit at none: none");
    // one proc a cast: an area cast critting several enemies opens Deathblow once (ankarna, 2026-10-26)
    {
        int castA = 0, castB = 0;           // two spell objects
        OncePerCast once;
        int procs = 0;
        for (int enemy = 0; enemy < 5; ++enemy)   // Whirling Steel crits 5 enemies in one update
            if (once.Allows(&castA, 1, 1000))
            {
                once.Took(&castA, 1, 1000);
                ++procs;
            }
        check(procs == 1, "five crits of one Whirling Steel: one Deathblow proc");
        check(once.Allows(&castA, 1, 2500), "the next cast (the spell object reused later): procs again");
        check(once.Allows(&castB, 1, 1000), "another cast: procs");
        check(once.Allows(&castA, 2, 1000), "another spell: procs");
        check(once.Allows(nullptr, 1, 1000), "a proc without a spell: allowed");
    }
    // stored procs: the cap is effect 1 (a talent raises it), never above the aura's stack maximum
    check(StackCap(1, 2) == 1 && StacksAfter(1, 1, StackCap(1, 2)) == 1, "without Lingering Death: one proc, a new one refreshes");
    check(StackCap(2, 2) == 2 && StacksAfter(1, 1, StackCap(2, 2)) == 2, "Lingering Death: a second proc is stored");
    check(StacksAfter(2, 1, StackCap(2, 2)) == 2, "Lingering Death at 2: a third proc does not exceed the cap");
    check(StackCap(3, 2) == 2, "never above the aura's stack maximum");
    // lengthening per crit, at most a limit an activation
    check(LengthenBy(1000, 5000, 10000, 10000) == 1000, "the first crit: +1 s");
    check(LengthenBy(1000, 5000, 14000, 10000) == 1000, "the fifth: +1 s (15 s)");
    check(LengthenBy(1000, 5000, 15000, 10000) == 0, "the sixth: nothing (the cap)");
    check(LengthenBy(1000, 5000, 14500, 10000) == 500, "only what is left of the 5 s");
    // a percent per enemy beyond the first, counting at most a number
    check(CountBonusPct(1, 5, 5) == 0 && CountBonusPct(2, 5, 5) == 5 && CountBonusPct(3, 5, 5) == 10, "1 / 2 / 3 enemies");
    check(CountBonusPct(4, 5, 5) == 15 && CountBonusPct(5, 5, 5) == 20 && CountBonusPct(9, 5, 5) == 20,
          "4 / 5 / 9: capped at 20 %");
    check(CountBonusPct(0, 5, 5) == 0, "nobody struck: nothing");
    check(DamageWithPct(200, CountBonusPct(5, 5, 5)) == 240, "a 200 hit on 5 enemies: 240 on each");
    // a bonus with exactly one enemy; percents summed
    check(SingleTargetPct(1, 50) == 50 && SingleTargetPct(2, 50) == 0 && SingleTargetPct(0, 50) == 0, "exactly one");
    check(DamageWithPct(100, 40) == 140 && DamageWithPct(0, 40) == 0, "Merciless Strike on 100: 140; a miss: 0");
    // a percent of max health
    check(PctOfMaxHealth(4321, 10) == 432 && PctOfMaxHealth(4321, 0) == 0, "10 % of max health");
    // a crit's total times a bonus
    check(CritWithBonus(200, 50) == 300, "a 200 crit deals 300");
    check(CritWithBonus(200, 0) == 200, "outside Killing Instinct: as rolled");
    // an uncapped extension per application
    check(ExtendCapped(0, 8000, 0) == 8000, "the bleed: 8 s");
    check(ExtendCapped(3000, 8000, 0) == 11000, "the bleed: 3 s left, 11 s");
    // the spec power's own maximum and its keeping
    check(SpecPowerMaxDelta(50, 1000) == -950, "Openings: 50 of the slot's 1000");
    check(SpecPowerMaxDelta(1000, 1000) == 0, "Static, Energy: the slot's");
    check(SpecPowerMaxDelta(1500, 1000) == 500, "the Pyromancer's Heat: 150, above the slot's 100");
    check(SpecPowerMaxDelta(0, 1000) == 0, "no maximum of its own");
    check(BelowHealth(34.9f, 35), "Headsman's Due: below 35 %");
    check(!BelowHealth(35.0f, 35), "Headsman's Due: not at 35 %");
    check(BelowHealth(0.0f, 35), "Headsman's Due: a killing blow counts");
    check(BelowHealth(90.0f, 0), "no gate: always");
    check(KeptPower(0, 30, true) == 30, "a level up empties the slot: Openings come back");
    check(KeptPower(0, 30, false) == 0, "a power that is not kept stays empty");
    check(KeptPower(20, 30, true) == 20, "a power not emptied stays as it is");

    // a growth chain on one clock, heal shares, mana back, a stored heal and its release
    check(GrowthLeft(18000, 9000) == 9000, "a Seed's natural end leaves a 9 s Sprout");
    check(GrowthLeft(18000, 2000) == 16000, "a Seed pushed at 2 s leaves a 16 s Sprout (the clock is not reset)");
    check(GrowthLeft(21000, 9000) == 12000, "Lasting Renewal's 3 s go to the Sprout");
    check(GrowthLeft(9000, 12000) == 0, "never below 0");
    check(StageMs(16000, 9000, true) == 16000, "the last stage runs what is left");
    check(StageMs(16000, 9000, false) == 9000, "a stage between keeps its own duration");
    check(PctOf(600, 18) == 108, "Deep Restoration 3: +18 %");
    check(PctOf(0, 40) == 0 && PctOf(300, 0) == 0, "nothing of nothing");
    check(ManaBack(4820, 20) == 96, "Rising Sap: 2 % of 4,820");
    check(StoreHundredths(300, 30) == 9000, "30 % of a 300 Bloom: 90.00");
    {
        uint32_t missing[3] = {500, 300, 900};
        uint32_t out[3] = {};
        check(ReleaseSplit(700, missing, out, 3) == 700 && out[0] == 500 && out[1] == 200 && out[2] == 0,
              "the release fills the lowest first, then the next");
        check(ReleaseSplit(5000, missing, out, 3) == 1700 && out[2] == 900, "what nobody needs is lost");
    }
    check(CooldownFromStart(45000, 12000) == 33000, "released at 12 s: 33 s left");
    check(CooldownFromStart(45000, 60000) == 0, "never below 0");
    // the Corsair's Flow (EfFlowLogic.h): a prepared finisher holds, No Quarter, a pair, a mark
    {
        using namespace Emberfall::ClassKit::Flow;
        check(NextStep(true, true, true) == Step::Nothing, "a prepared finisher: builders change nothing");
        check(NextStep(false, true, true) == Step::Override, "No Quarter: the builder's own finisher");
        check(NextStep(false, false, true) == Step::Combine, "a partner's mark: the pair's finisher");
        check(NextStep(false, false, false) == Step::Mark, "alone: the mark");
        // a percent of the strike's base split over the ticks, doubled by a crit, at least 1 a tick
        check(Ticks(12000, 1000) == 12 && Ticks(5000, 1000) == 5 && Ticks(0, 1000) == 1, "ticks over the base duration");
        check(SplitTick(70, 100, 12, false) == 5, "Saltwound at 60: 70 over 12 ticks");
        check(SplitTick(70, 100, 12, true) == 11, "a crit doubles it");
        check(SplitTick(31, 233, 5, false) == 14, "Razorstorm at 60: 7/15 of the spin a tick");
        check(SplitTick(3, 100, 12, false) == 1, "at least 1 a tick");
        check(SplitTick(0, 100, 12, true) == 0, "no base, nothing");
        // the pool: the first free instance, else the one closest to its end
        check(PickInstance({ 5000, -1, 3000 }) == 1, "a free instance first");
        check(PickInstance({ 5000, 2000, 3000 }) == 1, "all running: the least time left");
        // extra crit chance on what is left
        check(ExtraCritChance(10.0f, 25.0f) > 27.7f && ExtraCritChance(10.0f, 25.0f) < 27.8f, "25 of the 90 left");
        check(ExtraCritChance(100.0f, 25.0f) == 0.0f && ExtraCritChance(50.0f, 0.0f) == 0.0f, "nothing to add");
        // a pull that lands short
        check(HookTravel(20.0f, 5.0f) == 15.0f && HookTravel(4.0f, 5.0f) == 0.0f, "the haul");
        // v2.0 (ankarna 2026-10-27): Deep Wounds one at a time, Open Wounds six of their own
        check(PoolInstances(1, 0) == 1 && PoolInstances(1, 6) == 6 && PoolInstances(0, 0) == 1 && PoolInstances(1, 20) == 8,
              "the pool: the dummy's own, a passive's count, 1..8");
        check(StackBonusPct(20, 2) == 40 && StackBonusPct(0, 2) == 0 && WithPct(100, 40) == 140 && WithPct(100, 0) == 100,
              "Crimson Undertow: 2 % a stack, 40 % at 20");
        check(PctOf(100, 50) == 50 && PctOf(1, 30) == 1 && PctOf(0, 30) == 0, "Relentless Strike's half, at least 1");
        check(CountAmount(3, 1, 10) == 3 && CountAmount(14, 1, 10) == 10 && CountAmount(0, 1, 10) == 0,
              "Evasive Sweep: 1 % an enemy, at most 10");
        check(!RhythmChanged(0, 91207) && !RhythmChanged(91207, 91207) && RhythmChanged(91206, 91207),
              "Perfect Rhythm: the first starts the tracking, a repeat gives nothing, a change does");
        check(BarrageDelay(0, 200) == 0 && BarrageDelay(4, 200) == 800, "Cutlass Barrage: the first at once, 0.2 s apart");
    }
    // the Damnator's curses (EfSealLogic.h, EfClassKitLogic.h ExtendCapped)
    {
        using namespace Emberfall::ClassKit::Seal;
        // the refresh: an extension up to a cap, never shortening a longer time
        check(ExtendCapped(0, 18000, 23000) == 18000, "a fresh curse: 18 s");
        check(ExtendCapped(4000, 18000, 23000) == 22000, "4 s left: 22 s");
        check(ExtendCapped(10000, 18000, 23000) == 23000, "10 s left: capped at 23 s");
        check(ExtendCapped(30000, 18000, 23000) == 30000, "30 s left after a Torrent: kept, never shortened");
        // the tick bonus: a condition's percent and the others summed, never below -100
        check(TickBonus(true, 50, 0, 0, 0) == 50 && TickBonus(false, 50, 0, 0, 0) == 0, "the condition");
        check(TickBonus(true, 70, 0, 50, 0) == 120, "Salt in the Wound 2 in Crimson Flood");
        check(TickBonus(false, 75, 20, 50, 10) == 80, "Cursed Shoals, Flood and Red Wake add up");
        check(TickBonus(false, 0, -500, 0, 0) == -100, "never below -100");
        check(WithBonus(10, 50) == 15 && WithBonus(11, 75) == 19 && WithBonus(10, -100) == 0, "the multiplied tick");
        // the ticks left: the time rounded up
        check(TicksLeft(18000, 1000) == 18 && TicksLeft(17400, 1000) == 18 && TicksLeft(0, 1000) == 0, "ticks left");
        check(TicksLeft(500, 0) == 0, "no amplitude, no ticks");
        // a detonation: a percent of what is left (the boosted ticks counted), capped at an int32
        check(CollapseHit(10000, 125) == 12500, "125 % of 10,000");
        check(CollapseHit(Remaining(10000, 1000, WithBonus(1000, 50)), 125) == 18750, "in Crimson Flood: 18,750");
        check(CollapseHit(int64_t(1) << 40, 125) == 0x7FFFFFFF, "capped at an int32");
        // an extension per point spent, past the refresh cap
        check(TorrentExtension(10, 1000) == 10000 && TorrentExtension(10, 1500) == 15000, "the extension");
        check(Extended(20000, 10000) == 30000 && Extended(-1, 3000) == 3000, "past the 23 s cap");
        // Unbroken Vow (feedback #81): the Vow a Collapse detonates but leaves goes on with its own 18 s, not the time
        // it had (a Vow at 38 s after a Torrent was dealt as 38 ticks by the Collapse, then ticked those 38 again and
        // grew with the next Torrent)
        {
            int32_t const left = Extended(23000, TorrentExtension(10, 1500));
            int64_t const dealt = TicksLeft(left, 1000);
            int32_t const kept = KeptAfterCollapse(18000);
            check(dealt == 38 && kept == 18000 && kept < left, "a detonated 38 s Vow goes on with 18 s, not 38 s");
            check(KeptAfterCollapse(-1) == 0, "never below 0");
        }
        // Blood Tide: both missing first, then one, then none; the closest on a tie
        {
            std::vector<Candidate> c{ { 0, 0, 1.0f }, { 1, 1, 9.0f }, { 2, 2, 25.0f }, { 3, 1, 4.0f } };
            std::vector<std::size_t> want{ 2, 3 };
            check(PickSpread(c, 2) == want, "most lacking first, then the closest");
            std::vector<std::size_t> three{ 2, 3, 1 };
            check(PickSpread(c, 3) == three, "Spreading Stain: three");
            check(PickSpread({}, 2).empty(), "nobody near");
        }
        check(SpreadSeals(3, 1, 2) == 2 && SpreadSeals(1, 1, 2) == 1 && SpreadSeals(0, 1, 2) == 0, "at most 2 Seals");
        check(CopyLengthens(9000, 4000) && !CopyLengthens(9000, 12000) && CopyLengthens(9000, -1), "never shortening");
        check(ShareOf(200, 3) == 6 && ShareOf(-5, 6) == 0, "Drink Deep's share");
    }

    std::cout << "classkit tests: " << (g_checks - g_failed) << "/" << g_checks << " passed\n";
    return g_failed ? 1 : 0;
}
