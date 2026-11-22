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

// A flight master's first talk with a quest flight on offer (Emberfall's tools/py/emberfall_content/quest_flights.py,
// 2026-11-23), with no core types so that tests/questflight_tests.cpp checks it on its own.
//
// The core answers a flight master's gossip hello with its menu (Player::PrepareGossipMenu). Reaching the menu's taxi
// line while the player does not know the roost's node, it teaches the node (WorldSession::SendLearnNewTaxiNode:
// SMSG_NEW_TAXI_PATH) and stops building the menu there, so the menu goes out without the lines after the taxi line;
// the Emberfall client opens the flight map on SMSG_NEW_TAXI_PATH (net/Npc.cpp, as the original client). A quest flight
// (a gossip line shown while its quest is held, after the role lines) therefore never showed on the first talk to a
// roost-keeper, which is the talk a hand-off sends the player to.
//
// The rule: on a flight master the player has not learned yet whose menu offers some line besides the taxi line (one
// whose conditions hold for them: a quest flight), the module teaches the node without SMSG_NEW_TAXI_PATH (so the map
// stays shut; the "new flight path" line comes as a notification in the player's language instead) and then sends the
// whole menu: the quest flight and the taxi line. Everywhere else the core's own way runs unchanged (an unknown roost
// with nothing else to say: learned and the map at once; a known roost: the menu, which the client skips to the map
// when the taxi line is its only line).

#ifndef EF_QUEST_FLIGHT_LOGIC_H
#define EF_QUEST_FLIGHT_LOGIC_H

#include <cstdint>
#include <vector>

namespace Emberfall::QuestFlight
{
    // GossipOptionType GOSSIP_OPTION_TAXIVENDOR (the core's GossipDef.h): the "show me the map" line.
    constexpr uint32_t kTaxiOption = 4;

    // One line of the flight master's menu as the rule sees it.
    struct Line
    {
        uint32_t optionType = 0;    // gossip_menu_option.OptionType
        bool shown = false;         // its OptionNpcFlag is one of the NPC's flags and its conditions hold for the player
    };

    // Does the module answer this hello (teach the node quietly, then send the whole menu)? Only on a flight master
    // with a node here the player does not know yet, whose menu shows a line besides the taxi line.
    inline bool ShowsMenuOnFirstTalk(bool flightMaster, uint32_t node, bool nodeKnown, std::vector<Line> const& lines)
    {
        if (!flightMaster || node == 0 || nodeKnown)
            return false;
        for (Line const& l : lines)
            if (l.shown && l.optionType != kTaxiOption)
                return true;
        return false;
    }

    // The client's ERR_NEW_TAXI_PATH (Emberfall's client/interface/Strings/*/Npc.lua) for the session's locale
    // (the core's LocaleConstant numbers: enUS 0, frFR 2, deDE 3; everything else reads English).
    inline char const* NewFlightPathText(uint32_t locale)
    {
        switch (locale)
        {
            case 2: return "Nouvelle route a\xC3\xA9rienne d\xC3\xA9" "couverte.";
            case 3: return "Du hast eine neue Flugroute entdeckt.";
            default: return "You have found a new flight path.";
        }
    }
}

#endif
