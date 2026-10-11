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

// Emberfall talents (docs/design/talents.md §3.2; WPs M1-M5): the hooks and GM commands of the spec and
// talent system (src/talents/EfTalents.h). Everything is inert unless Emberfall.Talents.Enable = 1, and for
// a class only when ef_spec has rows for it; independent of Emberfall.OwnData.

#include "AllSpellScript.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "DatabaseEnv.h"
#include "EfTalents.h"
#include "Player.h"
#include "PlayerScript.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "WorldScript.h"

using namespace Acore::ChatCommands;

namespace
{
    class emberfall_talents_world : public WorldScript
    {
    public:
        emberfall_talents_world() : WorldScript("emberfall_talents_world",
            { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED }) { }

        void OnAfterConfigLoad(bool reload) override { Emberfall::Talents::ReadConfig(reload); }

        void OnBeforeWorldInitialized() override { Emberfall::Talents::Load(); }
    };

    class emberfall_talents_player : public PlayerScript
    {
    public:
        emberfall_talents_player() : PlayerScript("emberfall_talents_player", {
            PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LEVEL_CHANGED, PLAYERHOOK_ON_CALCULATE_TALENTS_POINTS, PLAYERHOOK_CAN_LEARN_TALENT,
            PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT, PLAYERHOOK_ON_DELETE_FROM_DB, PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_PLAYER_ENTER_COMBAT }) { }

        void OnPlayerLogin(Player* player) override { Emberfall::Talents::OnLogin(player); }

        void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override { Emberfall::Talents::OnLevelChanged(player, oldLevel); }

        // §3.5: a class with Emberfall trees has no stock talent points (the core then resets stock talents
        // it still had, Player::InitTalentForLevel) ...
        void OnPlayerCalculateTalentsPoints(Player const* player, uint32& talentPointsForLevel) override
        {
            if (Emberfall::Talents::Active(player))
                talentPointsForLevel = 0;
        }

        // ... and learns none (CMSG_LEARN_TALENT, CMSG_LEARN_PREVIEW_TALENTS and the GM paths).
        bool OnPlayerCanLearnTalent(Player* player, TalentEntry const* /*talent*/, uint32 /*rank*/) override
        {
            return !Emberfall::Talents::Active(player);
        }

        // §4.1: the EFT protocol rides on addon whispers to oneself; every `EFT\t` addon whisper is consumed
        // (never delivered or echoed), only the player's own is answered.
        bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg, Player* receiver) override
        {
            if (type != CHAT_MSG_WHISPER || language != uint32(LANG_ADDON) || msg.compare(0, ef::eft::kPrefix.size(), ef::eft::kPrefix) != 0)
                return true;
            if (receiver == player)
                Emberfall::Talents::HandleMessage(player, std::string_view(msg).substr(ef::eft::kPrefix.size()));
            return false;
        }

        // §1.5: the switch cast finishes (or fails) here, outside the spell's own processing ...
        void OnPlayerUpdate(Player* player, uint32 /*diff*/) override { Emberfall::Talents::OnUpdate(player); }

        // ... and entering combat cancels it (§1.6).
        void OnPlayerEnterCombat(Player* player, Unit* /*enemy*/) override { Emberfall::Talents::OnEnterCombat(player); }

        void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 guid) override
        {
            for (char const* table : { "ef_character_spec", "ef_character_loadout", "ef_character_actions", "ef_character_granted" })
                trans->Append("DELETE FROM `{}` WHERE `guid` = {}", table, guid);
        }
    };

    // The switch spell went off (Spell::_cast's end, after its last check). An AllSpellScript instead of the
    // SpellScript §3.2 planned: it needs no spell_script_names row, so the same code serves our content's switch
    // spell and the stock-test profile's spell_dbc row.
    class emberfall_talents_spells : public AllSpellScript
    {
    public:
        emberfall_talents_spells() : AllSpellScript("emberfall_talents_spells", { ALLSPELLHOOK_ON_CAST }) { }

        void OnSpellCast(Spell* /*spell*/, Unit* caster, SpellInfo const* spellInfo, bool /*skipCheck*/) override
        {
            if (caster && spellInfo)
                if (Player* player = caster->ToPlayer())
                    Emberfall::Talents::OnSpellCast(player, spellInfo->Id);
        }
    };

    // GM test commands (§3.2): `.ef talents dump|reset|reload`, `.ef spec ID`, `.ef send BODY`.
    class emberfall_talents_commands : public CommandScript
    {
    public:
        emberfall_talents_commands() : CommandScript("emberfall_talents_commands") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable talentsTable =
            {
                { "dump",   HandleDump,   SEC_GAMEMASTER,    Console::No },
                { "reset",  HandleReset,  SEC_GAMEMASTER,    Console::No },
                { "reload", HandleReload, SEC_ADMINISTRATOR, Console::Yes },
            };
            static ChatCommandTable efTable =
            {
                { "talents", talentsTable },
                { "spec",    HandleSpec,  SEC_GAMEMASTER,    Console::No },
                { "send",    HandleSend,  SEC_GAMEMASTER,    Console::No },
            };
            static ChatCommandTable commandTable =
            {
                { "ef", efTable },
            };
            return commandTable;
        }

        // The selected player, or the GM.
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
            Emberfall::Talents::Dump(target, lines);
            for (std::string const& line : lines)
                handler->SendSysMessage(line);
            return true;
        }

        static bool HandleReset(ChatHandler* handler)
        {
            Player* target = Target(handler);
            if (!target)
                return false;
            std::string out;
            bool const ok = Emberfall::Talents::GmReset(target, out);
            handler->SendSysMessage(out);
            handler->SetSentErrorMessage(!ok);
            return ok;
        }

        static bool HandleReload(ChatHandler* handler)
        {
            bool const ok = Emberfall::Talents::Load();
            handler->SendSysMessage(ok ? "Emberfall talents reloaded (online players are re-validated on their next request)."
                                       : "Emberfall talents: reload failed or the system is off (see the server log).");
            return true;
        }

        static bool HandleSpec(ChatHandler* handler, uint32 specId)
        {
            Player* target = Target(handler);
            if (!target)
                return false;
            std::string out;
            bool const ok = Emberfall::Talents::GmSetSpec(target, specId, out);
            handler->SendSysMessage(out);
            handler->SetSentErrorMessage(!ok);
            return ok;
        }

        // A test aid: handles BODY as if the GM's client had sent `EFT\tBODY`; the answers go to the GM's client
        // as usual (addon whispers).
        static bool HandleSend(ChatHandler* handler, Tail body)
        {
            Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
            if (!player || body.empty())
                return false;
            Emberfall::Talents::HandleMessage(player, body);
            return true;
        }
    };
}

void AddEmberfallTalentsScripts()
{
    new emberfall_talents_world();
    new emberfall_talents_player();
    new emberfall_talents_spells();
    new emberfall_talents_commands();
}
