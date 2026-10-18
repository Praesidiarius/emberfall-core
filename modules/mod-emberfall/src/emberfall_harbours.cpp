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

// Emberfall's harbours (Emberfall docs/design/harbours-and-ships.md §3.3): the harbourmaster on each pier.
//
//   npc_ef_harbourmaster   gossip: the NPC's own text (npc_text = its entry: the route) and, per ship that berths at
//                          this pier, a line with the next sailing: "The War Ferry is at the pier: it sails in 35 s." or
//                          "The War Ferry is due in 2 min 10 s and sails 50 s after." Picking the line asks again.
//
// The ships are the world's MO transports (gameobject_template type 15, spawned by the core from `transports`). The
// pier's stop is the stop frame of a ship's path on the harbourmaster's map within 90 yd of it; the time comes from the
// live ship's path progress (it may be on the other map) and its period, both the core's own clock (TransportMgr's
// key frames). The decisions are src/harbours/EfHarbourLogic.h (tests/harbour_tests.cpp).

#include "Creature.h"
#include "CreatureScript.h"
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptedGossip.h"
#include "ScriptMgr.h"
#include "Transport.h"
#include "TransportMgr.h"

#include "harbours/EfHarbourLogic.h"

#include <optional>
#include <string>
#include <vector>

using namespace Emberfall::Harbours;

namespace
{
    constexpr uint32 kActionRefresh = 1;

    struct ShipAtPier
    {
        std::string name;
        Stop stop;
        uint32 period = 0;
        std::optional<uint64> progress; // the live ship's path progress (none: not spawned)
    };

    // The live MO transport of `entry` on any of its maps.
    MotionTransport const* LiveShip(uint32 entry, TransportTemplate const& tpl)
    {
        for (uint32 mapId : tpl.mapsUsed)
            if (Map* map = sMapMgr->FindBaseNonInstanceMap(mapId))
                for (Transport* t : map->GetAllTransports())
                    if (t->GetEntry() == entry)
                        if (MotionTransport const* mo = t->ToMotionTransport())
                            return mo;
        return nullptr;
    }

    // Every ship whose path stops at this pier (a stop frame within 90 yd of the harbourmaster on its map).
    std::vector<ShipAtPier> ShipsAt(Creature const* keeper)
    {
        std::vector<ShipAtPier> out;
        for (auto const& [entry, goTpl] : *sObjectMgr->GetGameObjectTemplates())
        {
            if (goTpl.type != GAMEOBJECT_TYPE_MO_TRANSPORT)
                continue;
            TransportTemplate const* tpl = sTransportMgr->GetTransportTemplate(entry);
            if (!tpl)
                continue;
            std::vector<Stop> stops;
            for (KeyFrame const& k : tpl->keyFrames)
                if (k.IsStopFrame())
                    stops.push_back({ k.Node->mapid, k.Node->x, k.Node->y, k.ArriveTime, k.DepartureTime });
            std::optional<Stop> const berth = BerthNear(stops, keeper->GetMapId(), keeper->GetPositionX(), keeper->GetPositionY());
            if (!berth)
                continue;
            ShipAtPier s;
            s.name = goTpl.name;
            s.stop = *berth;
            s.period = tpl->pathTime;
            if (MotionTransport const* mo = LiveShip(entry, *tpl))
            {
                s.progress = mo->GetPathProgress();
                if (mo->GetPeriod())
                    s.period = mo->GetPeriod();
            }
            out.push_back(std::move(s));
        }
        return out;
    }

    class npc_ef_harbourmaster : public CreatureScript
    {
    public:
        npc_ef_harbourmaster() : CreatureScript("npc_ef_harbourmaster") { }

        static void Show(Player* player, Creature* creature)
        {
            ClearGossipMenuFor(player);
            for (ShipAtPier const& s : ShipsAt(creature))
            {
                std::string line;
                if (!s.progress)
                    line = s.name + " is not sailing today.";
                else
                    line = Describe(s.name, NextSailing(s.stop, *s.progress, s.period), (s.stop.depart - s.stop.arrive + 999) / 1000);
                AddGossipItemFor(player, GOSSIP_ICON_TAXI, line, GOSSIP_SENDER_MAIN, kActionRefresh);
            }
            SendGossipMenuFor(player, creature->GetEntry(), creature->GetGUID());   // npc_text = the harbourmaster's entry
        }

        bool OnGossipHello(Player* player, Creature* creature) override
        {
            Show(player, creature);
            return true;
        }

        bool OnGossipSelect(Player* player, Creature* creature, uint32 /*sender*/, uint32 /*action*/) override
        {
            Show(player, creature);   // asking again: the time has moved on
            return true;
        }
    };
}

void AddEmberfallHarboursScripts()
{
    new npc_ef_harbourmaster();
}
