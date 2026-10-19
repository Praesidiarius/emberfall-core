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

// Emberfall's world editor (Emberfall docs/design/world-editor.md): the hooks and the operator commands of
// src/editor/EfEditor.h. Inert unless Emberfall.Editor.Enable = 1 with Emberfall.Editor.Database set (the build realm,
// or a maintainer's local server); alpha1 runs with it off and never answers an EFE message.

#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "EfEditor.h"
#include "Player.h"
#include "PlayerScript.h"
#include "SharedDefines.h"
#include "WorldScript.h"

using namespace Acore::ChatCommands;

namespace
{
    constexpr std::string_view kPrefix = "EFE\t";

    class emberfall_editor_world : public WorldScript
    {
    public:
        emberfall_editor_world() : WorldScript("emberfall_editor_world",
            { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE }) { }

        void OnAfterConfigLoad(bool reload) override { Emberfall::Editor::ReadConfig(reload); }
        void OnStartup() override { Emberfall::Editor::OnStartup(); }
        void OnUpdate(uint32 diff) override { Emberfall::Editor::OnUpdate(diff); }
    };

    class emberfall_editor_player : public PlayerScript
    {
    public:
        emberfall_editor_player() : PlayerScript("emberfall_editor_player", {
            PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT }) { }

        void OnPlayerLogin(Player* player) override { Emberfall::Editor::OnLogin(player); }
        void OnPlayerLogout(Player* player) override { Emberfall::Editor::OnLogout(player); }

        // The EFE protocol: every `EFE\t` addon whisper is consumed (never delivered to anyone); only the player's own
        // (to itself) is handled, anything else is refused and logged.
        bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg, Player* receiver) override
        {
            if (type != CHAT_MSG_WHISPER || msg.compare(0, kPrefix.size(), kPrefix) != 0)
                return true;
            if (receiver == player && language == uint32(LANG_ADDON))
                Emberfall::Editor::HandleMessage(player, std::string_view(msg).substr(kPrefix.size()));
            else
                Emberfall::Editor::HandleForeign(player);
            return false;
        }
    };

    // `.efedit status`, `.efedit access ACCOUNT_ID` (the site's nudge over SOAP after a grant, revoke or freeze; 0 =
    // every account), `.efedit refresh`.
    class emberfall_editor_commands : public CommandScript
    {
    public:
        emberfall_editor_commands() : CommandScript("emberfall_editor_commands") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable editTable =
            {
                { "status",  HandleStatus,  SEC_GAMEMASTER,    Console::Yes },
                { "access",  HandleAccess,  SEC_ADMINISTRATOR, Console::Yes },
                { "refresh", HandleRefresh, SEC_ADMINISTRATOR, Console::Yes },
            };
            static ChatCommandTable commandTable =
            {
                { "efedit", editTable },
            };
            return commandTable;
        }

        static bool HandleStatus(ChatHandler* handler)
        {
            std::vector<std::string> lines;
            Emberfall::Editor::Status(lines);
            for (std::string const& line : lines)
                handler->SendSysMessage(line);
            return true;
        }

        static bool HandleAccess(ChatHandler* handler, uint32 accountId)
        {
            Emberfall::Editor::Refresh(accountId);
            handler->PSendSysMessage("Emberfall world editor: access of account {} re-read.", accountId);
            return true;
        }

        static bool HandleRefresh(ChatHandler* handler)
        {
            Emberfall::Editor::Refresh(0);
            handler->SendSysMessage("Emberfall world editor: access and review states re-read.");
            return true;
        }
    };
}

void AddEmberfallEditorScripts()
{
    new emberfall_editor_world();
    new emberfall_editor_player();
    new emberfall_editor_commands();
}
