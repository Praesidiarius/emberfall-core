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

// Standalone tests of the quest flight rule (src/questflights/EfQuestFlightLogic.h; tests/run_tests.sh builds and runs
// them): which first talk to a roost-keeper shows the menu instead of the flight map, and the new flight path line.

#include "EfQuestFlightLogic.h"

#include <cstring>
#include <iostream>
#include <string>

using namespace Emberfall::QuestFlight;

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

    constexpr uint32_t kGossip = 1;
}

int main()
{
    Line const taxi{kTaxiOption, true};
    Line const flightShown{kGossip, true};
    Line const flightHidden{kGossip, false};

    // the fault of 2026-11-22: an unknown roost with a quest flight on offer
    check(ShowsMenuOnFirstTalk(true, 94, false, {taxi, flightShown}),
          "an unknown roost, the quest held: the menu (quest flight and map line), not the map");
    check(ShowsMenuOnFirstTalk(true, 94, false, {flightShown, taxi}), "the line's place in the menu does not matter");
    // as today everywhere else
    check(!ShowsMenuOnFirstTalk(true, 94, false, {taxi, flightHidden}),
          "an unknown roost, the quest not held: the core's way (learned, the map at once)");
    check(!ShowsMenuOnFirstTalk(true, 94, false, {taxi}), "an unknown roost with only the taxi line: the core's way");
    check(!ShowsMenuOnFirstTalk(true, 94, true, {taxi, flightShown}),
          "a known roost: the core's own menu (it shows every line then)");
    check(!ShowsMenuOnFirstTalk(false, 94, false, {flightShown}), "not a flight master: the core's way");
    check(!ShowsMenuOnFirstTalk(true, 0, false, {taxi, flightShown}), "no node near the keeper: the core's way");
    check(!ShowsMenuOnFirstTalk(true, 94, false, {Line{kTaxiOption, true}, Line{kTaxiOption, true}}),
          "two taxi lines are still only the map");
    check(!ShowsMenuOnFirstTalk(true, 94, false, {}), "no menu lines: the core's way");

    // the new flight path line in the client's three languages (client/interface/Strings/*/Npc.lua ERR_NEW_TAXI_PATH)
    check(std::strcmp(NewFlightPathText(0), "You have found a new flight path.") == 0, "enUS");
    check(std::strcmp(NewFlightPathText(3), "Du hast eine neue Flugroute entdeckt.") == 0, "deDE");
    check(std::strcmp(NewFlightPathText(2), "Nouvelle route a\xC3\xA9rienne d\xC3\xA9" "couverte.") == 0, "frFR");
    check(std::strcmp(NewFlightPathText(6), NewFlightPathText(0)) == 0, "another locale reads English");

    std::cout << "quest flight tests: " << (g_checks - g_failed) << "/" << g_checks << " passed\n";
    return g_failed ? 1 : 0;
}
