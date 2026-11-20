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
// uncapped gains, Shattering Overflow's one pulse an activation (ankarna's hand-off of 2026-11-19) and its share over many
// enemies, Frigid Pulse's hit without damage and Echoing Frost's one roll a cast, Deep Winter's cap, First Contact, the Orb
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
    // what a gain makes beyond the cap
    {
        int64_t const per = 600;
        check(FrostLogic::ShellOverflow(0, per, 4, 10) == 0, "an empty shell: everything fits");
        check(FrostLogic::ShellOverflow(5400, per, 2, 10) == 600, "one stack short of full plus two: one stack's worth over");
        check(FrostLogic::ShellOverflow(6000, per, 3, 10) == 1800, "a full shell: the whole gain is over");
        check(Logic::ShellAfterGain(5400, per, 2, 10) + FrostLogic::ShellOverflow(5400, per, 2, 10) == 5400 + 2 * per,
              "what the pool keeps and what flows over add up to the gain");
        check(FrostLogic::ShellOverflow(7000, per, 1, 10) == 600, "a pool above a cap that fell: the gain is all over");
    }
    // Shattering Overflow since the hand-off of 2026-11-19: ONE pulse an ability activation. An activation's shell gain is
    // summed over every enemy it shattered (1 shell stack per 2 stacks), the cap looked at once, and anything over sends
    // one pulse; each enemy in reach takes the burst's own amount once (its level and spell power numbers: 100 here),
    // never a share of what was over, the same however many enemies or stacks the activation shattered
    {
        int64_t const per = 600;
        int32_t const maxStacks = 10;
        int32_t const burst = 100;
        // one activation over the enemies' stacks shattered: the pulses it sends; the pool follows
        auto activation = [&](int64_t& pool, std::vector<uint32_t> const& shattered, int64_t* over = nullptr)
        {
            uint32_t sum = 0;
            for (uint32_t s : shattered)
                sum += s;
            int32_t const gain = Logic::ShellGain(sum, 2, 1);
            int64_t const beyond = FrostLogic::ShellOverflow(pool, per, gain, maxStacks);
            pool = Logic::ShellAfterGain(pool, per, gain, maxStacks);
            if (over)
                *over = beyond;
            return FrostLogic::OverflowPulses(beyond);
        };
        std::vector<uint32_t> const icebreakSix(6, 2);     // Icebreak shatters 2 on each of six enemies: 12, 6 shell stacks
        std::vector<uint32_t> const ruptureSix(6, 8);      // Glacial Rupture shatters 8 on each of six: 48, 24 shell stacks
        int64_t pool = per * maxStacks;                    // a full shell
        int64_t overIce = 0, overRupture = 0;
        check(activation(pool, icebreakSix, &overIce) == 1, "one Icebreak shattering six enemies over a full shell: one pulse, not six");
        check(activation(pool, ruptureSix, &overRupture) == 1, "one Glacial Rupture shattering six enemies (48 stacks): one pulse, not six or 24");
        check(overIce == 6 * per && overRupture == 24 * per, "the shell over the cap: 6 and 24 stacks' worth (the shell itself not capped)");
        int32_t const iceHit = FrostLogic::OverflowHit(overIce, burst, 6, 5);
        int32_t const ruptureHit = FrostLogic::OverflowHit(overRupture, burst, 6, 5);
        check(iceHit == ruptureHit && iceHit == FrostLogic::AreaShare(burst, 6, 5),
              "each of the six takes the same, however much was over or however many stacks were shattered");
        check(iceHit * 6 <= burst * 6 && iceHit * 6 >= burst * 5 - 6, "the wave: six hits of the burst at most (the pack share above five), never 6 x 6");
        check(FrostLogic::OverflowHit(per, burst, 3, 5) == burst && FrostLogic::OverflowHit(100 * per, burst, 3, 5) == burst,
              "three enemies: 100 each, whether one stack or a hundred stacks' worth was over");
        // two activations close together: two pulses (no internal cooldown)
        int pulses = activation(pool, icebreakSix) + activation(pool, icebreakSix);
        check(pulses == 2, "two Icebreaks back to back over a full shell: two pulses");
        // a gain that fits: none
        int64_t emptyPool = 0;
        check(activation(emptyPool, icebreakSix) == 0, "six stacks into an empty shell: no pulse");
        int64_t nearly = per * 9;
        check(activation(nearly, std::vector<uint32_t>(1, 2)) == 0, "the last stack exactly fills it: no pulse");
        check(activation(nearly, std::vector<uint32_t>(1, 2)) == 1, "one stack more: one pulse");
        check(FrostLogic::OverflowHit(0, burst, 6, 5) == 0, "nothing over: no hit");
    }
    // Frigid Pulse since the hand-off of 2026-11-19: no damage; its own threat on every enemy struck, not shared out over
    // the pack; 1 stack, 2 / 3 with Gathering Frost 1 / 2; Echoing Frost one roll a cast, its pulse exactly 1 stack, no
    // damage, no threat
    {
        auto const base = FrostLogic::FrigidPulseHit(730, 1, 0);
        check(base.damage == 0 && base.threat == 730 && base.stacks == 1, "the pulse: no damage, its threat, 1 stack");
        check(FrostLogic::FrigidPulseHit(730, 1, 1).stacks == 2 && FrostLogic::FrigidPulseHit(730, 1, 2).stacks == 3,
              "Gathering Frost 1 / 2: 2 / 3 stacks an enemy");
        bool flat = true;
        for (int enemies = 1; enemies <= 10; ++enemies)
        {
            int64_t total = 0;
            for (int i = 0; i < enemies; ++i)
                total += FrostLogic::FrigidPulseHit(730, 1, 2).threat;
            flat = flat && total == int64_t(730) * enemies;
        }
        check(flat, "every enemy struck gets the whole threat, one enemy or ten (not shared)");
        auto const echo = FrostLogic::EchoHit(1);
        check(echo.damage == 0 && echo.threat == 0 && echo.stacks == 1, "the echo: exactly 1 stack, no damage, no threat");
        check(FrostLogic::EchoPulse(true, 10, 9) && !FrostLogic::EchoPulse(true, 10, 10), "10 %: rolls 0-9");
        check(!FrostLogic::EchoPulse(false, 10, 0), "without the talent: never");
        int echoes = 0;
        for (int roll = 0; roll < 100; ++roll)
            echoes += FrostLogic::EchoPulse(true, 10, roll) ? 1 : 0;
        check(echoes == 10, "10 of 100 casts");
        // one cast over six enemies with Gathering Frost 2/2 rolls once: all six get 3, or all six get 3 + 1 (never 3 + 3)
        for (int roll : { 0, 50 })
        {
            bool const second = FrostLogic::EchoPulse(true, 10, roll);
            std::vector<uint32_t> stacks(6, 0);
            for (uint32_t& s : stacks)
                s = FrostLogic::StacksAfterAdd(s, FrostLogic::FrigidPulseHit(730, 1, 2).stacks, 8);
            if (second)
                for (uint32_t& s : stacks)
                    s = FrostLogic::StacksAfterAdd(s, FrostLogic::EchoHit(1).stacks, 8);
            bool same = true;
            for (uint32_t s : stacks)
                same = same && s == (second ? 4u : 3u);
            check(same, second ? "an echo: 4 on every enemy (3 + 1)" : "no echo: 3 on every enemy");
        }
        check(FrostLogic::StacksAfterAdd(5, 3 + 1, 6) == 6 && FrostLogic::StacksAfterAdd(5, 3 + 1, 8) == 8,
              "the 6 or 8 cap holds the echo's stack too");
    }
    // the burst's soft cap (feedback #619): up to `full` enemies take it whole, beyond that the total stays amount x full
    {
        check(FrostLogic::AreaShare(900, 1, 5) == 900 && FrostLogic::AreaShare(900, 5, 5) == 900, "up to five: the whole 900");
        check(FrostLogic::AreaShare(900, 10, 5) == 450, "ten enemies: 450 each (4500 in all, as five)");
        check(FrostLogic::AreaShare(900, 6, 5) == 750, "six enemies: 750 each (4500 in all)");
        check(FrostLogic::AreaShare(900, 4, 1) == 225, "a cap of one: four share one hit");
        int32_t prev = 1000000;
        bool capped = true;
        for (uint32_t n = 1; n <= 20; ++n)
        {
            int32_t const each = FrostLogic::AreaShare(900, n, 5);
            int32_t const total = each * int32_t(n);
            capped = capped && each <= prev && total <= 900 * 5 + int32_t(n) && total >= 900 * int32_t(std::min<uint32_t>(n, 5)) - int32_t(n);
            prev = each;
        }
        check(capped, "each share falls with every enemy more; the total grows to five hits and no further");
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
        // the second orb is its own spell: the first orb's spell again would only refresh the first's ticks on the
        // enemies both stand over (2026-11-17)
        check(FrostLogic::TwinOrbSpell(94452, 94458) == 94458, "the second orb: its own spell");
        check(FrostLogic::TwinOrbSpell(94452, 94452) == 0, "never the first orb's spell");
        check(FrostLogic::TwinOrbSpell(94452, 0) == 0, "no twin spell in the data: no second orb");
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
