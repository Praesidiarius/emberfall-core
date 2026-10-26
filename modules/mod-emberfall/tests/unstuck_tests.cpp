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
// nearest town on the same map, the instance's entrance, home without a town, and the rules (combat, cooldown, dead);
// nearby unstuck (src/unstuck/EfUnstuckNearby.h): the rings, the stop, judging and picking a spot, its rules, the EFU
// line, the messages in English and German.

#include "EfUnstuckLogic.h"
#include "EfUnstuckNearby.h"

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

    // A few of Emberfall's town points (world.yaml): Millbrook, Hollybank, Coldharrow, Second Station; one on map 1 at
    // the spot of the first query on map 0. (World coordinates after the world scale of 2026-10-26.)
    std::vector<Point> const kPoints = {
        { 90001, 0, -1037.57f, -1178.35f, 52.36f, 3.14f, "Millbrook" },
        { 90029, 0, -1330.79f, -1612.64f, 54.6f, 1.2f, "Hollybank" },
        { 90010, 0, -2145.85f, -2192.03f, 56.15f, 0.0f, "Coldharrow" },
        { 90022, 0, -6324.27f, -2956.83f, 69.28f, 0.0f, "Second Station" },
        { 99999, 1, -1046.1f, -1191.95f, 52.36f, 0.0f, "Elsewhere" },
    };

    void TestNearest()
    {
        auto n = NearestTown(kPoints, 0, -1046.1f, -1191.95f, 46.72f);
        check(n && kPoints[*n].id == 90001, "next to Millbrook: Millbrook (not map 1's point at the same spot)");
        n = NearestTown(kPoints, 0, -2085.45f, -2131.62f, 57.75f);
        check(n && kPoints[*n].id == 90010, "near Coldharrow: Coldharrow");
        n = NearestTown(kPoints, 0, -6006.54f, -3593.47f, 66.43f);
        check(n && kPoints[*n].id == 90022, "in Redmere: Second Station");
        n = NearestTown(kPoints, 0, -1273.08f, -1558.94f, 54.28f);
        check(n && kPoints[*n].id == 90029, "in Hollybank Fields: Hollybank");
        n = NearestTown(kPoints, 1, 5950.0f, 6750.0f, -11.2f);
        check(n && kPoints[*n].id == 99999, "map 1: its only point, however far");
        check(!NearestTown(kPoints, 530, 0, 0, 0), "a map without points: none");
        check(!NearestTown({}, 0, 0, 0, 0), "no points: none");
    }

    void TestChoose()
    {
        Target t = Choose(kPoints, 0, false, false, -1046.1f, -1191.95f, 46.72f);
        check(t.where == Where::Town && t.town && kPoints[*t.town].id == 90001, "open world: the nearest town");
        t = Choose(kPoints, 751, true, true, 47.0f, 268.0f, 30.0f);
        check(t.where == Where::Entrance, "the Barrows (an instance with a door): the entrance");
        t = Choose(kPoints, 0, true, true, -1037.57f, -1178.35f, 52.36f);
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

    void TestGerman()
    {
        check(Duration(252, Lang::De) == "4 Min. 12 s", "German duration");
        check(Message(Refusal::Cooldown, 252, Lang::De) == "Befreien: wieder möglich in 4 Min. 12 s.", "German cooldown");
        check(Message(Refusal::Combat, 0, Lang::De) == "Befreien: nicht im Kampf.", "German combat");
        check(TakingYouTo(Where::Town, "Millbrook") == "Unstuck: taking you to Millbrook.", "taking you to a town");
        check(TakingYouTo(Where::Town, "Millbrook", Lang::De) == "Befreien: du wirst nach Millbrook gebracht.", "German town");
        check(TakingYouTo(Where::Entrance, "", Lang::De) == "Befreien: du wirst zum Eingang gebracht.", "German entrance");
        check(TakingYouTo(Where::Home, "") == "Unstuck: taking you home.", "home");
    }

    namespace N = Emberfall::Unstuck::Nearby;

    void TestRings()
    {
        N::Search s;
        std::vector<N::Offset> r = N::Rings(s);
        check(r.size() == s.radii.size() * std::size_t(s.perRing), "rings: radii x points");
        check(r.front().radius == 5 && r.back().radius == 40, "nearest ring first, 40 yd last");
        bool onCircle = true;
        for (N::Offset const& o : r)
            onCircle = onCircle && std::fabs(std::sqrt(o.dx * o.dx + o.dy * o.dy) - o.radius) < 0.01f;
        check(onCircle, "every point lies on its ring");
        check(std::fabs(r[0].dy) < 0.001f && std::fabs(r[std::size_t(s.perRing)].dy) > 0.1f, "every other ring turned half a step");
        s.perRing = 0;
        check(N::Rings(s).empty(), "no points per ring: none");

        N::Search d;
        check(!N::Stop(d, std::nullopt, 40), "nothing found: probe on to 40 yd");
        check(!N::Stop(d, 8.0f, 16), "found at 8: still the ring at 16 (8 + settle 8)");
        check(N::Stop(d, 8.0f, 20), "found at 8: not the ring at 20");
    }

    // A good spot `d` yd east of the player at (0, 0, 10).
    N::Probe Good(float d, float z = 10)
    {
        N::Probe p;
        p.radius = d;
        p.onMesh = true;
        p.x = d;
        p.y = 0;
        p.z = z;
        p.floor = true;
        p.floorGap = 0.2f;
        p.headroom = true;
        p.blockedSides = 0;
        p.openArea = 400;
        return p;
    }

    void TestJudge()
    {
        N::Rules r;
        check(N::Judge(Good(8), 0, 0, r) == N::Reject::None, "open ground 8 yd away: good");
        N::Probe p = Good(8);
        p.onMesh = false;
        check(N::Judge(p, 0, 0, r) == N::Reject::NoMesh, "no navmesh: no");
        check(N::Judge(Good(3), 0, 0, r) == N::Reject::TooClose, "3 yd: where they are stuck");
        check(N::Judge(Good(50), 0, 0, r) == N::Reject::TooFar, "50 yd: too far");
        p = Good(8);
        p.floor = false;
        check(N::Judge(p, 0, 0, r) == N::Reject::NoFloor, "no floor under the polygon: no");
        p = Good(8);
        p.floorGap = 3;
        check(N::Judge(p, 0, 0, r) == N::Reject::NoFloor, "the floor 3 yd off the navmesh: no");
        p = Good(8);
        p.headroom = false;
        check(N::Judge(p, 0, 0, r) == N::Reject::Headroom, "a roof at head height: no");
        p = Good(8);
        p.blockedSides = 1;
        check(N::Judge(p, 0, 0, r) == N::Reject::None, "against one wall: good");
        p.blockedSides = 2;
        check(N::Judge(p, 0, 0, r) == N::Reject::Wedged, "in a corner or inside a model: no");
        p = Good(8);
        p.underWater = true;
        check(N::Judge(p, 0, 0, r) == N::Reject::UnderWater, "under water: no");
        p = Good(8);
        p.openArea = 40;
        check(N::Judge(p, 0, 0, r) == N::Reject::Pocket, "a closed pocket of 40 yd2: no");
        p = Good(8);
        p.hostiles = 2;
        check(N::Judge(p, 0, 0, r) == N::Reject::None, "two hostiles about: allowed");
        p.hostiles = 3;
        check(N::Judge(p, 0, 0, r) == N::Reject::Camp, "three hostiles: a camp");

        check(N::PolygonArea({ 0, 0, 10, 0, 10, 10, 0, 10 }) == 100.0f, "a 10 x 10 square: 100");
        check(N::PolygonArea({ 0, 0, 0, 10, 10, 10, 10, 0 }) == 100.0f, "either winding");
        check(N::PolygonArea({ 0, 0, 1, 1 }) == 0.0f, "two points: no area");
    }

    void TestPick()
    {
        N::Rules r;
        check(!N::Pick({}, 0, 0, 10, r), "nothing probed: nothing");
        N::Probe bad = Good(6);
        bad.headroom = false;
        std::vector<N::Probe> v = { bad, Good(12), Good(9) };
        auto best = N::Pick(v, 0, 0, 10, r);
        check(best && *best == 2, "the nearest good spot (9 yd), not the nearer bad one");
        // A floor 6 yd up at 7 yd scores 7 + 12 = 19: a spot at 12 yd on the level wins.
        v = { Good(7, 16), Good(12) };
        best = N::Pick(v, 0, 0, 10, r);
        check(best && *best == 1, "a level spot beats a nearer one on the floor above");
        N::Probe watched = Good(8);
        watched.hostiles = 1;
        v = { watched, Good(15) };
        best = N::Pick(v, 0, 0, 10, r);
        check(best && *best == 1, "a hostile near a spot costs 12 yd: the quiet one at 15 wins over 8 + 12");
        v = { bad };
        check(!N::Pick(v, 0, 0, 10, r), "only bad spots: nothing (to town)");
    }

    void TestNearbyRules()
    {
        N::Ask a;
        a.now = 10000;
        check(N::Check(a) == N::Refusal::None, "alive, free, never used: yes");
        a.alive = false;
        check(N::Check(a) == N::Refusal::Dead, "dead: no (the town unstuck releases them)");
        a.alive = true;
        a.inCombat = true;
        check(N::Check(a) == N::Refusal::Combat, "in combat: no");
        a.inCombat = false;
        a.inFlight = true;
        check(N::Check(a) == N::Refusal::Flight, "on a flight path: no");
        a.inFlight = false;
        a.onTransport = true;
        check(N::Check(a) == N::Refusal::Transport, "on a ship: no");
        a.onTransport = false;
        a.onVehicle = true;
        check(N::Check(a) == N::Refusal::Vehicle, "on a vehicle: no");
        a.onVehicle = false;
        a.inBattleground = true;
        check(N::Check(a) == N::Refusal::Battleground, "in a battleground: no");
        a.inBattleground = false;
        a.lastUse = 10000 - 100;
        check(N::Check(a) == N::Refusal::Cooldown && Remaining(a.now, a.lastUse, a.cooldown) == 200, "100 s after a use: 200 s left");
        a.lastUse = 10000 - 300;
        check(N::Check(a) == N::Refusal::None, "300 s after: yes");
        a.enabled = false;
        a.alive = false;
        check(N::Check(a) == N::Refusal::Off, "off: off before anything else");

        check(N::StateLine(0, 212) == "0 212", "the EFU line");
        check(N::StateLine(-1, 0) == "-1 0", "the EFU line, nearby off");
        check(N::Message(N::Refusal::Cooldown, 200) == "Unstuck: you can use nearby unstuck again in 3 min 20 s.", "nearby cooldown");
        check(N::Message(N::Refusal::Cooldown, 200, Lang::De) == "Befreien: in der Nähe befreien wieder in 3 Min. 20 s.", "German nearby cooldown");
        bool prefixed = true;
        for (N::Refusal x : { N::Refusal::Off, N::Refusal::Dead, N::Refusal::Combat, N::Refusal::Flight, N::Refusal::Transport,
                              N::Refusal::Vehicle, N::Refusal::Battleground, N::Refusal::Cooldown })
            prefixed = prefixed && N::Message(x, 5).rfind("Unstuck: ", 0) == 0 && N::Message(x, 5, Lang::De).rfind("Befreien: ", 0) == 0;
        check(prefixed, "every refusal starts with the prefix, in both languages");
        check(N::Message(N::Refusal::None, 0).empty(), "no refusal: no message");
        check(N::Freed(11.6f) == "Unstuck: you are free, 12 yd from where you were.", "freed");
        check(N::Freed(11.6f, Lang::De) == "Befreien: du bist frei, 12 m von deinem alten Platz.", "German freed");
        check(N::NothingNear(40) == "Unstuck: no free spot within 40 yd; trying the nearest town.", "nothing near");
        check(N::NothingNear(40, Lang::De).rfind("Befreien: ", 0) == 0, "German nothing near");
    }
}

int main()
{
    TestNearest();
    TestChoose();
    TestRules();
    TestGerman();
    TestRings();
    TestJudge();
    TestPick();
    TestNearbyRules();
    std::cout << "unstuck tests: " << (g_checks - g_failed) << "/" << g_checks << " passed\n";
    return g_failed ? 1 : 0;
}
