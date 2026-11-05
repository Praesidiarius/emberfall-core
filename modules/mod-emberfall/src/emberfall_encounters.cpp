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

// Emberfall mini-encounters (Emberfall docs/design/mini-encounters.md): the framework fishing's fight on the line uses
// now and hunter taming later. One EFN protocol (addon whispers to oneself, prefix `EFN`, EfEncounterWire.h), one open
// encounter per player (EfEncounterLogic.h's Book), the item groups (ef_enc_item_group), a kind's handler per letter
// (EfEncounterMgr.h). This file:
//   - catches every `EFN\t` addon whisper (PlayerScript::OnPlayerCanUseChat), answers only the player's own (to
//     itself), drops more than 20 a second (ERR;THROTTLE), and hands it to its kind's handler (unknown or off kind:
//     ERR;NOENC; undecodable: ERR;DECODE);
//   - frees a player's open encounter on logout, map change and death (the kind's OnFree);
//   - loads ef_enc_item_group (boot line `>> Emberfall encounters: on (N groups, M items)`);
//   - `.efenc dump <player>` (GM): the player's open encounter and their last ten EFN messages; `.efenc reload`.
// Config: Emberfall.Encounters.Enable (1).
#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldScript.h"
#include "WorldSession.h"
#include "encounters/EfEncounterMgr.h"
#include <mutex>
#include <unordered_map>

using namespace Acore::ChatCommands;

namespace
{
    namespace E = Emberfall::Encounters;

    bool s_enable = true;
    std::mutex s_lock;   // the book, the log, the throttles, the sequence numbers
    E::Book s_book;
    E::MessageLog s_log;
    E::ItemGroups s_groups;
    std::unordered_map<std::uint64_t, efenc::Throttle> s_throttle;
    std::unordered_map<std::uint64_t, std::uint32_t> s_seq;
    std::unordered_map<char, E::Kind*> s_kinds;

    std::uint64_t Key(Player* player) { return player->GetGUID().GetCounter(); }
    std::uint64_t NowMs() { return std::uint64_t(GameTime::GetGameTimeMS().count()); }

    void LoadGroups()
    {
        s_groups.Clear();
        if (QueryResult result = WorldDatabase.Query("SELECT `group_name`, `item`, `band` FROM `ef_enc_item_group`"))
        {
            do
            {
                Field* f = result->Fetch();
                s_groups.Add(f[0].Get<std::string>(), f[1].Get<uint32>(), f[2].Get<uint8>());
            } while (result->NextRow());
        }
        LOG_INFO("server.loading", ">> Emberfall encounters: {} ({} groups, {} items)", s_enable ? "on" : "off",
                 s_groups.Groups(), s_groups.Rows());
    }

    void Free(Player* player, E::FreeReason reason)
    {
        std::optional<E::Open> open;
        E::Kind* kind = nullptr;
        {
            std::lock_guard<std::mutex> guard(s_lock);
            open = s_book.Of(Key(player));
            if (open)
            {
                auto it = s_kinds.find(open->kind);
                kind = it == s_kinds.end() ? nullptr : it->second;
            }
        }
        if (open && kind)
            kind->OnFree(player, *open, reason);
        std::lock_guard<std::mutex> guard(s_lock);
        if (open)
            s_book.End(Key(player), open->id);
        if (reason == E::FreeReason::Logout)
        {
            s_throttle.erase(Key(player));
            s_seq.erase(Key(player));
            s_log.Forget(Key(player));
        }
    }

    void HandleBody(Player* player, std::string_view body)
    {
        std::uint32_t const now = std::uint32_t(NowMs());
        {
            std::lock_guard<std::mutex> guard(s_lock);
            s_log.Add(Key(player), true, std::string(body));
            if (!s_throttle[Key(player)].Allow(now))
            {
                LOG_DEBUG("module", "Emberfall encounters: {} over 20 EFN messages a second", player->GetName());
                return;   // dropped; answering every one would be the flood the throttle stops
            }
        }
        std::optional<efenc::Message> msg = efenc::Decode(body);
        if (!msg)
        {
            E::SendErr(player, 0, '-', efenc::kErrDecode);
            return;
        }
        E::Kind* kind = nullptr;
        {
            std::lock_guard<std::mutex> guard(s_lock);
            auto it = s_kinds.find(msg->kind);
            kind = it == s_kinds.end() ? nullptr : it->second;
        }
        if (!s_enable || !kind || !kind->Enabled())
        {
            E::SendErr(player, msg->seq, msg->kind, efenc::kErrNoEnc, "OFF");
            return;
        }
        kind->OnMessage(player, *msg);
    }

    class emberfall_encounters_world : public WorldScript
    {
    public:
        emberfall_encounters_world()
            : WorldScript("emberfall_encounters_world", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_LOAD_CUSTOM_DATABASE_TABLE }) { }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            s_enable = sConfigMgr->GetOption<bool>("Emberfall.Encounters.Enable", true, false);
        }

        void OnLoadCustomDatabaseTable() override { LoadGroups(); }
    };

    class emberfall_encounters_player : public PlayerScript
    {
    public:
        emberfall_encounters_player()
            : PlayerScript("emberfall_encounters_player", { PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_MAP_CHANGED,
                PLAYERHOOK_ON_PLAYER_JUST_DIED, PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT }) { }

        void OnPlayerLogout(Player* player) override { Free(player, E::FreeReason::Logout); }
        void OnPlayerMapChanged(Player* player) override { Free(player, E::FreeReason::MapChange); }
        void OnPlayerJustDied(Player* player) override { Free(player, E::FreeReason::Died); }

        // EFN: every `EFN\t` addon whisper is consumed, only the player's own (to itself) handled.
        bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg, Player* receiver) override
        {
            if (type != CHAT_MSG_WHISPER || language != uint32(LANG_ADDON) || msg.size() <= efenc::kPrefix.size() ||
                msg.compare(0, efenc::kPrefix.size(), efenc::kPrefix) != 0 || msg[efenc::kPrefix.size()] != '\t')
                return true;
            if (receiver == player)
                HandleBody(player, std::string_view(msg).substr(efenc::kPrefix.size() + 1));
            return false;
        }
    };

    class emberfall_encounters_commands : public CommandScript
    {
    public:
        emberfall_encounters_commands() : CommandScript("emberfall_encounters_commands") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable encTable =
            {
                { "dump",   HandleDump,   SEC_GAMEMASTER, Console::No },
                { "reload", HandleReload, SEC_GAMEMASTER, Console::Yes },
            };
            static ChatCommandTable commandTable =
            {
                { "efenc", encTable },
            };
            return commandTable;
        }

        static bool HandleDump(ChatHandler* handler, Optional<PlayerIdentifier> target)
        {
            if (!target)
                target = PlayerIdentifier::FromTargetOrSelf(handler);
            Player* player = target ? target->GetConnectedPlayer() : nullptr;
            if (!player)
            {
                handler->SendSysMessage("EFENC no such player online");
                return true;
            }
            std::lock_guard<std::mutex> guard(s_lock);
            if (std::optional<E::Open> o = s_book.Of(Key(player)))
                handler->PSendSysMessage("EFENC {} open {} kind {} subject {:x} for {} ms", player->GetName(), o->id, o->kind,
                                         o->subject, NowMs() - o->startMs);
            else
                handler->PSendSysMessage("EFENC {} no open encounter ({} open on the realm)", player->GetName(), s_book.Size());
            for (std::string const& line : s_log.Of(Key(player)))
                handler->PSendSysMessage("EFENC {}", line);
            return true;
        }

        static bool HandleReload(ChatHandler* handler)
        {
            LoadGroups();
            handler->PSendSysMessage("EFENC reloaded: {} groups, {} items", s_groups.Groups(), s_groups.Rows());
            return true;
        }
    };
}

namespace Emberfall::Encounters
{
    void RegisterKind(char kind, Kind* handler)
    {
        std::lock_guard<std::mutex> guard(s_lock);
        s_kinds[kind] = handler;
    }

    bool Enabled() { return s_enable; }

    std::optional<Open> Begin(Player* player, char kind, std::uint64_t subject)
    {
        std::lock_guard<std::mutex> guard(s_lock);
        return s_book.Start(Key(player), kind, subject, NowMs());
    }

    void Finish(Player* player, std::uint32_t id)
    {
        std::lock_guard<std::mutex> guard(s_lock);
        s_book.End(Key(player), id);
    }

    std::optional<Open> Current(Player* player)
    {
        std::lock_guard<std::mutex> guard(s_lock);
        return s_book.Of(Key(player));
    }

    std::uint32_t NextSeq(Player* player)
    {
        std::lock_guard<std::mutex> guard(s_lock);
        return ++s_seq[Key(player)];
    }

    void Send(Player* player, std::string const& body)
    {
        if (body.empty())
            return;
        {
            std::lock_guard<std::mutex> guard(s_lock);
            s_log.Add(Key(player), false, body);
        }
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player,
                                     std::string(efenc::kPrefix) + "\t" + body);
        player->SendDirectMessage(&data);
    }

    void SendErr(Player* player, std::uint32_t seq, char kind, std::string_view code, std::string const& detail)
    {
        efenc::Message m;
        m.seq = seq;
        m.kind = kind;
        m.cmd = "ERR";
        m.fields = {std::string(code)};
        if (!detail.empty())
            m.fields.push_back(detail);
        Send(player, efenc::Encode(m));
    }

    ItemGroups const& Groups() { return s_groups; }
}

void AddEmberfallEncountersScripts()
{
    new emberfall_encounters_world();
    new emberfall_encounters_player();
    new emberfall_encounters_commands();
}
