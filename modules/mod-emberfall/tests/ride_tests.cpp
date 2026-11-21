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

// Standalone tests of the ride rule (src/rides/EfRideLogic.h; tests/run_tests.sh builds and runs them): who of a
// clicker's group is taken along on their cart, where they sit, and when the cart goes (feedback #693).

#include "EfRideLogic.h"

#include <iostream>
#include <string>

using namespace Emberfall::Ride;

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

    Member Near(float distance)
    {
        Member m;
        m.sameMap = true;
        m.distance = distance;
        m.alive = true;
        m.rideToDo = true;
        return m;
    }
}

int main()
{
    // the ride to do: in the log, not complete, the ride's credit not had
    check(RideToDo(true, 0, 1), "taken, not ridden: to do");
    check(!RideToDo(true, 1, 1), "ridden, an item still to fetch: the ride is done (no free second ride)");
    check(!RideToDo(false, 0, 1), "not in the log (or complete): nothing to do");
    check(!RideToDo(true, 0, 0), "a quest without the ride's credit is no ride");

    // who is taken along
    check(TakesAlong(Near(3.0f)), "#693: a member beside the cart with the ride to do rides with the clicker");
    check(TakesAlong(Near(kTakeAlongReach)), "at the reach's edge: taken");
    check(!TakesAlong(Near(kTakeAlongReach + 0.1f)), "farther: not taken (they click the parked cart)");
    Member m = Near(2.0f);
    m.rideToDo = false;
    check(!TakesAlong(m), "without the quest (or the ride done): never taken past a zone's danger");
    m = Near(2.0f);
    m.sameMap = false;
    check(!TakesAlong(m), "another map or phase: not taken");
    m = Near(2.0f);
    m.alive = false;
    check(!TakesAlong(m), "a dead member: not taken");
    m = Near(2.0f);
    m.inCombat = true;
    check(!TakesAlong(m), "a member in a fight: not pulled out of it onto a cart");
    m = Near(2.0f);
    m.onVehicle = true;
    check(!TakesAlong(m), "already riding (their own cart, a lift): left where they are");
    m = Near(2.0f);
    m.inFlight = true;
    check(!TakesAlong(m), "on a flight: not taken");

    // who brings the group
    check(BringsGroup(true, true), "the clicker sitting down brings their group");
    check(!BringsGroup(true, false), "a member taken along brings nobody (no chain of carts)");
    check(!BringsGroup(false, false), "the driver and the pony sitting down bring nobody");

    // where they sit: nearest first, one seat each, the rest ride their own cart
    std::vector<int> seats{3, 4, 5, 6};   // the bed's four (the bench's seat is the clicker's)
    std::vector<Member> group{Near(9.0f), Near(1.0f), Near(5.0f), Near(30.0f)};
    auto a = Assign(group, seats);
    check(a.size() == 3, "three of four near: three sit down");
    check(a.size() == 3 && a[0].first == 1 && a[0].second == 3, "the nearest takes the first free seat");
    check(a.size() == 3 && a[1].first == 2 && a[2].first == 0, "then the next nearest");
    bool far = false;
    for (auto const& [i, s] : a)
        far = far || i == 3;
    check(!far, "the one 30 yd off is not taken");

    // a full party of five: the clicker on the bench and four on the bed
    std::vector<Member> party{Near(2.0f), Near(3.0f), Near(4.0f), Near(5.0f)};
    check(Assign(party, seats).size() == 4, "#693: a party of five all ride in one cart");
    // a raid's sixth: no seat left, rides a cart of their own (the parked cart stays)
    std::vector<Member> raid{Near(2.0f), Near(3.0f), Near(4.0f), Near(5.0f), Near(6.0f)};
    auto r = Assign(raid, seats);
    check(r.size() == 4, "a sixth near: four seats, four taken");
    bool sixth = false;
    for (auto const& [i, s] : r)
        sixth = sixth || i == 4;
    check(!sixth, "the farthest of five members is the one left to click the parked cart");
    check(Assign(party, std::vector<int>{}).empty(), "no free seat: nobody taken");
    check(Assign(std::vector<Member>{}, seats).empty(), "alone: nobody taken");
    std::vector<int> two{5, 6};
    auto t = Assign(party, two);
    check(t.size() == 2 && t[0].second == 5 && t[1].second == 6, "the free seats in their order");

    // when the cart goes on the way
    check(CartEmpty(0), "the last player got down (or logged out): the cart goes");
    check(!CartEmpty(1), "a member still on: the cart rides on with them");
    check(kEmptyDataField == 1 && kEmptyDataValue == 1, "the cart's data_set row is rides.py's EMPTY_DATA (1, 1)");

    std::cout << "ride tests: " << (g_checks - g_failed) << "/" << g_checks << " passed\n";
    return g_failed ? 1 : 0;
}
