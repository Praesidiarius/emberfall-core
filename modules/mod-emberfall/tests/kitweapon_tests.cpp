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

// Standalone tests of the core's class kit weapon strikes (src/server/game/Spells/EmberfallKitWeapon.h, PATCHES.md #28;
// Emberfall docs/design/classes.md §4.8; tests/run_tests.sh builds and runs them): only the kits' families, the misc
// value as the weapon's percent, the points added after it.

#include "EmberfallKitWeapon.h"

#include <cstdint>
#include <iostream>
#include <string>

using namespace Emberfall::KitWeapon;

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

    void testFamily()
    {
        check(IsKitFamily(101), "the Warrior kit (101) strikes the kit way");
        check(IsKitFamily(103) && IsKitFamily(111) && IsKitFamily(120), "the Hunter, Druid and profession families too");
        check(!IsKitFamily(0) && !IsKitFamily(4) && !IsKitFamily(17), "stock families (generic, warrior ... pet) keep upstream's way");
    }

    void testPct()
    {
        check(EffectPct(0) == 100.0f, "no misc value: 100 % of the weapon (Cross Cut's weapon damage + N)");
        check(EffectPct(45) == 45.0f, "misc 45: 45 % of the weapon");
        check(EffectPct(150) == 150.0f, "misc 150: one and a half times the weapon (Finish)");
        check(EffectPct(-5) == 100.0f, "a negative misc value is no percent");
    }

    void testStrike()
    {
        check(Strike(200, 45.0f, 12) == 102, "45 % of a 200 swing + 12 = 102");
        check(Strike(200, 100.0f, 0) == 200, "100 %, no points: the weapon's damage");
        check(Strike(170, 150.0f, 6) == 261, "150 % of 170 + 6 (the points never multiplied)");
        check(Strike(0, 45.0f, 9) == 9, "no weapon damage (disarmed): the points alone");
        check(Strike(10, 50.0f, -20) == 0, "never below 0");
        // two weapon effects on one unit multiply into one percent (the core's product), the points add
        check(Strike(300, 50.0f * 150.0f / 100.0f, 10) == 235, "50 % and a 150 % percent effect: 75 % of 300 + 10");
    }
}

int main()
{
    testFamily();
    testPct();
    testStrike();
    std::cout << "kit weapon tests: " << (g_checks - g_failed) << "/" << g_checks << " passed\n";
    return g_failed == 0 ? 0 : 1;
}
