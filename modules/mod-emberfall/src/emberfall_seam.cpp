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

// The seam guard (Emberfall docs/design/kindred-map-split.md §3.4 item 4). The Kindred is two maps, 1 north and 754
// south; each draws a one-tile margin of the other half, purely visual, whose terrain has a "beyond the seam" area
// (ef_seam_area, written by Emberfall's compiler). Once a second (and a few seconds after login) a player is looked
// at: on their own side the spot is remembered; in such an area (zone or subarea) they are put back on the last spot on
// their own side (NearTeleportTo) with "The ground ends here. Take a ship." in their language; with no spot on this map,
// to their hearth. GMs in GM mode, flights, ships and vehicles are left alone. No rows: nothing happens. The rules
// live in src/seam/EfSeamLogic.h (tests/seam_tests.cpp).

#include "Chat.h"
#include "DataMap.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "WorldScript.h"
#include "WorldSession.h"
#include "seam/EfSeamLogic.h"

namespace
{
    namespace ES = Emberfall::Seam;

    // Written once at startup (before any player), only read afterwards.
    ES::Areas s_areas;

    struct SeamState : public DataMap::Base
    {
        uint32 timer = ES::kCheckMs;
        ES::Memory memory;
    };

    void LoadAreas()
    {
        s_areas.clear();
        if (QueryResult r = WorldDatabase.Query("SELECT `area_id`, `map_id` FROM `ef_seam_area`"))
        {
            do
            {
                Field* f = r->Fetch();
                s_areas[f[0].Get<uint32>()] = f[1].Get<uint32>();
            } while (r->NextRow());
        }
        LOG_INFO("server.loading", ">> Emberfall seam guard: {} area(s) beyond the seam", s_areas.size());
    }

    class emberfall_seam_world : public WorldScript
    {
    public:
        emberfall_seam_world() : WorldScript("emberfall_seam_world", { WORLDHOOK_ON_LOAD_CUSTOM_DATABASE_TABLE }) { }

        void OnLoadCustomDatabaseTable() override { LoadAreas(); }
    };

    class emberfall_seam_player : public PlayerScript
    {
    public:
        emberfall_seam_player() : PlayerScript("emberfall_seam_player", { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_UPDATE }) { }

        void OnPlayerLogin(Player* player) override
        {
            if (!s_areas.empty())
                player->CustomData.GetDefault<SeamState>("ef_seam")->timer = ES::kLoginGraceMs;
        }

        void OnPlayerUpdate(Player* player, uint32 diff) override
        {
            if (s_areas.empty() || !player->IsInWorld())
                return;
            SeamState* state = player->CustomData.GetDefault<SeamState>("ef_seam");
            if (state->timer > diff)
            {
                state->timer -= diff;
                return;
            }
            state->timer = ES::kCheckMs;

            ES::Seen seen;
            seen.map = player->GetMapId();
            player->GetZoneAndAreaId(seen.zone, seen.area);
            seen.gm = player->IsGameMaster();
            seen.moved = player->IsInFlight() || player->GetTransport() || player->GetVehicle();
            seen.teleporting = player->IsBeingTeleported();
            seen.falling = player->IsFalling();
            seen.here = { seen.map, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
                          player->GetOrientation() };

            ES::Act const act = ES::Step(s_areas, seen, state->memory);
            if (act != ES::Act::SendBack && act != ES::Act::SendHome)
                return;

            WorldSession* session = player->GetSession();
            ChatHandler(session).SendSysMessage(ES::Message(ES::LangOf(session ? uint32(session->GetSessionDbLocaleIndex()) : 0)));
            if (act == ES::Act::SendBack)
            {
                ES::Spot const& s = state->memory.spot;
                LOG_INFO("module", "Emberfall seam guard: {} (guid {}) in area {} (zone {}) of map {} at ({:.1f}, {:.1f}, {:.1f}) "
                         "sent back to ({:.1f}, {:.1f}, {:.1f})", player->GetName(), player->GetGUID().GetCounter(), seen.area,
                         seen.zone, seen.map, seen.here.x, seen.here.y, seen.here.z, s.x, s.y, s.z);
                player->NearTeleportTo(s.x, s.y, s.z, s.o);
            }
            else
            {
                LOG_INFO("module", "Emberfall seam guard: {} (guid {}) in area {} (zone {}) of map {} at ({:.1f}, {:.1f}, {:.1f}) "
                         "with no spot on this map: home to map {}", player->GetName(), player->GetGUID().GetCounter(), seen.area,
                         seen.zone, seen.map, seen.here.x, seen.here.y, seen.here.z, player->m_homebindMapId);
                player->TeleportTo(player->m_homebindMapId, player->m_homebindX, player->m_homebindY, player->m_homebindZ,
                                   player->GetOrientation());
            }
        }
    };
}

void AddEmberfallSeamScripts()
{
    new emberfall_seam_world();
    new emberfall_seam_player();
}
