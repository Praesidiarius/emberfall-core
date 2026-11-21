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

// The ride rule (Emberfall feedback #693, 2026-11-21, Nathanael on The empty sett cart: "if i'm in a group, the wagon just
// drives away - it should put all players from the group that are near in the cart"; Emberfall's
// tools/py/emberfall_content/quests/rides.py), with no core types so that tests/ride_tests.cpp checks it on its own.
// A ride's player clicks the parked cart and gets a cart of their own (a summon, the clicker its summoner), which seats
// them on its bench. As they sit down the members of their group are taken along, on the cart's other rider seats:
//   - who: a member on the same map and phase, within kTakeAlongReach yd of the cart, alive, out of a fight, on no
//     vehicle and no flight, with the ride still to do in their quest log (the quest taken and its ride credit not yet
//     had). Nobody else: a member without the quest never rides past a zone's danger on someone else's cart;
//   - where: the cart's empty rider seats (a seat a player may enter: the bench beside the driver and the bed's four),
//     nearest member first; a member the seats do not hold (a raid's sixth and later) is not taken and clicks the
//     parked cart for a cart of their own, which never goes away (the parked cart stays for every clicker);
//   - only the cart's own clicker brings their group (a member taken along brings nobody);
//   - the end: every rider seat's player gets the ride's credit where the cart stops (the cart's SmartAI). A rider who
//     gets down on the way, logs out or dies is off it, the others ride on; when its last player is off, the rule tells
//     the cart (kEmptyData, its SmartAI's data_set: the driver says so and goes). Whoever got down climbs up again at
//     the parked cart.

#ifndef EF_RIDE_LOGIC_H
#define EF_RIDE_LOGIC_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <utility>
#include <vector>

namespace Emberfall::Ride
{
    // How near the cart (yd, the core's distance between the two bodies) a member must stand to be taken along: the
    // ride spell's own range (Emberfall spells/quests.yaml 90375), a few steps round the carter.
    constexpr float kTakeAlongReach = 15.0f;

    // The cart's SmartAI hears that its last player got down (set_data field, value; rides.py EMPTY_DATA).
    constexpr uint32_t kEmptyDataField = 1;
    constexpr uint32_t kEmptyDataValue = 1;

    // A member of the clicker's group as the rule sees them.
    struct Member
    {
        bool sameMap = false;       // on the cart's map, in its phase
        float distance = 0.0f;      // yd to the cart
        bool alive = false;
        bool inCombat = false;
        bool onVehicle = false;     // already a passenger (another ride, a lift, a ship's seat)
        bool inFlight = false;      // on a flight path
        bool rideToDo = false;      // the ride's quest in their log, its ride credit not yet had (RideToDo)
    };

    // Is the ride still to do for a player? The quest is in their log, not complete, and its ride credit (the kill
    // credit of the cart) is under its count. A quest whose ride is done but another objective not (an item) is not.
    constexpr bool RideToDo(bool questIncomplete, uint32_t credit, uint32_t required)
    {
        return questIncomplete && required > 0 && credit < required;
    }

    // Is a member taken along?
    constexpr bool TakesAlong(Member const& m)
    {
        return m.sameMap && m.distance <= kTakeAlongReach && m.alive && !m.inCombat && !m.onVehicle && !m.inFlight
            && m.rideToDo;
    }

    // Does this passenger's sitting down bring their group? Only the cart's own clicker (its summoner), a player.
    constexpr bool BringsGroup(bool passengerIsPlayer, bool passengerIsSummoner)
    {
        return passengerIsPlayer && passengerIsSummoner;
    }

    // Which members sit where: (index into `members`, seat), the members taken along nearest first, one to each of the
    // free rider seats in their order; the members past the seats get none (they ride a cart of their own).
    template <typename Seat>
    std::vector<std::pair<std::size_t, Seat>> Assign(std::vector<Member> const& members, std::vector<Seat> const& freeSeats)
    {
        std::vector<std::size_t> order(members.size());
        std::iota(order.begin(), order.end(), std::size_t(0));
        order.erase(std::remove_if(order.begin(), order.end(), [&](std::size_t i) { return !TakesAlong(members[i]); }),
                    order.end());
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b)
        {
            return members[a].distance < members[b].distance;
        });
        std::vector<std::pair<std::size_t, Seat>> out;
        for (std::size_t k = 0; k < order.size() && k < freeSeats.size(); ++k)
            out.emplace_back(order[k], freeSeats[k]);
        return out;
    }

    // Does the cart go on the way? When no player is on it any more (the driver and the pony are creatures).
    constexpr bool CartEmpty(uint32_t playersOn)
    {
        return playersOn == 0;
    }
}

#endif
