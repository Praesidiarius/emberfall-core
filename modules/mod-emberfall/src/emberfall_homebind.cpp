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

// Emberfall start hearths (Emberfall docs/design/stoneborn.md §2.4). The core binds a new character's hearth to its
// start position (playercreateinfo). A race whose hearth should be elsewhere from the first day (the stoneborn: they
// start at the Stonereach's sledge foot, their hearth is the Banked Fire up at Deepholm Stair) has a row in
// ef_start_homebind (race, map, area, position; Emberfall's compiler writes it from player.yaml `start.<race>.hearth`).
// On the character's first login this module binds the hearth there (Player::SetHomebind, as an innkeeper's bind) and
// tells the client (SMSG_BINDPOINTUPDATE). Races without a row keep the core's bind. Read at startup.

#include "DatabaseEnv.h"
#include "Log.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldScript.h"
#include "WorldSession.h"

#include <unordered_map>

namespace
{
    struct StartHearth
    {
        uint32 map = 0;
        uint32 area = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f;
    };

    std::unordered_map<uint8, StartHearth>& Hearths()
    {
        static std::unordered_map<uint8, StartHearth> hearths;
        return hearths;
    }

    class emberfall_homebind_world : public WorldScript
    {
    public:
        emberfall_homebind_world() : WorldScript("emberfall_homebind_world", { WORLDHOOK_ON_STARTUP }) { }

        void OnStartup() override
        {
            Hearths().clear();
            if (QueryResult r = WorldDatabase.Query("SELECT `race`, `map`, `area`, `position_x`, `position_y`, `position_z` "
                                                    "FROM `ef_start_homebind`"))
            {
                do
                {
                    Field* f = r->Fetch();
                    Hearths()[f[0].Get<uint8>()] = { f[1].Get<uint32>(), f[2].Get<uint32>(), f[3].Get<float>(),
                                                     f[4].Get<float>(), f[5].Get<float>() };
                } while (r->NextRow());
            }
            LOG_INFO("module", "Emberfall start hearths: {} race(s) bound away from their start", Hearths().size());
        }
    };

    class emberfall_homebind_player : public PlayerScript
    {
    public:
        emberfall_homebind_player() : PlayerScript("emberfall_homebind_player", { PLAYERHOOK_ON_FIRST_LOGIN }) { }

        void OnPlayerFirstLogin(Player* player) override
        {
            auto it = Hearths().find(player->getRace());
            if (it == Hearths().end())
                return;
            StartHearth const& h = it->second;
            WorldLocation loc(h.map, h.x, h.y, h.z, 0.0f);
            player->SetHomebind(loc, h.area);
            WorldPacket data(SMSG_BINDPOINTUPDATE, 4 + 4 + 4 + 4 + 4);
            data << h.x << h.y << h.z << uint32(h.map) << uint32(h.area);
            player->SendDirectMessage(&data);
            LOG_DEBUG("module", "Emberfall start hearth: {} (race {}) bound at map {} area {}", player->GetName(),
                      player->getRace(), h.map, h.area);
        }
    };
}

void AddEmberfallHomebindScripts()
{
    new emberfall_homebind_world();
    new emberfall_homebind_player();
}
