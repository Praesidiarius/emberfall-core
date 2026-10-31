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

// Standalone tests of the Hunter Spirit Caller v2.0's arithmetic (src/classkit/EfSpiritLogic.h; tests/run_tests.sh builds
// and runs them): the Rabbit's refresh window, Focused Fury's exact counts, Desperate Renewal's ticks, Undying Spirit's
// fatal hit and the healing left, Unbroken Spirit's cap, Serpent's Judgment's bite. The numbers are ankarna's (Emberfall
// docs/design/classes/hunter-spiritcaller.md, his "Spirit Caller - Developer Handoff v2.0", 2026-10-31).

#include "EfSpiritLogic.h"

#include <iostream>
#include <string>

using namespace Emberfall::ClassKit::SpiritLogic;

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
    // Spirit of the Rabbit: not again while more than 3 s of it are left
    check(MayRefresh(0, 3000), "no buff: always");
    check(MayRefresh(3000, 3000) && MayRefresh(1200, 3000), "3 s or less left: renewed");
    check(!MayRefresh(3001, 3000) && !MayRefresh(20000, 3000), "more than 3 s left: refused");

    // Focused Fury: +100 % on exactly one, +50 % on each of exactly two, nothing on three or more
    check(ExactCountPct(1, 1, 100) == 100 && ExactCountPct(1, 2, 50) == 0, "one enemy: the first rule only");
    check(ExactCountPct(2, 1, 100) == 0 && ExactCountPct(2, 2, 50) == 50, "two enemies: the second rule only");
    check(ExactCountPct(3, 1, 100) + ExactCountPct(3, 2, 50) == 0, "three: normal damage");
    check(ExactCountPct(0, 1, 100) == 0 && ExactCountPct(1, 0, 100) == 0, "nothing struck, or no count: nothing");

    // Desperate Renewal: the ticks doubled (5 % -> 10 %, 20 % -> 40 % over 6 s) when cast at 50 % or below
    check(LowHealthTick(5, 50.0f, 50, 100) == 10 && LowHealthTick(5, 12.5f, 50, 100) == 10, "at or below 50 %: doubled");
    check(LowHealthTick(5, 50.1f, 50, 100) == 5 && LowHealthTick(5, 90.0f, 50, 100) == 5, "above 50 %: as it is");
    check(LowHealthTick(5, 10.0f, 0, 100) == 5 && LowHealthTick(5, 10.0f, 50, 0) == 5, "no threshold or no bonus");

    // Undying Spirit: the fatal hit leaves 1 health; what the ticks had left is healed at once
    check(FatalAbsorb(500, 400) == 101, "500 on 400 health: 101 absorbed, 1 health left");
    check(FatalAbsorb(400, 400) == 1, "exactly the health: 1 absorbed");
    check(FatalAbsorb(399, 400) == 0, "not fatal: nothing");
    check(TicksLeft(4, 1) == 3 && TicksLeft(4, 4) == 0 && TicksLeft(4, 6) == 0, "ticks left of 4");
    check(HealLeft(10000, 5, 3) == 1500, "3 ticks of 5 % of 10000: 1500");
    check(HealLeft(10000, 10, 4) == 4000, "Desperate Renewal's ticks untouched: 4 x 10 % = 40 %");
    check(HealLeft(10000, 0, 4) == 0, "no ticks' worth");

    // Unbroken Spirit: no hit above 30 % of max health in Nature's Wrath (max health 13000 with its +30 %)
    check(OverCap(5000, 13000, 30) == 1100, "5000 against a 3900 cap: 1100 absorbed");
    check(OverCap(3900, 13000, 30) == 0 && OverCap(100, 13000, 30) == 0, "at or below the cap: nothing");
    check(OverCap(5000, 13000, 0) == 0, "no cap: nothing");

    // Serpent's Judgment: 50 % of the venom left (ticks of 40, 3 of 5 left: 120 left, 60 bitten)
    check(BurstOfLeft(40, 3, 50) == 60, "60 of the 120 left");
    check(BurstOfLeft(40, 0, 50) == 0, "the venom had nothing left");
    check(BurstOfLeft(0, 3, 50) == 0 && BurstOfLeft(40, 3, 0) == 0, "no tick or no percent: nothing");

    std::cout << "classkit spirit tests: " << (g_checks - g_failed) << "/" << g_checks << " passed\n";
    return g_failed ? 1 : 0;
}
