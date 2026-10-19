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

// Emberfall's dev vendors (Emberfall docs/design/gear/frostguard-set.md §5; content/data/dev/npcs.yaml): NPCs for the
// local dev server only, the first the dev quartermaster 92001 in Millbrook with the Frostguard set for free.
//
//   emberfall_devvendor_world   WorldScript::OnBeforeWorldInitialized: with Emberfall.DevVendor.Enable = 1
//                               (AC_EMBERFALL_DEV_VENDOR_ENABLE; Emberfall's ef_server.sh sets it, a public realm never
//                               does) spawns the manual spawn group 92001 on its map (Map::SpawnGroupSpawn: the group is
//                               active on that map, its grid loader puts the NPCs in when the grid loads). Off, the group
//                               is never spawned: the core leaves a manual group out of the world.
//   npc_ef_dev_vendor           the dev vendors' script, the second lock: off, the NPC leaves the world on its first
//                               update (and on every respawn) and its gossip, and so its vendor list, opens nothing.
//
// The vendor's free price is data, not code: its npc_vendor rows carry the zero extended cost 3001 (world.sql's
// itemextendedcost_dbc), and the core asks no money for an item with an extended cost (VendorItem::IsGoldRequired).
// The gating is src/devvendor/EfDevVendorLogic.h (tests/devvendor_tests.cpp).

#include "Config.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "CreatureScript.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptedGossip.h"
#include "ScriptMgr.h"
#include "WorldScript.h"

#include "EfDevVendorLogic.h"

#include <string>

using namespace Emberfall::DevVendor;

namespace
{
    // Read when asked (a `.reload config` that turns it off sends a dev vendor away at its next update; turning it on
    // takes a restart, the spawn happens at boot).
    bool DevVendorOn()
    {
        return sConfigMgr->GetOption<bool>(std::string(kSwitch), false, false);
    }

    constexpr Seconds kAwayRespawn = Seconds(DAY);

    class emberfall_devvendor_world : public WorldScript
    {
    public:
        emberfall_devvendor_world() : WorldScript("emberfall_devvendor_world", { WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED }) { }

        void OnBeforeWorldInitialized() override
        {
            bool const on = DevVendorOn();
            SpawnGroupTemplateData const* group = sObjectMgr->GetSpawnGroupData(kSpawnGroup);
            if (!SpawnsGroup(on, group != nullptr, group ? uint32(group->flags) : 0))
            {
                if (on)
                    LOG_WARN("server.loading", ">> Emberfall dev vendors: {} is on, but spawn group {} is missing or not "
                             "a manual group (Emberfall's world.sql, content/data/dev/npcs.yaml)", kSwitch, kSpawnGroup);
                else
                    LOG_INFO("server.loading", ">> Emberfall dev vendors: off ({} = 0), spawn group {} stays out of the "
                             "world", kSwitch, kSpawnGroup);
                return;
            }
            if (group->mapId == SPAWNGROUP_MAP_UNSET)
            {
                LOG_WARN("server.loading", ">> Emberfall dev vendors: spawn group {} has no members", kSpawnGroup);
                return;
            }
            Map* map = sMapMgr->CreateBaseMap(group->mapId);
            if (!map || map->Instanceable())
            {
                LOG_WARN("server.loading", ">> Emberfall dev vendors: spawn group {} is on map {}, which is not a "
                         "continent", kSpawnGroup, group->mapId);
                return;
            }
            uint32 members = 0;
            auto range = sObjectMgr->GetSpawnDataForGroup(kSpawnGroup);
            for (auto it = range.first; it != range.second; ++it)
                ++members;
            map->SpawnGroupSpawn(kSpawnGroup);
            LOG_INFO("server.loading", ">> Emberfall dev vendors: on ({} = 1), spawn group {} ({} spawn(s)) active on "
                     "map {}", kSwitch, kSpawnGroup, members, group->mapId);
        }
    };

    class npc_ef_dev_vendor : public CreatureScript
    {
    public:
        npc_ef_dev_vendor() : CreatureScript(std::string(kScriptName).c_str()) { }

        struct npc_ef_dev_vendorAI : public CreatureAI
        {
            explicit npc_ef_dev_vendorAI(Creature* creature) : CreatureAI(creature) { }

            // Off: out of the world at once (and again on every respawn, a day later); on: an ordinary passive vendor.
            void UpdateAI(uint32 /*diff*/) override
            {
                if (VendorStays(DevVendorOn()))
                    return;
                LOG_INFO("module", "Emberfall dev vendor {} (spawn {}): {} is off, it leaves the world", me->GetEntry(),
                         me->GetSpawnId(), kSwitch);
                me->ReplaceAllNpcFlags(UNIT_NPC_FLAG_NONE);
                me->SetVisible(false);
                me->DespawnOrUnsummon(0ms, kAwayRespawn);
            }

            void AttackStart(Unit* /*who*/) override { }
            void MoveInLineOfSight(Unit* /*who*/) override { }
        };

        CreatureAI* GetAI(Creature* creature) const override { return new npc_ef_dev_vendorAI(creature); }

        // On: the core's own menu (the NPC's text and "Let me see what you have.", gossip.yaml); off: nothing.
        bool OnGossipHello(Player* player, Creature* /*creature*/) override
        {
            if (GossipOpens(DevVendorOn()))
                return false;
            ClearGossipMenuFor(player);
            CloseGossipMenuFor(player);
            return true;
        }

        bool OnGossipSelect(Player* player, Creature* /*creature*/, uint32 /*sender*/, uint32 /*action*/) override
        {
            if (GossipOpens(DevVendorOn()))
                return false;
            CloseGossipMenuFor(player);
            return true;
        }
    };
}

void AddEmberfallDevVendorScripts()
{
    new emberfall_devvendor_world();
    new npc_ef_dev_vendor();
}
