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
    // the Hunter's Focus: 5 a second in a fight, times haste and the regen-percent auras (Held Breath +50 %), the
    // fractions carried; 25 a second out of one
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
    // the Marksman's Full Draw: 10 % more for each Bead
    check(StackedDamage(200, 10, 5) == 300, "5 Beads: half again");
    check(StackedDamage(200, 14, 5) == 340, "Patient Kill: 14 % a Bead");
    check(StackedDamage(200, 10, 0) == 200, "no Beads: the base");
    check(StackedDamage(0, 10, 5) == 0, "a miss stays a miss");

    // the Oathkeeper's chain: 20 % of the BASE cooldown (Q4, 2026-10-23), never more than is left, nothing while
    // ready, the reset, the 60 s floor of the Retribution
    check(CutBasePct(5000, 6000, 20) == 1200, "Shieldbrand: Aegis Ricochet -1.2 s");
    check(CutBasePct(10000, 15000, 20) == 3000, "Aegis Ricochet: Bastion Breaker -3 s");
    check(CutBasePct(20000, 30000, 20) == 6000, "Bastion Breaker: Aegis of Judgment -6 s");
    // (taken at Breaker's cast, hit or miss: spell_ef_kit_helper_links reads the burst's link; feedback #47)
    check(CutBasePct(20000, 30000, 30) == 9000, "Linked Oaths 2: Aegis of Judgment -9 s");
    check(CutBasePct(4000, 6000, 20) == 1200, "Oathbound Rush: Aegis Ricochet -1.2 s");
    check(CutBasePct(1000, 6000, 20) == 1000, "1 s left: never below zero");
    check(CutBasePct(0, 6000, 20) == 0, "ready: nothing");
    check(CutBasePct(5000, 0, 20) == 0, "no base cooldown: nothing");
    check(CutBasePct(18000, 30000, 100) == 18000, "Judgment's Echo: all of it");
    check(CutBasePct(30000, 30000, 150) == 30000, "never more than what is left");
    check(CutBasePct(170000, 180000, 20, 180000, 60000) == 36000, "Aegis of Judgment: the Retribution -36 s");
    check(CutBasePct(100000, 180000, 20, 180000, 60000, 110000) == 10000, "110 s cut already: 10 s left to cut");
    check(CutBasePct(100000, 180000, 20, 180000, 60000, 120000) == 0, "120 s cut already: at the 60 s floor");
    check(CutBasePct(140000, 180000, 20, 150000, 60000, 72000) == 18000, "a 150 s cooldown: the floor caps the cut");
    // Bastion's Resolve: +8 s a press, at most 20 s
    check(ExtendCapped(0, 8000, 20000) == 8000, "the first press: 8 s");
    check(ExtendCapped(5000, 8000, 20000) == 13000, "5 s left: 13 s");
    check(ExtendCapped(15000, 8000, 20000) == 20000, "15 s left: capped at 20 s");
    check(ExtendCapped(15000, 8000, 28000) == 23000, "Long Resolve 2: 28 s cap");
    check(ExtendCapped(0, 8000, 0) == 8000, "no cap: the add");
    // Solenne's Favor: a charge a gain, 2 at most
    check(ChargesAfter(0, 2) == 1, "the first charge");
    check(ChargesAfter(1, 2) == 2, "the second charge");
    check(ChargesAfter(2, 2) == 2, "never a third");
    // Solenne's Aegis: a burst at most once a second
    check(BurstDue(5000, 0, 1000), "the first blow bursts");
    check(!BurstDue(5500, 5000, 1000), "half a second later: not");
    check(BurstDue(6000, 5000, 1000), "a second later: again");

    // the Executioner v2.0: personal Openings (max 5), finishers spend exactly 3 (0 with Perfect Execution)
    check(OpeningsAfterGain(0, 1, 5) == 1, "Sundering Blow: +1");
    check(OpeningsAfterGain(4, 3, 5) == 5, "Earthbreaker at 4: capped at 5, the excess lost");
    check(OpeningsAfterGain(5, 2, 5) == 5, "Deathblow at 5: nothing above the cap");
    check(OpeningsAfterGain(1, 2, 5) == 3, "Deathblow: +2");
    check(OpeningsAfterGain(1, 3, 5) == 4, "Deathblow in Killing Instinct: +3");
    check(!FinisherCastable(2, 3) && FinisherCastable(3, 3) && FinisherCastable(5, 3), "a finisher needs 3");
    check(FinisherOpeningsLeft(3, 3, false, 0, 5) == 0, "3 -> 0");
    check(FinisherOpeningsLeft(4, 3, false, 0, 5) == 1, "4 -> 1");
    check(FinisherOpeningsLeft(5, 3, false, 0, 5) == 2, "5 -> 2");
    check(FinisherOpeningsLeft(3, 3, true, 0, 5) == 3, "Perfect Execution: 3 -> 3");
    check(FinisherOpeningsLeft(5, 3, true, 0, 5) == 5, "Perfect Execution: 5 -> 5");
    check(FinisherOpeningsLeft(2, 3, true, 0, 5) == 2, "Perfect Execution still needs 3: nothing happens at 2");
    check(FinisherOpeningsLeft(3, 3, false, 1, 5) == 1, "Headsman's Due: 3 -> 1");
    check(FinisherOpeningsLeft(5, 3, true, 1, 5) == 5, "Perfect Execution and Headsman's Due: never above 5");
    // the generators: once a cast, not per enemy (Cleaving Edge +1, Earthbreaker +3, Deathblow +2 also when it sweeps)
    check(PowerGain(1, 0, 5, true, false) == 1, "Cleaving Edge on 5 enemies: +1");
    check(PowerGain(1, 0, 0, true, false) == 0, "Cleaving Edge on nobody: nothing");
    check(PowerGain(3, 0, 4, true, false) == 3, "Earthbreaker on 4 enemies: +3");
    check(PowerGain(2, 0, 1, true, false) == 2, "Deathblow: +2");
    check(PowerGain(3, 0, 1, true, false) == 3, "Deathblow in Killing Instinct: +3");
    // Sweeping Deathblow: up to 4 more enemies at half damage
    check(SweepCount(7, 4) == 4, "7 near: 4 swept");
    check(SweepCount(2, 4) == 2, "2 near: 2 swept");
    check(SweepCount(3, 0) == 0, "none allowed: none");
    check(SweepDamage(200, 50) == 100, "a sweep hit: half");
    check(SweepDamage(300, 50) == 150, "Red Harvest: 1.5 x the base, then half: three quarters");
    check(SweepDamage(0, 50) == 0, "a miss stays a miss");
    // the Executioner's Talent Tree v1.0 (warrior-executioner.md §15): Deathblow 1, Deadly Opportunity 2, Murderous
    // Instinct 3 in Killing Instinct (not on top)
    check(BuildWithRules(PowerGain(1, 0, 1, true, false), 0, 0) == 1, "v1.0 Deathblow: +1");
    check(BuildWithRules(PowerGain(2, 0, 1, true, false), 0, 0) == 2, "Deadly Opportunity: +2");
    check(BuildWithRules(PowerGain(1, 0, 1, true, false), 0, 3) == 3, "Murderous Instinct without Deadly Opportunity: 3");
    check(BuildWithRules(PowerGain(2, 0, 1, true, false), 0, 3) == 3, "Murderous Instinct with Deadly Opportunity: 3, not 5");
    check(BuildWithRules(PowerGain(1, 0, 0, true, false), 0, 3) == 0, "a Deathblow parried: nothing, floor or not");
    // Sundering Blow: Executioner's Opportunity +1 at or below 30 %
    check(AtOrBelowHealth(30.0f, 30) && AtOrBelowHealth(12.5f, 30) && !AtOrBelowHealth(30.5f, 30), "the 30 % gate");
    check(!AtOrBelowHealth(10.0f, 0), "no gate set: nobody qualifies");
    check(BuildWithRules(1, AtOrBelowHealth(25.0f, 30) ? 1 : 0, 0) == 2, "Sundering Blow at 25 %: 2 Openings");
    check(BuildWithRules(1, AtOrBelowHealth(60.0f, 30) ? 1 : 0, 0) == 1, "Sundering Blow at 60 %: 1");
    // Reaping Arc's Openings: 1 when it hits, Sweeping Opportunity +1 at 3 enemies or more, nothing on nobody
    for (uint32_t hits : { 0u, 1u, 2u, 3u, 6u })
    {
        int32_t const base = PowerGain(1, 0, hits, true, false);
        check(BuildWithRules(base, 0, 0) == (hits ? 1 : 0), "Reaping Arc: 1 Opening once it hits, not per enemy");
        check(BuildWithRules(base, CountReached(hits, 3) ? 1 : 0, 0) == (hits >= 3 ? 2 : hits ? 1 : 0),
              "Sweeping Opportunity: 2 at 3 or more, 1 below, 0 on nobody");
    }
    check(!CountReached(5, 0), "no count set: never reached");
    // Growing Precision: a Final Verdict without a crit +1 stack (10 percentage points), a crit takes them all, at most 10
    check(CritRampStacks(0, false, 10) == 1, "first non-crit: 1 stack");
    check(CritRampStacks(3, false, 10) == 4, "repeated non-crits accumulate");
    check(CritRampStacks(10, false, 10) == 10, "never above the stack maximum");
    check(CritRampStacks(4, true, 10) == 0, "a crit consumes and resets every stack");
    check(CritRampStacks(0, true, 10) == 0, "a crit at none: none");
    // Lingering Death: the window 1 proc (cap = effect 1), 2 with the talent's +1; one Deathblow spends one
    check(StackCap(1, 2) == 1 && StacksAfter(1, 1, StackCap(1, 2)) == 1, "without Lingering Death: one proc, a new one refreshes");
    check(StackCap(2, 2) == 2 && StacksAfter(1, 1, StackCap(2, 2)) == 2, "Lingering Death: a second proc is stored");
    check(StacksAfter(2, 1, StackCap(2, 2)) == 2, "Lingering Death at 2: a third proc does not exceed the cap");
    check(StackCap(3, 2) == 2, "never above the aura's stack maximum");
    // Unending Slaughter: +1 s a critical finisher in Killing Instinct, at most 5 s an activation (10 s -> 15 s)
    check(LengthenBy(1000, 5000, 10000, 10000) == 1000, "the first crit: +1 s");
    check(LengthenBy(1000, 5000, 14000, 10000) == 1000, "the fifth: +1 s (15 s)");
    check(LengthenBy(1000, 5000, 15000, 10000) == 0, "the sixth: nothing (the cap)");
    check(LengthenBy(1000, 5000, 14500, 10000) == 500, "only what is left of the 5 s");
    // Relentless Whirl: +5 % per enemy beyond the first, counting at most 5
    check(CountBonusPct(1, 5, 5) == 0 && CountBonusPct(2, 5, 5) == 5 && CountBonusPct(3, 5, 5) == 10, "1 / 2 / 3 enemies");
    check(CountBonusPct(4, 5, 5) == 15 && CountBonusPct(5, 5, 5) == 20 && CountBonusPct(9, 5, 5) == 20,
          "4 / 5 / 9: capped at 20 %");
    check(CountBonusPct(0, 5, 5) == 0, "nobody struck: nothing");
    check(DamageWithPct(200, CountBonusPct(5, 5, 5)) == 240, "a 200 hit on 5 enemies: 240 on each");
    // Focused Reaping: +50 % with exactly one enemy; Merciless Strike +40 % (summed with the others)
    check(SingleTargetPct(1, 50) == 50 && SingleTargetPct(2, 50) == 0 && SingleTargetPct(0, 50) == 0, "exactly one");
    check(DamageWithPct(100, 40) == 140 && DamageWithPct(0, 40) == 0, "Merciless Strike on 100: 140; a miss: 0");
    // Reaper's Vitality: 10 % of max health, the same against one or many
    check(PctOfMaxHealth(4321, 10) == 432 && PctOfMaxHealth(4321, 0) == 0, "10 % of max health");
    // Killing Instinct v1.0: a crit's total x 1.2, x 1.5 with Predator's Fury
    check(CritWithBonus(200, 20) == 240, "v1.0 Killing Instinct: a 200 crit deals 240");
    // Killing Instinct: a crit's total x 1.5
    check(CritWithBonus(200, 50) == 300, "a 200 crit deals 300");
    check(CritWithBonus(200, 0) == 200, "outside Killing Instinct: as rolled");
    // the bleed: +8 s an application, no cap, on each enemy on its own
    check(ExtendCapped(0, 8000, 0) == 8000, "the bleed: 8 s");
    check(ExtendCapped(3000, 8000, 0) == 11000, "the bleed: 3 s left, 11 s");
    check(ExtendCapped(30000, 8000, 0) == 38000, "the bleed: 30 s left, 38 s");
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

    // the Druid's Bloomtender: the fixed 18 s clock, heal shares, Rising Sap, Verdant Ancient's store and release
    check(GrowthLeft(18000, 9000) == 9000, "a Seed's natural end leaves a 9 s Sprout");
    check(GrowthLeft(18000, 2000) == 16000, "a Seed pushed at 2 s leaves a 16 s Sprout (the clock is not reset)");
    check(GrowthLeft(21000, 9000) == 12000, "Lasting Renewal's 3 s go to the Sprout");
    check(GrowthLeft(9000, 12000) == 0, "never below 0");
    check(StageMs(16000, 9000, true) == 16000, "the last stage runs what is left");
    check(StageMs(16000, 9000, false) == 9000, "a stage between keeps its own duration");
    check(PctOf(600, 18) == 108, "Deep Restoration 3: +18 %");
    check(PctOf(300, 40) == 120, "Radiant Blossom 2: 40 % of a Bloom");
    check(PctOf(0, 40) == 0 && PctOf(300, 0) == 0, "nothing of nothing");
    check(ManaBack(4820, 20) == 96, "Rising Sap: 2 % of 4,820");
    check(ManaBack(4820, 30) == 144, "Returning Essence 2: 3 %");
    check(StoreHundredths(300, 30) == 9000, "30 % of a 300 Bloom: 90.00");
    {
        uint32_t missing[3] = {500, 300, 900};
        uint32_t out[3] = {};
        check(ReleaseSplit(700, missing, out, 3) == 700 && out[0] == 500 && out[1] == 200 && out[2] == 0,
              "the release fills the lowest first, then the next");
        check(ReleaseSplit(5000, missing, out, 3) == 1700 && out[2] == 900, "what nobody needs is lost");
    }
    check(CooldownFromStart(45000, 12000) == 33000, "released at 12 s: 33 s left");
    check(CooldownFromStart(45000, 20000) == 25000, "the automatic release: 25 s left");
    check(CooldownFromStart(45000, 60000) == 0, "never below 0");
    // the Corsair's Flow (EfFlowLogic.h): a prepared finisher holds, No Quarter, a pair, a mark
    {
        using namespace Emberfall::ClassKit::Flow;
        check(NextStep(true, true, true) == Step::Nothing, "a prepared finisher: builders change nothing");
        check(NextStep(false, true, true) == Step::Override, "No Quarter: the builder's own finisher");
        check(NextStep(false, false, true) == Step::Combine, "a partner's mark: the pair's finisher");
        check(NextStep(false, false, false) == Step::Mark, "alone: the mark");
        // Saltwound: 100 % of the strike's base over 12 ticks, doubled by a crit; Razorstorm: 233 % over 5
        check(Ticks(12000, 1000) == 12 && Ticks(5000, 1000) == 5 && Ticks(0, 1000) == 1, "ticks over the base duration");
        check(SplitTick(70, 100, 12, false) == 5, "Saltwound at 60: 70 over 12 ticks");
        check(SplitTick(70, 100, 12, true) == 11, "a crit doubles it");
        check(SplitTick(31, 233, 5, false) == 14, "Razorstorm at 60: 7/15 of the spin a tick");
        check(SplitTick(3, 100, 12, false) == 1, "at least 1 a tick");
        check(SplitTick(0, 100, 12, true) == 0, "no base, nothing");
        // the pool: the first free instance, else the one closest to its end
        check(PickInstance({ 5000, -1, 3000 }) == 1, "a free instance first");
        check(PickInstance({ 5000, 2000, 3000 }) == 1, "all running: the least time left");
        // Blood in the Water: +25 % on a 10 % chance -> 35 % in all
        check(ExtraCritChance(10.0f, 25.0f) > 27.7f && ExtraCritChance(10.0f, 25.0f) < 27.8f, "25 of the 90 left");
        check(ExtraCritChance(100.0f, 25.0f) == 0.0f && ExtraCritChance(50.0f, 0.0f) == 0.0f, "nothing to add");
        // Boarding Hook: lands 5 yd short
        check(HookTravel(20.0f, 5.0f) == 15.0f && HookTravel(4.0f, 5.0f) == 0.0f, "the haul");
    }
    // the Damnator's curses (EfSealLogic.h, EfClassKitLogic.h ExtendCapped)
    {
        using namespace Emberfall::ClassKit::Seal;
        // the refresh: +18 s, at most 23 s, never shortening a Torrent's extension
        check(ExtendCapped(0, 18000, 23000) == 18000, "a fresh curse: 18 s");
        check(ExtendCapped(4000, 18000, 23000) == 22000, "4 s left: 22 s");
        check(ExtendCapped(10000, 18000, 23000) == 23000, "10 s left: capped at 23 s");
        check(ExtendCapped(30000, 18000, 23000) == 30000, "30 s left after a Torrent: kept, never shortened");
        // the tick bonus: Bloodbinding beside Brine +50 %, Salt in the Wound 2 +20; Vow below 35 % +75; Flood +50
        check(TickBonus(true, 50, 0, 0, 0) == 50 && TickBonus(false, 50, 0, 0, 0) == 0, "the condition");
        check(TickBonus(true, 70, 0, 50, 0) == 120, "Salt in the Wound 2 in Crimson Flood");
        check(TickBonus(false, 75, 20, 50, 10) == 80, "Cursed Shoals, Flood and Red Wake add up");
        check(TickBonus(false, 0, -500, 0, 0) == -100, "never below -100");
        check(WithBonus(10, 50) == 15 && WithBonus(11, 75) == 19 && WithBonus(10, -100) == 0, "the multiplied tick");
        // the ticks left: the time rounded up
        check(TicksLeft(18000, 1000) == 18 && TicksLeft(17400, 1000) == 18 && TicksLeft(0, 1000) == 0, "ticks left");
        check(TicksLeft(500, 0) == 0, "no amplitude, no ticks");
        // Crimson Collapse: 10,000 left -> 12,500; in Flood the ticks are 1.5 x: 18,750
        check(CollapseHit(10000, 125) == 12500, "125 % of 10,000");
        check(CollapseHit(Remaining(10000, 1000, WithBonus(1000, 50)), 125) == 18750, "in Crimson Flood: 18,750");
        check(CollapseHit(10000, 145) == 14500, "Hungry Deep 2");
        check(CollapseHit(int64_t(1) << 40, 125) == 0x7FFFFFFF, "capped at an int32");
        // Sanguine Torrent: 10 Seals, +10 s each; Rising Water 2: +15 s; past the cap
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
