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

// Standalone tests of the unstuck logic (src/unstuck/EfUnstuckLogic.h; tests/run_tests.sh builds and runs them): the
// nearest town on the same map, the instance's entrance, home without a town, and the rules (combat, cooldown, dead).

#include "EfUnstuckLogic.h"

#include <iostream>
#include <string>

using namespace Emberfall::Unstuck;

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

    // A few of Emberfall's town points (world.yaml): Millbrook, Hollybank, Coldharrow, Second Station; one on map 1.
    std::vector<Point> const kPoints = {
        { 90001, 0, -165.0f, -64.0f, 45.0f, 3.14f, "Millbrook" },
        { 90029, 0, -310.0f, -248.0f, 47.0f, 1.2f, "Hollybank" },
        { 90010, 0, -760.4f, -560.4f, 48.4f, 0.0f, "Coldharrow" },
        { 90022, 0, -3157.7f, -979.7f, 58.0f, 0.0f, "Second Station" },
        { 99999, 1, -170.0f, -70.0f, 45.0f, 0.0f, "Elsewhere" },
    };

    void TestNearest()
    {
        auto n = NearestTown(kPoints, 0, -170.0f, -70.0f, 40.0f);
        check(n && kPoints[*n].id == 90001, "next to Millbrook: Millbrook (not map 1's point at the same spot)");
        n = NearestTown(kPoints, 0, -700.0f, -500.0f, 50.0f);
        check(n && kPoints[*n].id == 90010, "near Coldharrow: Coldharrow");
        n = NearestTown(kPoints, 0, -3000.0f, -1200.0f, 60.0f);
        check(n && kPoints[*n].id == 90022, "in Redmere: Second Station");
        n = NearestTown(kPoints, 0, -290.0f, -230.0f, 47.0f);
        check(n && kPoints[*n].id == 90029, "in Hollybank Fields: Hollybank");
        n = NearestTown(kPoints, 1, 5000.0f, 5000.0f, 0.0f);
        check(n && kPoints[*n].id == 99999, "map 1: its only point, however far");
        check(!NearestTown(kPoints, 530, 0, 0, 0), "a map without points: none");
        check(!NearestTown({}, 0, 0, 0, 0), "no points: none");
    }

    void TestChoose()
    {
        Target t = Choose(kPoints, 0, false, false, -170.0f, -70.0f, 40.0f);
        check(t.where == Where::Town && t.town && kPoints[*t.town].id == 90001, "open world: the nearest town");
        t = Choose(kPoints, 751, true, true, 47.0f, 268.0f, 30.0f);
        check(t.where == Where::Entrance, "the Barrows (an instance with a door): the entrance");
        t = Choose(kPoints, 0, true, true, -165.0f, -64.0f, 45.0f);
        check(t.where == Where::Entrance, "an instance: the entrance, even at a town's coordinates");
        t = Choose(kPoints, 760, true, false, 0, 0, 0);
        check(t.where == Where::Home, "an instance without a door and no town: home");
        t = Choose(kPoints, 530, false, false, 0, 0, 0);
        check(t.where == Where::Home, "a map without towns: home");
    }

    void TestRules()
    {
        Ask a;
        a.now = 10000;
        check(Check(a) == Refusal::None, "alive, out of combat, never used: yes");
        a.inCombat = true;
        check(Check(a) == Refusal::Combat, "in combat: no");
        a.inCombat = false;
        a.lastUse = 10000 - 299;
        check(Check(a) == Refusal::Cooldown && Remaining(a.now, a.lastUse, a.cooldown) == 1, "299 s after a use: 1 s left");
        a.lastUse = 10000 - 300;
        check(Check(a) == Refusal::None, "300 s after a use: yes");
        a.lastUse = 10000 - 10;
        a.cooldown = 0;
        check(Check(a) == Refusal::None, "cooldown 0: no cooldown");
        a.cooldown = 300;
        a.alive = false;
        a.inCombat = true;
        check(Check(a) == Refusal::None, "dead: released to the graveyard whatever the cooldown");
        a.alive = true;
        a.inCombat = false;
        a.inFlight = true;
        check(Check(a) == Refusal::Flight, "on a flight path: no");
        a.enabled = false;
        check(Check(a) == Refusal::Off, "off: no");
        check(Message(Refusal::Cooldown, 252) == "Unstuck: you can use it again in 4 min 12 s.", "the cooldown message");
        check(Message(Refusal::Cooldown, 40) == "Unstuck: you can use it again in 40 s.", "under a minute");
        check(Message(Refusal::Cooldown, 120) == "Unstuck: you can use it again in 2 min.", "whole minutes");
        check(Message(Refusal::Combat, 0).rfind("Unstuck: ", 0) == 0, "every message starts 'Unstuck: '");
    }
}

int main()
{
    TestNearest();
    TestChoose();
    TestRules();
    std::cout << "unstuck tests: " << (g_checks - g_failed) << "/" << g_checks << " passed\n";
    return g_failed ? 1 : 0;
}
