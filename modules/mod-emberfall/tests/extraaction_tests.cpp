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

// Standalone tests of the extra action rule (src/extraaction/EfExtraActionLogic.h; tests/run_tests.sh builds and runs
// them): only a dummy aura with the marker's misc value grants a spell, and only its own trigger spell.

#include "EfExtraActionLogic.h"

#include <iostream>
#include <string>
#include <vector>

using namespace Emberfall::ExtraAction;

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

    MarkerEffect Effect(uint32_t aura, int32_t misc, uint32_t trigger)
    {
        MarkerEffect e;
        e.auraType = aura;
        e.miscValue = misc;
        e.triggerSpell = trigger;
        return e;
    }
}

int main()
{
    constexpr uint32_t kHelpUp = 99370;
    check(kMarker == 0x45464541, "the marker is EFEA (the client and the pipeline use the same number)");
    check(GrantedSpell(Effect(kAuraDummy, kMarker, kHelpUp)) == kHelpUp, "a marker grants its trigger spell");
    check(GrantedSpell(Effect(kAuraDummy, 0, kHelpUp)) == 0, "a plain dummy aura with a trigger spell grants nothing");
    check(GrantedSpell(Effect(42, kMarker, kHelpUp)) == 0, "another aura type with the marker's misc grants nothing");
    check(GrantedSpell(Effect(kAuraDummy, kMarker, 0)) == 0, "a marker without a spell grants nothing");

    std::vector<MarkerEffect> carried{ Effect(3, 0, 0), Effect(kAuraDummy, 7, 12345), Effect(kAuraDummy, kMarker, kHelpUp) };
    check(Grants(carried, kHelpUp), "the marker among other auras grants its spell");
    check(!Grants(carried, 12345), "an unmarked dummy's trigger spell is not granted");
    check(!Grants(carried, 0), "spell 0 is never granted");
    check(!Grants(std::vector<MarkerEffect>{}, kHelpUp), "no auras: nothing granted");

    std::cout << "extraaction_tests: " << g_checks - g_failed << "/" << g_checks << " passed\n";
    return g_failed ? 1 : 0;
}
