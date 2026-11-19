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

// Standalone tests of the Hunter Ranger v1.0's rules (src/classkit/EfRangerLogic.h, the spec power's tier rule in
// src/classkit/EfClassKitLogic.h; tests/run_tests.sh builds and runs them), named by the "Developer Acceptance Tests" of
// ankarna's and sonaetiger's "Developer Handoff v1.0" (2026-11-19, Emberfall docs/design/classes/hunter-ranger.md §9)
// wherever the server decides the outcome. The numbers in the vectors are the hand-off's; the rules read theirs from the
// spells' own effects.

#include "EfClassKitLogic.h"
#include "EfRangerLogic.h"

#include <iostream>
#include <string>
#include <vector>

using namespace Emberfall::ClassKit;
using namespace Emberfall::ClassKit::Ranger;

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

    // A spend sequence from `start`: every cost taken, the transition's gate after it.
    int32_t Spend(int32_t start, std::vector<int32_t> const& costs)
    {
        int32_t e = start;
        for (int32_t c : costs)
            e = AfterSpend(e, c);
        return e;
    }
}

int main()
{
    // AT 1: a fight starts in Falcon's Eye with 100 Energy; leaving it from either stance: Falcon's Eye, 100 Energy
    {
        RestAction const fromBear = Rest(false, false, true, 0, 1000);
        check(fromBear.addRest && fromBear.removeOther && fromBear.fill, "AT1: out of a fight from Bear's Claw: Falcon's Eye on, Bear's Claw off, full");
        RestAction const fromFalcon = Rest(false, true, false, 250, 1000);
        check(!fromFalcon.addRest && !fromFalcon.removeOther && fromFalcon.fill, "AT1: out of a fight in Falcon's Eye at 25: filled");
        RestAction const atRest = Rest(false, true, false, 1000, 1000);
        check(!atRest.addRest && !atRest.removeOther && !atRest.fill, "AT1: at rest and full: nothing to do");
        RestAction const fight = Rest(true, false, true, 0, 1000);
        check(!fight.addRest && !fight.removeOther && !fight.fill, "AT1: in a fight Bear's Claw stays, no Energy comes back");
        RestAction const none = Rest(true, false, false, 300, 1000);
        check(none.addRest && !none.fill, "AT1: in a fight with no stance (after a death): Falcon's Eye, no Energy");
    }

    // AT 2: Falcon 25 + 25 + 50 = 0, then Savage Leap: Bear's Claw, 100
    check(Spend(100, { 25, 25, 50 }) == 0 && TransitionOpen(0), "AT2: 25 + 25 + 50 reach exactly 0: Savage Leap open");
    check(AfterTransition(100, 100) == 100, "AT2: the transition sets 100");

    // AT 3: Bear 50 + 50 = 0, Retreat with no target: Falcon's Eye, 100 (no target needed: the area pick below)
    check(Spend(100, { 50, 50 }) == 0 && TransitionOpen(0), "AT3: 50 + 50 reach exactly 0: Retreat open");
    check(AreaPick(0, -1, 5).empty(), "AT3: Retreat with nobody around strikes nobody and still goes");

    // AT 4: Brutal Cleave at 100 reaches 0; Retreat against a wall switches anyway (its leap's landing bonuses come at
    // once when it does not move: spell_ef_kit_landing_bonuses)
    check(Spend(100, { 100 }) == 0 && TransitionOpen(0), "AT4: Brutal Cleave spends all 100: Retreat open");

    // AT 5: no unspendable residual from the approved costs
    check(AfterSpend(25, 25) == 0, "AT5: 25 in Falcon's Eye: Piercing Shot usable, to 0");
    check(AfterSpend(50, 50) == 0, "AT5: 50 in Bear's Claw: Rending Strike usable, to 0");
    check(EverySpendable(100, { 25, 50, 75 }), "AT5: Falcon's Eye 25 / 50 / 75: every reachable amount spends to 0");
    check(EverySpendable(100, { 50, 50, 100 }), "AT5: Bear's Claw 50 / 50 / 100: every reachable amount spends to 0");
    check(!EverySpendable(100, { 30, 50 }), "AT5: the rule sees a stranded residual (30 + 50 would leave 20, 10)");

    // AT 6 and 7: nonzero Energy shuts the transitions (the spec power's tier at 0: Energy Spent while exactly 0)
    check(Logic::TierHolds(0, 0), "AT6/7: the tier at 0 holds at exactly 0");
    check(!Logic::TierHolds(0, 10) && !Logic::TierHolds(0, 250) && !Logic::TierHolds(0, 1000), "AT7: any Energy shuts it");
    check(!TransitionOpen(5) && !TransitionOpen(25), "AT7: low Energy is not 0");
    check(Logic::TierHolds(300, 300) && !Logic::TierHolds(300, 299), "the other tiers keep their rule (Heat's Blazing at 30)");

    // AT 9: Retreat with your target in the area and four more: full + four halves; no target: at most four halves
    {
        std::vector<size_t> const with = AreaPick(7, 3, 5);
        check(with.size() == 5 && with[0] == 3, "AT9: the selected enemy and 4 others (5 of 7)");
        check(AreaShare(1000, true, 50) == 1000 && AreaShare(1000, false, 50) == 500, "AT9: full and half");
        std::vector<size_t> const without = AreaPick(7, -1, 5);
        check(without.size() == 4 && without[0] == 0 && without[3] == 3, "AT9: no target: the 4 nearest, at half");
        std::vector<size_t> const few = AreaPick(2, -1, 5);
        check(few.size() == 2, "AT9: no target and two in the area: both at half");
    }

    // AT 11: Perfect Shot's sure crit at exactly 80 % and 20 % as it strikes; at 50 % none
    check(HealthCrit(80.0f, 80, 1) && HealthCrit(100.0f, 80, 1), "AT11: Fresh Prey at 80 % and above");
    check(HealthCrit(20.0f, 20, 2) && HealthCrit(3.0f, 20, 2), "AT11: Final Prey at 20 % and below");
    check(!HealthCrit(50.0f, 80, 1) && !HealthCrit(50.0f, 20, 2), "AT11: at 50 % only the ordinary chance");
    check(!HealthCrit(79.9f, 80, 1) && !HealthCrit(20.1f, 20, 2), "AT11: just inside the thresholds: no sure crit");

    // AT 13: Hunter's Opening: 1 then 2 stacks, the timer renewed at the cap, the next Leap +100 %, the bleed untouched
    check(StacksAfterAdd(0, 2) == 1 && StacksAfterAdd(1, 2) == 2 && StacksAfterAdd(2, 2) == 2, "AT13: 1, 2, still 2");
    check(StackedStrike(1000, 1, 50) == 1500, "AT13: one stack: 150 %");
    check(StackedStrike(1000, 2, 50) == 2000, "AT13: two stacks: 200 % (added, never 1.5 x 1.5)");
    check(StackedStrike(1000, 0, 50) == 1000, "AT13: no stack: the ordinary impact");

    // AT 14: two quick Raking Sweeps: two bleeds of their own clocks; one 30 % slow until both are gone
    {
        std::vector<int32_t> left = { -1, -1, -1 };
        size_t const first = PickInstance(left);
        left[first] = 6000;
        size_t const second = PickInstance(left);
        check(first == 0 && second == 1, "AT14: the second Raking Wounds takes a free instance, the first untouched");
        left[second] = 6000;
        left[first] = 4000;   // the first ran 2 s
        check(PickInstance({ 4000, 6000, 1000 }) == 2, "a full pool replaces the one with the least time left");
        check(SlowOf({ 30, 30 }) == 30 && SlowOf({ 30 }) == 30 && SlowOf({}) == 0, "AT14: one 30 % slow, never 60 %");
        check(EchoTick(1000, 50, 3) == 166, "Raking Wounds: 50 % of 1000 over 3 ticks");
        check(EchoTick(1000, 55, 3) == 183, "Empowered Wounds' 55 % (the modifier on the ticks)");
        check(EchoTick(0, 50, 3) == 0, "nothing dealt: no bleed");
    }

    // AT 15: two charges spent at once come back one after the other (the shared charge rule, EfClassKitLogic.h)
    {
        Logic::ChargeSpend const one = Logic::SpendCharge(0, 2);
        check(one.recharging == 1 && one.left, "AT15: one of two spent: one left, one recharging");
        Logic::ChargeSpend const two = Logic::SpendCharge(1, 2);
        check(two.recharging == 2 && !two.left, "AT15: both spent: two recharging, the cooldown until the first");
    }

    // AT 16: Brutal Cleave on five: Sweeping Momentum 5 stacks of 5 % (+25 %) on the next Retreat
    check(HitStacks(5, 5) == 5 && HitStacks(7, 5) == 5 && HitStacks(1, 5) == 1 && HitStacks(0, 5) == 0,
          "AT16: a stack an enemy struck, at most 5");

    // AT 17 and 18: Unleash: the companion runs from beyond 25 yd, charges from within; the command lapses with no target
    check(PetCommand(true, true, true, 38.0f, 25.0f, true, 0, 10000) == PetStep::Run, "AT17: 38 yd: it runs");
    check(PetCommand(true, true, true, 24.0f, 25.0f, true, 1400, 10000) == PetStep::Charge, "AT17: within 25 yd: it charges");
    check(PetCommand(true, true, true, 20.0f, 25.0f, false, 0, 10000) == PetStep::Run, "AT17: out of sight: it runs on");
    check(PetCommand(true, false, true, 10.0f, 25.0f, true, 0, 10000) == PetStep::Lapse, "AT18: the target died: nothing");
    check(PetCommand(false, true, true, 10.0f, 25.0f, true, 0, 10000) == PetStep::Lapse, "AT18: the companion died: nothing");
    check(PetCommand(true, true, true, 40.0f, 25.0f, true, 10200, 10000) == PetStep::Lapse, "AT18: never reached: lapses");
    check(PetCommand(true, true, false, 10.0f, 25.0f, true, 0, 10000) == PetStep::Wait, "a stunned companion waits");

    // AT 19: two empowered swings, then the ordinary; twice as fast for 5 s
    {
        Swing const a = SwingBonus(100, 30, 2);
        Swing const b = SwingBonus(100, 30, a.stacksLeft);
        Swing const c = SwingBonus(100, 30, b.stacksLeft);
        check(a.damage == 130 && b.damage == 130 && c.damage == 100 && c.stacksLeft == 0, "AT19: Pack Ferocity: two swings +30 %");
        check(HastedSwingMs(2000, 100) == 1000, "AT19: Hunting Frenzy: twice the frequency");
    }

    std::cout << "classkit ranger tests: " << (g_checks - g_failed) << "/" << g_checks << " passed\n";
    return g_failed ? 1 : 0;
}
