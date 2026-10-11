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

// Emberfall (PATCHES.md, "Module"): the parts of Emberfall.OwnData that AzerothCore's script hooks
// can carry, so they need no core patch. Each script does nothing unless Emberfall.OwnData = 1
// (worldserver.conf or AC_EMBERFALL_OWN_DATA; src/server/game/World/EmberfallOwnData.h).

#include "AllCreatureScript.h"
#include "AreaDefines.h"
#include "DBCStores.h"
#include "EmberfallOwnData.h"
#include "GossipDef.h"
#include "Group.h"
#include "Log.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "RaceMgr.h"
#include "ServerScript.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldScript.h"
#include "WorldSession.h"

namespace
{
    // PATCHES.md #10: the spirit healer's gossip option casts 17251 (Player::OnGossipSelect), and
    // only the stock spell script on 17251 (spell_gen_spirit_healer_res) sends the confirm; a
    // worldserver built with SCRIPTS=none never offers the resurrection. Sent here instead; the core
    // still casts 17251 afterwards (its visual). CMSG_SPIRIT_HEALER_ACTIVATE checks the NPC and the
    // death itself, so an extra confirm grants nothing.
    class emberfall_spirit_healer_confirm : public AllCreatureScript
    {
    public:
        emberfall_spirit_healer_confirm() : AllCreatureScript("emberfall_spirit_healer_confirm") { }

        bool CanCreatureGossipSelect(Player* player, Creature* creature, uint32 /*sender*/, uint32 action) override
        {
            // A database gossip option's action is its OptionType.
            if (action == GOSSIP_OPTION_SPIRITHEALER && player->isDead() && Emberfall::OwnData())
            {
                WorldPacket data(SMSG_SPIRIT_HEALER_CONFIRM, 8);
                data << creature->GetGUID();
                player->SendDirectMessage(&data);
            }
            return false; // the core handles the option as usual
        }
    };

    class emberfall_own_data_world : public WorldScript
    {
    public:
        emberfall_own_data_world() : WorldScript("emberfall_own_data_world",
            { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED }) { }

        // PATCHES.md #5: setting up Wintergrasp creates map 571 and asserts on it; our data has no
        // Wintergrasp (the emberfall-data profile also sets Wintergrasp.Enable = 2).
        void OnAfterConfigLoad(bool /*reload*/) override
        {
            if (Emberfall::OwnData() && sWorld->getIntConfig(CONFIG_WINTERGRASP_ENABLE) != 2)
            {
                LOG_WARN("server.loading", "Emberfall.OwnData: Wintergrasp.Enable forced to 2 (no Wintergrasp in our data)");
                sWorld->setIntConfig(CONFIG_WINTERGRASP_ENABLE, 2);
            }
        }

        // PATCHES.md #2: the core skips the stock start-area map check under Emberfall.OwnData;
        // every playercreateinfo position must be on a .map file instead (a WARN, not an exit;
        // Emberfall's log check forbids it once the server has our maps).
        void OnBeforeWorldInitialized() override
        {
            if (!Emberfall::OwnData())
                return;

            for (uint32 race = 1; race < sRaceMgr->GetMaxRaces(); ++race)
                for (uint32 cls = 1; cls < MAX_CLASSES; ++cls)
                    if (PlayerInfo const* info = sObjectMgr->GetPlayerInfo(race, cls))
                        if (!MapMgr::ExistMapAndVMap(info->mapId, info->positionX, info->positionY))
                            LOG_WARN("server.loading", "Emberfall.OwnData: no map file for the start position of race {} class {} (map {}, {} {})",
                                race, cls, info->mapId, info->positionX, info->positionY);
        }
    };

    // PATCHES.md #7: a raid leader's MSG_SET_RAID_DIFFICULTY makes the core create maps 571 and 0
    // (to park players inside a shared-difficulty raid) and assert on a missing Map.dbc row. With
    // Emberfall's placeholder rows (dbc_placeholders.sql, the default) both exist and nothing
    // changes; without them a group leader's change is refused instead of crashing the server.
    class emberfall_own_data_server : public ServerScript
    {
    public:
        emberfall_own_data_server() : ServerScript("emberfall_own_data_server", { SERVERHOOK_CAN_PACKET_RECEIVE }) { }

        bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
        {
            if (packet.GetOpcode() != MSG_SET_RAID_DIFFICULTY || !session || packet.size() < 4)
                return true;
            Player* player = session->GetPlayer();
            if (!player || !Emberfall::OwnData() || (sMapStore.LookupEntry(MAP_NORTHREND) && sMapStore.LookupEntry(MAP_EASTERN_KINGDOMS)))
                return true;
            uint32 mode = packet.read<uint32>(0);
            Group* group = player->GetGroup();
            if (mode >= MAX_RAID_DIFFICULTY || Difficulty(mode) == player->GetRaidDifficulty() || !group || !group->IsLeader(player->GetGUID()))
                return true; // the core's handler does not reach the home maps
            player->SendRaidDifficulty(true);
            return false;
        }
    };
}

void AddEmberfallOwnDataScripts()
{
    new emberfall_spirit_healer_confirm();
    new emberfall_own_data_world();
    new emberfall_own_data_server();
}
