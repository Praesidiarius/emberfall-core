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

// Standalone tests of the bots' seat rule (mod-playerbots' src/Emberfall/EmberfallRestLogic.h; tests/run_tests.sh
// builds and runs them): a resting bot keeps its seat, a rested one, one in a fight or one with the rule off stands.

#include "EmberfallRestLogic.h"

#include <iostream>
#include <string>

using namespace EmberfallRest::Logic;

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

    SeatState Seated()
    {
        SeatState s;
        s.seated = true;
        return s;
    }
}

int main()
{
    SeatState s = Seated();
    s.eating = true;
    s.restedEnough = true;
    check(KeepsSeat(s), "a bot that eats keeps its seat, even at the rest's end (the food runs out by itself)");

    s = Seated();
    s.drinking = true;
    s.restedEnough = true;
    check(KeepsSeat(s), "a bot that drinks keeps its seat");

    s = Seated();
    check(KeepsSeat(s), "a seated bot not rested enough keeps its seat (no food it can use: it rests seated)");

    s = Seated();
    s.restedEnough = true;
    check(!KeepsSeat(s), "a seated bot that is rested and eats nothing stands up as before");

    s = Seated();
    s.eating = true;
    s.inCombat = true;
    check(!KeepsSeat(s), "a fight stands a bot up, even mid-meal");

    s = Seated();
    s.on = false;
    check(!KeepsSeat(s), "with Playerbots.Emberfall.RestSeated 0 (or a bot without a spec) the stock stand-up applies");

    s = SeatState();
    s.eating = true;
    check(!KeepsSeat(s), "a standing bot has no seat to keep");

    std::cout << "botrest: " << g_checks - g_failed << " of " << g_checks << " checks passed\n";
    return g_failed ? 1 : 0;
}
