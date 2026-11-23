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

// Emberfall's quest flights on the first talk to their roost-keeper (questflights/EfQuestFlightLogic.h has the rule and
// why). The hello of a flight master whose node the player does not know yet, with a line besides the taxi line on offer
// to them (a quest flight: Emberfall's gossip.yaml `taxi_path`, shown while its quest is held): the node is learned
// here as the core's SendLearnNewTaxiNode does it but without SMSG_NEW_TAXI_PATH (the client would open the map), the
// "new flight path" line goes out as a notification, then the core's own menu (now with every line: the taxi line no
// longer stops it). Any other hello is the core's. On with Emberfall.OwnData = 1.

#include "AllCreatureScript.h"
#include "ConditionMgr.h"
#include "Creature.h"
#include "EmberfallOwnData.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "questflights/EfQuestFlightLogic.h"

#include <string>
#include <vector>

namespace
{
    namespace QF = Emberfall::QuestFlight;

    class emberfall_quest_flights_creatures : public AllCreatureScript
    {
    public:
        emberfall_quest_flights_creatures() : AllCreatureScript("emberfall_quest_flights_creatures") { }

        bool CanCreatureGossipHello(Player* player, Creature* creature) override
        {
            if (!Emberfall::OwnData() || !player || !creature || !player->GetSession())
                return false;
            bool const flightMaster = creature->HasNpcFlag(UNIT_NPC_FLAG_FLIGHTMASTER);
            if (!flightMaster)
                return false;
            uint32 const node = sObjectMgr->GetNearestTaxiNode(*creature, player->GetTeamId(true));
            bool const known = node && player->m_taxi.IsTaximaskNodeKnown(node);
            if (!node || known)
                return false;

            // The menu's lines as Player::PrepareGossipMenu filters them (npc flag, conditions).
            uint32 const npcFlags = creature->GetNpcFlags();
            std::vector<QF::Line> lines;
            GossipMenuItemsMapBounds bounds = sObjectMgr->GetGossipMenuItemsMapBounds(creature->GetGossipMenuId());
            for (auto it = bounds.first; it != bounds.second; ++it)
                lines.push_back({ it->second.OptionType, (it->second.OptionNpcFlag & npcFlags) != 0 &&
                                  sConditionMgr->IsObjectMeetToConditions(player, creature, it->second.Conditions) });
            if (!QF::ShowsMenuOnFirstTalk(flightMaster, node, known, lines))
                return false;

            // WorldSession::SendLearnNewTaxiNode without its SMSG_NEW_TAXI_PATH.
            if (!player->m_taxi.SetTaximaskNode(node))
                return false;
            WorldPacket status(SMSG_TAXINODE_STATUS, 9);
            status << creature->GetGUID();
            status << uint8(1);
            player->GetSession()->SendPacket(&status);
            std::string const text = QF::NewFlightPathText(uint32(player->GetSession()->GetSessionDbLocaleIndex()));
            WorldPacket note(SMSG_NOTIFICATION, text.size() + 1);
            note << text;
            player->GetSession()->SendPacket(&note);
            sScriptMgr->OnPlayerLearnTaxiNode(player, node);

            player->PrepareGossipMenu(creature, creature->GetGossipMenuId(), true);
            player->SendPreparedGossip(creature);
            return true;
        }
    };
}

void AddEmberfallQuestFlightScripts()
{
    new emberfall_quest_flights_creatures();
}
