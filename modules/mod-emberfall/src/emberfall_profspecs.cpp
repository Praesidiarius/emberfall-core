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

// Emberfall profession specialisations (Emberfall docs/design/professions.md §10): the hooks and GM commands of
// src/profspecs/EfProfSpecs.h. Inert unless the ef_prof_* world tables have rows (Emberfall.ProfSpecs.Enable, default 1).

#include "AllCreatureScript.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "DatabaseEnv.h"
#include "EfProfSpecs.h"
#include "Player.h"
#include "PlayerScript.h"
#include "SharedDefines.h"
#include "WorldScript.h"

using namespace Acore::ChatCommands;

namespace
{
    constexpr std::string_view kPrefix = "EFP\t";

    class emberfall_profspecs_world : public WorldScript
    {
    public:
        emberfall_profspecs_world() : WorldScript("emberfall_profspecs_world",
            { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED }) { }

        void OnAfterConfigLoad(bool reload) override { Emberfall::ProfSpecs::ReadConfig(reload); }
        void OnBeforeWorldInitialized() override { Emberfall::ProfSpecs::Load(); }
    };

    class emberfall_profspecs_player : public PlayerScript
    {
    public:
        emberfall_profspecs_player() : PlayerScript("emberfall_profspecs_player", {
            PLAYERHOOK_ON_LOGIN, PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT, PLAYERHOOK_ON_SET_SKILL, PLAYERHOOK_ON_BEFORE_SEND_LOOT,
            PLAYERHOOK_ON_UPDATE_CRAFTING_SKILL, PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_DELETE_FROM_DB }) { }

        void OnPlayerLogin(Player* player) override { Emberfall::ProfSpecs::OnLogin(player); }

        // The EFP protocol: every `EFP\t` addon whisper is consumed, only the player's own (to itself) answered.
        bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg, Player* receiver) override
        {
            if (type != CHAT_MSG_WHISPER || language != uint32(LANG_ADDON) || msg.compare(0, kPrefix.size(), kPrefix) != 0)
                return true;
            if (receiver == player)
                Emberfall::ProfSpecs::HandleMessage(player, std::string_view(msg).substr(kPrefix.size()));
            return false;
        }

        // Learned (STATE) or unlearned (its ranks go, GONE).
        void OnPlayerSetSkill(Player* player, uint32 skillId, uint32 /*value*/, uint32 /*max*/, uint32 /*step*/, uint32 newValue) override
        {
            Emberfall::ProfSpecs::OnSetSkill(player, skillId, newValue);
        }

        // Full Baskets / Rich Seams: one more herb or ore, rolled once per node's loot.
        void OnPlayerBeforeSendLoot(Player* player, ObjectGuid lootGuid, Loot* loot) override
        {
            Emberfall::ProfSpecs::OnBeforeSendLoot(player, lootGuid, loot);
        }

        // The Steady Hand and The Careful Reading: after a craft made its item (Player::UpdateCraftSkill).
        void OnPlayerUpdateCraftingSkill(Player* player, SkillLineAbilityEntry const* skill, uint32 /*currentLevel*/, uint32& /*gain*/) override
        {
            Emberfall::ProfSpecs::OnCraft(player, skill);
        }

        // Far Sight: the far nodes every few seconds while tracked (a cheap no-op without ranks).
        void OnPlayerUpdate(Player* player, uint32 diff) override { Emberfall::ProfSpecs::OnUpdate(player, diff); }

        void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 guid) override
        {
            trans->Append("DELETE FROM `ef_character_prof_rank` WHERE `guid` = {}", guid);
            trans->Append("DELETE FROM `ef_character_prof_granted` WHERE `guid` = {}", guid);
        }
    };

    // The reset at the profession trainer (SV gathering-specs.md §2: a whole tree, for coin).
    class emberfall_profspecs_trainers : public AllCreatureScript
    {
    public:
        emberfall_profspecs_trainers() : AllCreatureScript("emberfall_profspecs_trainers") { }

        bool CanCreatureGossipHello(Player* player, Creature* creature) override
        {
            return Emberfall::ProfSpecs::OnGossipHello(player, creature);
        }

        bool CanCreatureGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action) override
        {
            return Emberfall::ProfSpecs::OnGossipSelect(player, creature, sender, action);
        }
    };

    // `.efprof dump|reset [skill]|reload|send BODY`.
    class emberfall_profspecs_commands : public CommandScript
    {
    public:
        emberfall_profspecs_commands() : CommandScript("emberfall_profspecs_commands") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable profTable =
            {
                { "dump",   HandleDump,   SEC_GAMEMASTER,    Console::No },
                { "reset",  HandleReset,  SEC_GAMEMASTER,    Console::No },
                { "reload", HandleReload, SEC_ADMINISTRATOR, Console::Yes },
                { "send",   HandleSend,   SEC_GAMEMASTER,    Console::No },
            };
            static ChatCommandTable commandTable =
            {
                { "efprof", profTable },
            };
            return commandTable;
        }

        static Player* Target(ChatHandler* handler)
        {
            Player* target = handler->getSelectedPlayerOrSelf();
            if (!target)
            {
                handler->SendSysMessage("No player selected.");
                handler->SetSentErrorMessage(true);
            }
            return target;
        }

        static bool HandleDump(ChatHandler* handler)
        {
            Player* target = Target(handler);
            if (!target)
                return false;
            std::vector<std::string> lines;
            Emberfall::ProfSpecs::Dump(target, lines);
            for (std::string const& line : lines)
                handler->SendSysMessage(line);
            return true;
        }

        static bool HandleReset(ChatHandler* handler, Optional<uint32> skill)
        {
            Player* target = Target(handler);
            if (!target)
                return false;
            std::string out;
            bool const ok = Emberfall::ProfSpecs::GmReset(target, skill.value_or(0), out);
            handler->SendSysMessage(out);
            handler->SetSentErrorMessage(!ok);
            return ok;
        }

        static bool HandleReload(ChatHandler* handler)
        {
            bool const ok = Emberfall::ProfSpecs::Load();
            handler->SendSysMessage(ok ? "Emberfall profession specialisations reloaded." : "Emberfall profession specialisations: off (see the log).");
            return true;
        }

        static bool HandleSend(ChatHandler* handler, Tail body)
        {
            Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
            if (!player || body.empty())
                return false;
            Emberfall::ProfSpecs::HandleMessage(player, body);
            return true;
        }
    };
}

void AddEmberfallProfSpecsScripts()
{
    new emberfall_profspecs_world();
    new emberfall_profspecs_player();
    new emberfall_profspecs_trainers();
    new emberfall_profspecs_commands();
}
