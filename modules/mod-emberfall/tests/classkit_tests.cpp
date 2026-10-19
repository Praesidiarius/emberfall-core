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

    std::cout << "classkit tests: " << (g_checks - g_failed) << "/" << g_checks << " passed\n";
    return g_failed ? 1 : 0;
}
