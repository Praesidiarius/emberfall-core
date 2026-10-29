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

// Standalone tests of the core's miss rules (src/server/game/Spells/EmberfallMissRules.h, PATCHES.md #26; Emberfall alpha1
// feedback #142 and #146; tests/run_tests.sh builds and runs them): a spender pays in full on a miss with own data, as
// upstream without; a strike's energize on its caster needs a struck unit, a self spell's never does; creatures keep
// upstream's way.

#include "EmberfallMissRules.h"

#include <cstdint>
#include <iostream>
#include <string>

using namespace Emberfall::MissRules;

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

    void testCost()
    {
        check(!PaysLessOnMiss(true), "own data: a dodged Final Verdict takes its 3 Openings (#142)");
        check(!PaysLessOnMiss(true), "own data: a parried Savage Cleave takes its whole Battlelust");
        check(PaysLessOnMiss(false), "stock data: upstream's share of the cost back on a miss");
    }

    void testEnergize()
    {
        // own data, a player's strike
        check(EnergizesCaster(true, true, true, 1), "Cross Cut that struck: its rage");
        check(EnergizesCaster(true, true, true, 4), "a sweep that struck four: its power once");
        check(!EnergizesCaster(true, true, true, 0), "Cross Cut dodged, parried or missed: no rage (#146)");
        check(!EnergizesCaster(true, true, true, 0), "Patient Shot at an immune or evading enemy: no Focus");
        // a self spell: always
        check(EnergizesCaster(true, true, false, 0), "a self spell that gives power (no enemy effect) always gives");
        // creatures and stock data: upstream
        check(EnergizesCaster(true, false, true, 0), "a creature's strike keeps upstream's energize");
        check(EnergizesCaster(false, true, true, 0), "stock data: upstream's energize on a miss");
    }
}

int main()
{
    testCost();
    testEnergize();
    std::cout << "miss rules tests: " << (g_checks - g_failed) << "/" << g_checks << " passed\n";
    return g_failed == 0 ? 0 : 1;
}
