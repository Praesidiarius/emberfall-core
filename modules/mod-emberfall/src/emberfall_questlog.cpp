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

// Emberfall's quest log of 50 (feedback #251, Emberfall docs/design/quest-log.md; the core's PATCHES.md #36): slots
// 25-49 are no update fields, so a player is told about them on the EFQL channel, an addon whisper to themselves (the
// EFA way). On each update the module compares the player's slots 25-49 (Player::GetQuestSlotField) with what it told
// them, and sends one message per slot that changed; the first update after a login sends `1;0;CLR` and every slot
// in use. The protocol and the diff are src/questlog/EfQuestLogLogic.h (tests/questlog_tests.cpp).
//
// A client without EFQL (Emberfall before 0.4.x) ignores the channel (an EF prefix is reserved) and sees 25 slots.
// GM command `.efquestlog`: the selected player's 50 slots in use.

#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "DataMap.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "questlog/EfQuestLogLogic.h"

using namespace Acore::ChatCommands;

namespace
{
    namespace QL = Emberfall::QuestLogSync;

    constexpr char const* kDataKey = "emberfall_questlog";

    struct PlayerQuestLog : public DataMap::Base
    {
        QL::State state;
    };

    QL::Ext Collect(Player const* player)
    {
        QL::Ext now{};
        for (uint16 i = 0; i < QL::kExtSlots; ++i)
            for (uint8 w = 0; w < QL::kSlotWords; ++w)
                now[i][w] = player->GetQuestSlotField(uint16(QL::kFieldSlots + i), w);
        return now;
    }

    void Send(Player* player, std::string const& text)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, text);
        player->SendDirectMessage(&data);
    }

    class emberfall_questlog_player : public PlayerScript
    {
    public:
        emberfall_questlog_player()
            : PlayerScript("emberfall_questlog_player", { PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT }) { }

        void OnPlayerUpdate(Player* player, uint32 /*diff*/) override
        {
            if (!player || !player->IsInWorld() || !player->GetSession())
                return;
            PlayerQuestLog* data = player->CustomData.GetDefault<PlayerQuestLog>(kDataKey);
            // 125 words compared: cheaper than any throttle's bookkeeping
            for (std::string const& body : QL::Diff(data->state, Collect(player)))
                Send(player, QL::Text(body));
        }

        // A fresh start: the client forgot slots 25-49 at its login; the first update sends CLR and the slots in use.
        void OnPlayerLogin(Player* player) override { player->CustomData.Erase(kDataKey); }
        void OnPlayerLogout(Player* player) override { player->CustomData.Erase(kDataKey); }
    };

    class emberfall_questlog_commands : public CommandScript
    {
    public:
        emberfall_questlog_commands() : CommandScript("emberfall_questlog_commands") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable commandTable =
            {
                { "efquestlog", HandleEfQuestLog, SEC_GAMEMASTER, Console::No },
            };
            return commandTable;
        }

        // `EFQL <slot> <quest> <state> <c0> <c1> <c2> <c3> <time>` per slot in use of the selected player (or oneself),
        // then `EFQL end <used>/<slots>`.
        static bool HandleEfQuestLog(ChatHandler* handler)
        {
            Player* player = handler->getSelectedPlayerOrSelf();
            if (!player)
                return false;
            uint32 used = 0;
            for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
                if (uint32 const quest = player->GetQuestSlotQuestId(slot))
                {
                    ++used;
                    handler->PSendSysMessage("EFQL {} {} {} {} {} {} {} {}", slot, quest, player->GetQuestSlotState(slot),
                        player->GetQuestSlotCounter(slot, 0), player->GetQuestSlotCounter(slot, 1),
                        player->GetQuestSlotCounter(slot, 2), player->GetQuestSlotCounter(slot, 3), player->GetQuestSlotTime(slot));
                }
            handler->PSendSysMessage("EFQL end {}/{}", used, uint32(MAX_QUEST_LOG_SIZE));
            return true;
        }
    };
}

void AddEmberfallQuestLogScripts()
{
    new emberfall_questlog_player();
    new emberfall_questlog_commands();
}
