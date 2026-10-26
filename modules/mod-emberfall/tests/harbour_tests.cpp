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

// Standalone tests of the harbourmaster's logic (src/harbours/EfHarbourLogic.h; tests/run_tests.sh builds and runs
// them): the pier's stop, the ship at the pier or due, the wrap at the period's end, the wording.

#include "EfHarbourLogic.h"

#include <iostream>
#include <string>

using namespace Emberfall::Harbours;

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

    // The war ferry's two stops (Emberfall's design: period 283 s; the first berth at the period's start). World
    // coordinates after the world scale of 2026-10-26 (harbours moved rigidly).
    std::vector<Stop> const kStops = {
        { 0, -618.3f, -5000.48f, 0, 50000 },        // the War-Ferry Stage (map 0)
        { 1, 465.98f, 33.4f, 142000, 192000 },  // the Ferry Hards (map 1)
    };
    constexpr uint32_t kPeriod = 283000;
}

int main()
{
    // the pier's stop: same map, within reach
    auto at0 = BerthNear(kStops, 0, -613.2f, -5004.98f);
    check(at0 && at0->arrive == 0, "the stage's harbourmaster finds the stage's stop");
    auto at1 = BerthNear(kStops, 1, 450.08f, 26.0f);
    check(at1 && at1->arrive == 142000, "the Ferry Hards' harbourmaster finds its stop");
    check(!BerthNear(kStops, 0, 465.98f, 33.4f), "a stop on the other map is not this pier's");
    check(!BerthNear(kStops, 0, -180.45f, -5086.01f), "a stop 450 yd away is not this pier's");

    // at the pier
    Sailing s = NextSailing(kStops[0], 10000, kPeriod);
    check(s.atPier && s.untilDepart == 40, "10 s into the stage's wait: at the pier, sails in 40 s");
    s = NextSailing(kStops[1], 150500, kPeriod);
    check(s.atPier && s.untilDepart == 42, "8.5 s into the Hards' wait: sails in 42 s (rounded up)");
    // due
    s = NextSailing(kStops[1], 60000, kPeriod);
    check(!s.atPier && s.untilArrive == 82 && s.untilDepart == 132, "at 60 s the Hards' ship is due in 82 s, sails 50 s later");
    // the wrap: the stage's stop at the period's start is reached again at its end
    s = NextSailing(kStops[0], 250000, kPeriod);
    check(!s.atPier && s.untilArrive == 33, "at 250 s the stage's ship is due in 33 s (the period wraps)");
    s = NextSailing(kStops[0], 283000 * 5 + 1000, kPeriod);
    check(s.atPier && s.untilDepart == 49, "a progress past several periods wraps");
    s = NextSailing(kStops[0], 1000, 0);
    check(!s.atPier && s.untilArrive == 0, "a period of 0 says nothing");

    // the wording
    check(Duration(45) == "45 s", "45 s");
    check(Duration(80) == "1 min 20 s", "1 min 20 s");
    check(Duration(120) == "2 min", "2 min");
    check(Describe("The War Ferry", NextSailing(kStops[0], 10000, kPeriod), 50) == "The War Ferry is at the pier: it sails in 40 s.",
          "at the pier, in words");
    check(Describe("The War Ferry", NextSailing(kStops[1], 60000, kPeriod), 50) ==
              "The War Ferry is due in 1 min 22 s and sails 50 s after.",
          "due, in words");

    std::cout << "harbour tests: " << (g_checks - g_failed) << "/" << g_checks << " passed\n";
    return g_failed ? 1 : 0;
}
