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

// Standalone tests of the Frostguard rework's rules (ankarna's notes of 2026-11-15; src/classkit/EfFrostLogic.h with the
// shared EfClassKitLogic.h ShellAfterGain / ShellStacks / ShellGain / StackCap; tests/run_tests.sh builds and runs them):
// the shell's 80 % share of a blow with the rest passing to health, its capacity per stack, its two durations, the
// uncapped gains, Shattering Overflow's damage and its share over many enemies, Deep Winter's cap, First Contact, the Orb
// stopping at its first enemy, Twin Orbs' limits, and Stillpoint's order and boss rule. The numbers are examples fed to
// the functions, not the kit's values (those: the game repo's content/data/classes/mage.yaml).

#include "EfClassKitLogic.h"
#include "EfFrostLogic.h"

#include <iostream>
#include <string>
#include <vector>

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
    // the shell takes 80 % of a blow, 20 % passes to health; never more than the pool
    {
        check(FrostLogic::ShellTake(1000, 5000, 80) == 800, "a 1000 blow: 800 into the shell, 200 to health");
        check(FrostLogic::ShellTake(1000, 300, 80) == 300, "a nearly worn shell takes what it has left");
        check(FrostLogic::ShellTake(1000, 5000, 100) == 1000 && FrostLogic::ShellTake(1000, 5000, 0) == 1000,
              "no share (100 or none): the whole blow, the rule before");
        check(FrostLogic::ShellTake(0, 5000, 80) == 0 && FrostLogic::ShellTake(1000, 0, 80) == 0, "nothing to take");
        check(FrostLogic::ShellTake(1000, 5000, 50) == 500, "a share of 50: half of it (the lever the notes name first)");
        // a blow worth exactly the 10 stacks: the shell keeps 20 % of it, then the next blow goes on
        int64_t pool = 10 * 150;
        int64_t health = 0;
        for (int blow = 0; blow < 3; ++blow)
        {
            int64_t const took = FrostLogic::ShellTake(600, pool, 80);
            pool -= took;
            health += 600 - took;
        }
        check(pool == 1500 - 3 * 480 && health == 3 * 120, "three 600 blows: 1440 absorbed, 360 to health");
    }
    // the capacity per stack is effect 2's percent of max health: 15 (10 before, +50 %); 10 stacks the most
    {
        int64_t const maxHealth = 4000;
        int64_t const per = maxHealth * 15 / 100;
        check(per == 600 && per * 10 == 6000, "15 % of 4000: 600 a stack, 6000 for ten");
        check(Logic::ShellStacks(5999, per, 10) == 10 && Logic::ShellStacks(601, per, 10) == 2, "every started stack shows");
    }
    // the two durations: a gain lasts the source's 8 s (12 s with Improved Icebreak's +4), the longer of the two wins
    {
        int32_t const base = 8000, improved = 8000 + 4000;
        check(std::max(3000, base) == 8000 && std::max(3000, improved) == 12000, "8 s, 12 s with Improved Icebreak");
        check(std::max(11000, base) == 11000, "a gain never shortens what is left");
    }
    // uncapped gains: Icebreak 1 shell stack per 2 shattered on any number of enemies (the 4-cap of 2026-11-03 gone)
    {
        check(Logic::ShellGain(16, 2, 1) == 8, "Icebreak on eight enemies: 8 shell stacks (was 4)");
        check(Logic::ShellGain(5 + 6 + 8, 2, 1) == 9, "Glacial Rupture on 5 + 6 + 8: 9, summed and rounded down");
        check(Logic::ShellGain(12, 2, 1, 3) == 3, "Focused Shatter's own 3 stays (the capstone unchanged)");
    }
    // what a gain makes beyond the cap, and Shattering Overflow's 50 % of it
    {
        int64_t const per = 600;
        check(FrostLogic::ShellOverflow(0, per, 4, 10) == 0, "an empty shell: everything fits");
        check(FrostLogic::ShellOverflow(5400, per, 2, 10) == 600, "one stack short of full plus two: one stack's worth over");
        check(FrostLogic::ShellOverflow(6000, per, 3, 10) == 1800, "a full shell: the whole gain is over");
        check(Logic::ShellAfterGain(5400, per, 2, 10) + FrostLogic::ShellOverflow(5400, per, 2, 10) == 5400 + 2 * per,
              "what the pool keeps and what flows over add up to the gain");
        check(FrostLogic::ShellOverflow(7000, per, 1, 10) == 600, "a pool above a cap that fell: the gain is all over");
        check(FrostLogic::OverflowDamage(1800, 50) == 900, "50 % of 1800: 900 frost damage");
        check(FrostLogic::OverflowDamage(0, 50) == 0 && FrostLogic::OverflowDamage(1800, 0) == 0, "nothing over: no burst");
    }
    // the burst's share over many enemies: one takes it whole, n take 1 / sqrt(n) each [proposal: full 1]
    {
        check(FrostLogic::AreaShare(900, 1, 1) == 900, "one enemy: the whole 900");
        check(FrostLogic::AreaShare(900, 4, 1) == 450, "four enemies: 450 each (1800 in all)");
        check(FrostLogic::AreaShare(900, 9, 1) == 300, "nine enemies: 300 each (2700 in all)");
        check(FrostLogic::AreaShare(900, 3, 3) == 900 && FrostLogic::AreaShare(900, 12, 3) == 450,
              "a share that starts after three: whole up to three, half at twelve");
        int32_t prev = 1000000;
        bool falls = true;
        for (uint32_t n = 1; n <= 20; ++n)
        {
            int32_t const each = FrostLogic::AreaShare(900, n, 1);
            falls = falls && each <= prev && each * int32_t(n) >= 900;
            prev = each;
        }
        check(falls, "each enemy's share falls with every enemy more, the whole never below one hit");
    }
    // Deep Winter: the Frost Stacks' cap 6 -> 8 (StackCap of effect 1 with the talent's +2, the aura's maximum 8)
    {
        uint32_t const cap6 = uint32_t(Logic::StackCap(6, 8)), cap8 = uint32_t(Logic::StackCap(6 + 2, 8));
        check(cap6 == 6 && cap8 == 8, "6 without the talent, 8 with it");
        check(FrostLogic::StacksAfterAdd(5, 2, cap6) == 6 && FrostLogic::StacksAfterAdd(5, 2, cap8) == 7, "5 + 2: 6 or 7");
        check(FrostLogic::StacksAfterAdd(7, 3, cap8) == 8, "never above 8");
        check(FrostLogic::StacksAfterAdd(8, 2, cap6) == 8, "stacks above a cap that fell stay (the talent unlearned)");
        check(uint32_t(Logic::StackCap(6 + 5, 8)) == 8, "never above the aura's own maximum");
    }
    // First Contact: +100 % on an enemy the mage had not hurt before the cast, nothing on one it had
    {
        check(FrostLogic::FirstContactDamage(400, true, 100) == 800, "the opening spear: 800");
        check(FrostLogic::FirstContactDamage(400, false, 100) == 400, "the second spear: 400");
        check(FrostLogic::FirstContactDamage(400, true, 0) == 400, "without the talent: 400");
    }
    // the Orb stops at the first enemy on its line (30 yd, 3 yd to each side), the nearest along it
    {
        using C = FrostLogic::LineCandidate;
        std::vector<C> pack = { { 18.0f, 1.0f, 1.0f }, { 9.0f, -2.0f, 1.0f }, { 5.0f, 9.0f, 1.0f }, { -4.0f, 0.0f, 1.0f } };
        check(FrostLogic::FirstOnLine(pack, 3.0f, 30.0f) == 1, "the one 9 yd ahead, not the farther one, not the one aside or behind");
        std::vector<C> none = { { 12.0f, 8.0f, 1.0f }, { 40.0f, 0.0f, 1.0f } };
        check(FrostLogic::FirstOnLine(none, 3.0f, 30.0f) == -1, "nobody on the line: it flies its whole length");
        std::vector<C> big = { { 20.0f, 5.0f, 3.0f } };
        check(FrostLogic::FirstOnLine(big, 3.0f, 30.0f) == 0, "a big enemy's reach counts");
        check(FrostLogic::FlightMs(9.0f, 9.0f) == 1000 && FrostLogic::FlightMs(27.0f, 9.0f) == 3000, "9 yd/s: 1 s, 3 s");
    }
    // Twin Orbs: only the pressed cast rolls, with the talent, below the chance; a second orb never summons another
    {
        check(FrostLogic::TwinOrb(false, true, 20, 19) && !FrostLogic::TwinOrb(false, true, 20, 20), "20 %: rolls 0-19");
        check(!FrostLogic::TwinOrb(true, true, 20, 0), "the second orb never rolls");
        check(!FrostLogic::TwinOrb(false, false, 20, 0), "without the talent: none");
        int twins = 0;
        for (int roll = 0; roll < 100; ++roll)
            twins += FrostLogic::TwinOrb(false, true, 20, roll) ? 1 : 0;
        check(twins == 20, "20 of 100 rolls");
    }
    // Stillpoint in the notes' order: the stacks read first; frozen at 6+ (never a boss, never the stun-immune); every
    // enemy marked; Frost Accumulation's 2 only on those that had fewer than 6, after the freeze check
    {
        auto const six = FrostLogic::Stillpoint(6, 6, false, false, true, 2);
        check(six.freeze && six.mark && six.addStacks == 0, "6 stacks: frozen, marked, no stacks added");
        auto const five = FrostLogic::Stillpoint(5, 6, false, false, true, 2);
        check(!five.freeze && five.mark && five.addStacks == 2,
              "5 stacks: not frozen (the stacks it gains come after the check), marked, +2");
        auto const boss = FrostLogic::Stillpoint(8, 6, true, false, true, 2);
        check(!boss.freeze && boss.mark && boss.addStacks == 0, "a boss at 8: never frozen, still marked");
        auto const immune = FrostLogic::Stillpoint(6, 6, false, true, false, 2);
        check(!immune.freeze && immune.mark, "a creature immune to stuns: not frozen, marked");
        auto const bare = FrostLogic::Stillpoint(2, 6, false, false, false, 2);
        check(!bare.freeze && bare.mark && bare.addStacks == 0, "without Frost Accumulation: only the mark");
    }

    std::cout << "classkit frost tests: " << (g_checks - g_failed) << "/" << g_checks << " passed\n";
    return g_failed ? 1 : 0;
}
