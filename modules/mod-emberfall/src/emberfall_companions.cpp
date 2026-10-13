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

// Emberfall companions (Emberfall docs/design/companions.md): the hooks and the GM command `.efparty`.
//
//   .efparty                                     the list (as .efparty list) and the syntax
//   .efparty add <tank|healer|dps|melee|ranged> [archetype] [count]
//   .efparty list | remove <n|all> | stay | follow | assist on|off | level
//   .efparty stats [reset] | threat              the test numbers (fights, kills, aggro losses, threat leads)
//   .efparty reload                              the ef_companion tables again (administrator)
//
// Every subcommand needs Emberfall.Companions.Security (default 2) on top of the command table's level.

#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "EfCompanions.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "WorldScript.h"
#include "WorldSession.h"

using namespace Acore::ChatCommands;
namespace EC = Emberfall::Companions;

namespace
{
    class emberfall_companions_world : public WorldScript
    {
    public:
        emberfall_companions_world() : WorldScript("emberfall_companions_world",
            { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED }) { }

        void OnAfterConfigLoad(bool reload) override { EC::ReadConfig(reload); }

        void OnBeforeWorldInitialized() override { EC::Load(); }
    };

    class emberfall_companions_player : public PlayerScript
    {
    public:
        emberfall_companions_player() : PlayerScript("emberfall_companions_player", {
            PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_MAP_CHANGED, PLAYERHOOK_ON_LEVEL_CHANGED,
            PLAYERHOOK_ON_PLAYER_JUST_DIED }) { }

        void OnPlayerUpdate(Player* player, uint32 diff) override { EC::OnOwnerUpdate(player, diff); }

        void OnPlayerLogout(Player* player) override { EC::DespawnAll(player, true); }

        // A far teleport (an instance, another continent) leaves the guardians behind: summon them again here.
        void OnPlayerMapChanged(Player* player) override { EC::Resummon(player); }

        void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override { EC::Resync(player); }

        void OnPlayerJustDied(Player* player) override { EC::OnOwnerDied(player); }
    };

    class emberfall_companions_commands : public CommandScript
    {
    public:
        emberfall_companions_commands() : CommandScript("emberfall_companions_commands") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable partyTable =
            {
                { "add",    HandleAdd,    SEC_GAMEMASTER,    Console::No },
                { "list",   HandleList,   SEC_GAMEMASTER,    Console::No },
                { "remove", HandleRemove, SEC_GAMEMASTER,    Console::No },
                { "stay",   HandleStay,   SEC_GAMEMASTER,    Console::No },
                { "follow", HandleFollow, SEC_GAMEMASTER,    Console::No },
                { "assist", HandleAssist, SEC_GAMEMASTER,    Console::No },
                { "level",  HandleLevel,  SEC_GAMEMASTER,    Console::No },
                { "stats",  HandleStats,  SEC_GAMEMASTER,    Console::No },
                { "threat", HandleThreat, SEC_GAMEMASTER,    Console::No },
                { "reload", HandleReload, SEC_ADMINISTRATOR, Console::No },
                { "",       HandleList,   SEC_GAMEMASTER,    Console::No },
            };
            static ChatCommandTable commandTable =
            {
                { "efparty", partyTable },
            };
            return commandTable;
        }

        // The GM's own character, if its account has Emberfall.Companions.Security.
        static Player* Owner(ChatHandler* handler)
        {
            WorldSession* session = handler->GetSession();
            Player* player = session ? session->GetPlayer() : nullptr;
            if (!player)
                return nullptr;
            if (uint32(session->GetSecurity()) < EC::Cfg().security)
            {
                handler->SendSysMessage("You may not use .efparty (Emberfall.Companions.Security).");
                handler->SetSentErrorMessage(true);
                return nullptr;
            }
            return player;
        }

        static bool Send(ChatHandler* handler, std::vector<std::string> const& lines)
        {
            for (std::string const& line : lines)
                handler->SendSysMessage(line);
            return true;
        }

        static bool HandleAdd(ChatHandler* handler, Tail args)
        {
            Player* owner = Owner(handler);
            if (!owner)
                return false;
            EC::AddArgs const a = EC::ParseAdd(std::string(args), EC::Cfg().maxPerPlayer);
            if (!a.error.empty())
            {
                handler->SendSysMessage("Syntax: .efparty add <tank|healer|dps|melee|ranged> [archetype] [count]: " + a.error + ".");
                handler->SetSentErrorMessage(true);
                return false;
            }
            return Send(handler, EC::Add(owner, a));
        }

        static bool HandleList(ChatHandler* handler)
        {
            Player* owner = Owner(handler);
            if (!owner)
                return false;
            return Send(handler, EC::List(owner));
        }

        static bool HandleRemove(ChatHandler* handler, Tail which)
        {
            Player* owner = Owner(handler);
            if (!owner)
                return false;
            return Send(handler, EC::Remove(owner, std::string(which).empty() ? "all" : std::string(which)));
        }

        static bool HandleStay(ChatHandler* handler)
        {
            Player* owner = Owner(handler);
            if (!owner)
                return false;
            EC::SetStay(owner, true);
            handler->SendSysMessage("Companions: stay.");
            return true;
        }

        static bool HandleFollow(ChatHandler* handler)
        {
            Player* owner = Owner(handler);
            if (!owner)
                return false;
            EC::SetStay(owner, false);
            handler->SendSysMessage("Companions: follow.");
            return true;
        }

        static bool HandleAssist(ChatHandler* handler, Tail onOff)
        {
            Player* owner = Owner(handler);
            if (!owner)
                return false;
            std::string const v = EC::Lower(std::string(onOff));
            if (v != "on" && v != "off")
            {
                handler->SendSysMessage("Syntax: .efparty assist on|off");
                handler->SetSentErrorMessage(true);
                return false;
            }
            EC::SetAssist(owner, v == "on");
            handler->PSendSysMessage("Companions: assist {} ({}).", v,
                                     v == "on" ? "the DPS attack your target, else the tank's" : "the DPS only fight back");
            return true;
        }

        static bool HandleLevel(ChatHandler* handler)
        {
            Player* owner = Owner(handler);
            if (!owner)
                return false;
            uint32 const n = EC::Resync(owner);
            handler->PSendSysMessage("Companions: {} set to level {}.", n, owner->GetLevel());
            return true;
        }

        static bool HandleStats(ChatHandler* handler, Tail what)
        {
            Player* owner = Owner(handler);
            if (!owner)
                return false;
            if (EC::Lower(std::string(what)) == "reset")
            {
                EC::ResetStats(owner);
                handler->SendSysMessage("Companions: stats reset.");
                return true;
            }
            return Send(handler, EC::StatsLines(owner));
        }

        static bool HandleThreat(ChatHandler* handler)
        {
            Player* owner = Owner(handler);
            if (!owner)
                return false;
            return Send(handler, EC::ThreatLines(owner));
        }

        static bool HandleReload(ChatHandler* handler)
        {
            EC::ReadConfig(true);
            bool const ok = EC::Load();
            handler->PSendSysMessage("Companions: {} archetypes loaded{}.", EC::Archetypes().size(), ok ? "" : " (see the server log)");
            return true;
        }
    };
}

void AddEmberfallCompanionAIScripts();

void AddEmberfallCompanionsScripts()
{
    new emberfall_companions_world();
    new emberfall_companions_player();
    new emberfall_companions_commands();
    AddEmberfallCompanionAIScripts();
}
