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

// Emberfall unstuck (Emberfall docs/design/unstuck.md): any player who is stuck in the world types `.unstuck` (or
// `.stuck`, or picks Unstuck in the game menu) and is taken to the nearest town on their map.
//
// The stock `.unstuck` (cs_misc) casts SPELL_STUCK 7355 for a player account; Emberfall's Spell.dbc makes 7355 instant (2026-10-20; it was a 5 s
// cast that movement and damage broke; content/data/spells/engine.yaml). This module's spell script on 7355:
//   - CheckCast refuses it in combat, on a flight path and within the cooldown (Emberfall.Unstuck.Cooldown, 300 s),
//     with an "Unstuck: ..." system message;
//   - on its Stuck effect a living player goes to the nearest ef_unstuck_point of their map (Emberfall's compiler
//     writes the rows from world.yaml teleports with `town:`); in an instance to its start point (the door's
//     areatrigger_teleport target); with no town on the map, home (the hearth's bind). The dead keep the stock effect:
//     released to their graveyard.
// Each use is logged (logger "module"): the character, from where, to where.
//
// Config: Emberfall.Unstuck.Enable (1), Emberfall.Unstuck.Cooldown (300, seconds; 0: none).

#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "WorldScript.h"
#include "WorldSession.h"

#include "unstuck/EfUnstuckLogic.h"

#include <unordered_map>
#include <vector>

using namespace Acore::ChatCommands;
using namespace Emberfall::Unstuck;

namespace
{
    constexpr uint32 kSpellStuck = 7355;

    bool s_enable = true;
    int64 s_cooldown = 300;
    std::vector<Point> s_points;
    // The last use per character (seconds, game time); kept in memory: a restart forgets it.
    std::unordered_map<ObjectGuid::LowType, int64> s_lastUse;

    void LoadPoints()
    {
        s_points.clear();
        QueryResult result = WorldDatabase.Query("SELECT `id`, `map`, `x`, `y`, `z`, `o`, `name` FROM `ef_unstuck_point`");
        if (result)
        {
            do
            {
                Field* f = result->Fetch();
                Point p;
                p.id = f[0].Get<uint32>();
                p.map = f[1].Get<uint32>();
                p.x = f[2].Get<float>();
                p.y = f[3].Get<float>();
                p.z = f[4].Get<float>();
                p.o = f[5].Get<float>();
                p.name = f[6].Get<std::string>();
                s_points.push_back(std::move(p));
            } while (result->NextRow());
        }
        LOG_INFO("server.loading", ">> Emberfall unstuck: {} ({} town points, cooldown {} s)", s_enable ? "on" : "off",
                 s_points.size(), s_cooldown);
    }

    class emberfall_unstuck_world : public WorldScript
    {
    public:
        emberfall_unstuck_world()
            : WorldScript("emberfall_unstuck_world", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_LOAD_CUSTOM_DATABASE_TABLE }) { }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            s_enable = sConfigMgr->GetOption<bool>("Emberfall.Unstuck.Enable", true, false);
            s_cooldown = sConfigMgr->GetOption<int32>("Emberfall.Unstuck.Cooldown", 300, false);
        }

        void OnLoadCustomDatabaseTable() override { LoadPoints(); }
    };

    Ask AskFor(Player* player)
    {
        Ask a;
        a.enabled = s_enable;
        a.alive = player->IsAlive();
        a.inCombat = player->IsInCombat();
        a.inFlight = player->IsInFlight();
        a.now = int64(GameTime::GetGameTime().count());
        auto it = s_lastUse.find(player->GetGUID().GetCounter());
        a.lastUse = it == s_lastUse.end() ? 0 : it->second;
        a.cooldown = s_cooldown;
        return a;
    }

    // Refused: the message in the player's chat; true if refused.
    bool Refuse(Player* player)
    {
        Ask a = AskFor(player);
        Refusal r = Check(a);
        if (r == Refusal::None)
            return false;
        ChatHandler(player->GetSession()).SendSysMessage(Message(r, Remaining(a.now, a.lastUse, a.cooldown)));
        return true;
    }

    void Send(Player* player)
    {
        Map* map = player->GetMap();
        uint32 mapId = player->GetMapId();
        bool instance = map && map->Instanceable();
        AreaTriggerTeleport const* door = instance ? sObjectMgr->GetMapEntranceTrigger(mapId) : nullptr;
        Target t = Choose(s_points, mapId, instance, door != nullptr, player->GetPositionX(), player->GetPositionY(),
                          player->GetPositionZ());

        uint32 toMap = 0;
        float x = 0, y = 0, z = 0, o = player->GetOrientation();
        std::string to;
        switch (t.where)
        {
            case Where::Entrance:
                toMap = door->target_mapId, x = door->target_X, y = door->target_Y, z = door->target_Z, o = door->target_Orientation;
                to = "the entrance";
                break;
            case Where::Town:
            {
                Point const& p = s_points[*t.town];
                toMap = p.map, x = p.x, y = p.y, z = p.z, o = p.o;
                to = p.name;
                break;
            }
            default:
                toMap = player->m_homebindMapId, x = player->m_homebindX, y = player->m_homebindY, z = player->m_homebindZ;
                to = "home";
                break;
        }
        LOG_INFO("module", "Emberfall unstuck: {} (guid {}) from map {} ({:.1f}, {:.1f}, {:.1f}) to {} map {} ({:.1f}, {:.1f}, {:.1f})",
                 player->GetName(), player->GetGUID().GetCounter(), mapId, player->GetPositionX(), player->GetPositionY(),
                 player->GetPositionZ(), to, toMap, x, y, z);
        s_lastUse[player->GetGUID().GetCounter()] = int64(GameTime::GetGameTime().count());
        ChatHandler(player->GetSession()).PSendSysMessage("Unstuck: taking you to {}.", to);
        player->TeleportTo(toMap, x, y, z, o);
    }
}

// SPELL_STUCK 7355 (spell_script_names from Emberfall's compiler): the rules and the nearest town.
class spell_ef_unstuck : public SpellScript
{
    PrepareSpellScript(spell_ef_unstuck);

    SpellCastResult CheckCast()
    {
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (player && Refuse(player))
            return SPELL_FAILED_DONT_REPORT;
        return SPELL_CAST_OK;
    }

    void HandleStuck(SpellEffIndex effIndex)
    {
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!player)
            return;
        if (!player->IsAlive())
        {
            // The stock effect: released to the graveyard.
            LOG_INFO("module", "Emberfall unstuck: {} (guid {}) dead on map {} ({:.1f}, {:.1f}, {:.1f}): to the graveyard",
                     player->GetName(), player->GetGUID().GetCounter(), player->GetMapId(), player->GetPositionX(),
                     player->GetPositionY(), player->GetPositionZ());
            return;
        }
        PreventHitDefaultEffect(effIndex);
        if (Refuse(player))  // combat started during the cast
            return;
        Send(player);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_ef_unstuck::CheckCast);
        OnEffectHit += SpellEffectFn(spell_ef_unstuck::HandleStuck, EFFECT_0, SPELL_EFFECT_STUCK);
    }
};

// `.stuck`: the same as the stock `.unstuck` for a player (`.unstuck` is the core's, cs_misc).
class emberfall_unstuck_commands : public CommandScript
{
public:
    emberfall_unstuck_commands() : CommandScript("emberfall_unstuck_commands") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable =
        {
            { "stuck", HandleStuck, SEC_PLAYER, Console::No },
        };
        return commandTable;
    }

    static bool HandleStuck(ChatHandler* handler)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;
        player->CastSpell(player, kSpellStuck, false);
        return true;
    }
};

void AddEmberfallUnstuckScripts()
{
    new emberfall_unstuck_world();
    new emberfall_unstuck_commands();
    RegisterSpellScript(spell_ef_unstuck);
}
