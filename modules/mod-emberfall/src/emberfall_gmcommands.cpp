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

// Emberfall GM command fixes (no core patch; the pure parts in src/gmcommands/EfGmCommandLogic.h):
//
// `.modify speed [all|walk|backwalk|swim|fly] N` keeps its rate. The stock handlers (cs_modify) set the speed rate
// once, and the next Unit::UpdateSpeed (any speed aura applied or removed, a mount, a dismount, a daze in combat)
// recalculates it from the auras and overwrites it. Here the GM's rate per player and move type is kept and set again
// on the player's next update whenever a recalculation replaced it; `.modify speed 1` (per variant) drops it and
// recalculates (auras and mount count again), logging out forgets it. The stock `walk`, `backwalk`, `swim` and `fly`
// handlers only stored the rate (SetSpeedRate) without telling the client; here every variant sends it (SetSpeed).
//
// `.die` on a creature credits the GM: the creature is tapped for them (unless someone tapped it already), the player
// damage requirement is lowered as if they did all of it, and Unit::Kill(gm, creature) runs, so the corpse has loot
// (lootable, the sparkle), the kill gives XP and quest credit like a real one. Players, pets and the rest keep the stock
// `.die` (cs_misc HandleDieCommand: with Die.Command.Mode 1 it kills without loot or reward).
//
// Both through AllCommandScript::OnTryExecuteCommand: the core calls it after matching the typed words in its command
// tree, before the stock handler; returning false skips the stock handler. Only well-formed calls the invoker may run are
// taken over; anything else (a bad value, no target) goes on to the stock handler and its messages.

#include "AccountMgr.h"
#include "AllCommandScript.h"
#include "Chat.h"
#include "Creature.h"
#include "Language.h"
#include "Log.h"
#include "Player.h"
#include "PlayerScript.h"
#include "RBAC.h"
#include "ScriptMgr.h"
#include "WorldSession.h"

#include "gmcommands/EfGmCommandLogic.h"

#include <unordered_map>

using namespace Emberfall::GmCommands;

namespace
{
    // The GM rates per character (GUID counter); in memory only: a logout or a restart forgets them.
    std::unordered_map<ObjectGuid::LowType, Rates> s_rates;

    uint32 PermissionOf(SpeedKind kind, bool bare)
    {
        switch (kind)
        {
            case SpeedKind::All: return bare ? rbac::RBAC_PERM_COMMAND_MODIFY_SPEED : rbac::RBAC_PERM_COMMAND_MODIFY_SPEED_ALL;
            case SpeedKind::Walk: return rbac::RBAC_PERM_COMMAND_MODIFY_SPEED_WALK;
            case SpeedKind::BackWalk: return rbac::RBAC_PERM_COMMAND_MODIFY_SPEED_BACKWALK;
            case SpeedKind::Swim: return rbac::RBAC_PERM_COMMAND_MODIFY_SPEED_SWIM;
            case SpeedKind::Fly: return rbac::RBAC_PERM_COMMAND_MODIFY_SPEED_FLY;
        }
        return rbac::RBAC_PERM_COMMAND_MODIFY_SPEED;
    }

    // cs_modify's messages per variant (LANG_YOU_CHANGE_*, LANG_YOURS_*_CHANGED).
    std::pair<AcoreStrings, AcoreStrings> MessagesOf(SpeedKind kind)
    {
        switch (kind)
        {
            case SpeedKind::Walk: return { LANG_YOU_CHANGE_SPEED, LANG_YOURS_SPEED_CHANGED };
            case SpeedKind::BackWalk: return { LANG_YOU_CHANGE_BACK_SPEED, LANG_YOURS_BACK_SPEED_CHANGED };
            case SpeedKind::Swim: return { LANG_YOU_CHANGE_SWIM_SPEED, LANG_YOURS_SWIM_SPEED_CHANGED };
            case SpeedKind::Fly: return { LANG_YOU_CHANGE_FLY_SPEED, LANG_YOURS_FLY_SPEED_CHANGED };
            default: return { LANG_YOU_CHANGE_ASPEED, LANG_YOURS_ASPEED_CHANGED };
        }
    }

    void LogUsage(ChatHandler& handler, std::string_view cmdStr)
    {
        WorldSession* session = handler.GetSession();
        Player* player = session ? session->GetPlayer() : nullptr;
        if (!player || AccountMgr::IsPlayerAccount(session->GetSecurity()))
            return;
        LOG_GM(session->GetAccountId(), "Command: {} [Player: {} ({}) (Account: {}) Map: {} X: {} Y: {} Z: {} Selected: {}] (mod-emberfall)",
               cmdStr, player->GetName(), player->GetGUID().ToString(), session->GetAccountId(), player->GetMapId(),
               player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), player->GetTarget().ToString());
    }

    // `.modify speed ...` with a number: false once handled (the stock handler is skipped).
    bool HandleSpeed(ChatHandler& handler, Parsed const& p, bool bare)
    {
        if (!handler.GetSession() || !handler.HasPermission(PermissionOf(p.kind, bare)) || !RateInBounds(p.rate))
            return true; // the stock handler: its permission check, LANG_BAD_VALUE, its usage
        Player* target = handler.getSelectedPlayerOrSelf();
        if (!target)
            return true;
        if (handler.HasLowerSecurity(target))
            return false; // the message is sent
        // cs_modify refuses a player on a flight path, except for `fly`.
        if (target->IsInFlight() && p.kind != SpeedKind::Fly)
        {
            handler.SendErrorMessage(LANG_CHAR_IN_FLIGHT, handler.GetNameLink(target).c_str());
            return false;
        }

        auto [you, yours] = MessagesOf(p.kind);
        handler.PSendSysMessage(you, p.rate, handler.GetNameLink(target));
        if (handler.needReportToTarget(target))
            ChatHandler(target->GetSession()).PSendSysMessage(yours, handler.GetNameLink(), p.rate);

        Rates& rates = s_rates[target->GetGUID().GetCounter()];
        for (int type : MoveTypesOf(p.kind))
        {
            rates.Set(type, p.rate);
            if (rates.Holds(type))
                target->SetSpeed(UnitMoveType(type), p.rate, true);
            else
                target->UpdateSpeed(UnitMoveType(type), true); // rate 1: the normal calculation (auras, mount) again
        }
        if (rates.Empty())
            s_rates.erase(target->GetGUID().GetCounter());
        LOG_INFO("module", "Emberfall GM speed: {} (guid {}) {} rate {:.2f}{}", target->GetName(), target->GetGUID().GetCounter(),
                 bare ? "all" : "variant", p.rate, s_rates.count(target->GetGUID().GetCounter()) ? ", held" : ", released");
        return false;
    }

    // `.die` on a living creature: false once handled.
    bool HandleDie(ChatHandler& handler)
    {
        Player* gm = handler.GetSession() ? handler.GetSession()->GetPlayer() : nullptr;
        if (!gm || !handler.HasPermission(rbac::RBAC_PERM_COMMAND_DIE) || !gm->GetTarget())
            return true;
        Unit* target = handler.getSelectedUnit();
        Creature* creature = target ? target->ToCreature() : nullptr;
        // Players, pets, guardians and charmed units, the dead: the stock `.die`.
        if (!creature || !creature->IsAlive() || creature->IsPet() || creature->IsGuardian() || creature->IsTotem() ||
            creature->GetCharmerOrOwnerGUID() || !creature->IsInMap(gm))
            return true;

        if (!creature->hasLootRecipient())
            creature->SetLootRecipient(gm);
        creature->LowerPlayerDamageReq(creature->GetMaxHealth(), true, gm->GetLevel());
        Unit::Kill(gm, creature);
        LOG_INFO("module", "Emberfall .die: {} (guid {}) killed {} (entry {}, guid {}) with credit", gm->GetName(),
                 gm->GetGUID().GetCounter(), creature->GetName(), creature->GetEntry(), creature->GetSpawnId());
        return false;
    }
}

class emberfall_gmcommands_command : public AllCommandScript
{
public:
    emberfall_gmcommands_command() : AllCommandScript("emberfall_gmcommands_command", { ALLCOMMANDHOOK_ON_TRY_EXECUTE_COMMAND }) { }

    bool OnTryExecuteCommand(ChatHandler& handler, std::string_view cmdStr) override
    {
        if (handler.IsConsole())
            return true;
        Parsed p = Parse(cmdStr);
        bool handled = false;
        switch (p.command)
        {
            case Command::Speed: handled = !HandleSpeed(handler, p, Words(cmdStr).size() == 3); break;
            case Command::Die: handled = !HandleDie(handler); break;
            default: return true;
        }
        if (handled)
            LogUsage(handler, cmdStr);
        return !handled;
    }
};

class emberfall_gmcommands_player : public PlayerScript
{
public:
    emberfall_gmcommands_player()
        : PlayerScript("emberfall_gmcommands_player", { PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT }) { }

    // A recalculation (Unit::UpdateSpeed) replaced a held rate: set it again (sent to the client).
    void OnPlayerUpdate(Player* player, uint32 /*diff*/) override
    {
        if (s_rates.empty())
            return;
        auto it = s_rates.find(player->GetGUID().GetCounter());
        if (it == s_rates.end() || !player->IsInWorld() || player->IsBeingTeleported() || player->IsInFlight())
            return;
        for (int type : it->second.Drifted([player](int t) { return player->GetSpeedRate(UnitMoveType(t)); }))
        {
            LOG_INFO("module", "Emberfall GM speed: {} move type {} recalculated to {:.2f}, held rate {:.2f} set again", player->GetName(),
                     type, player->GetSpeedRate(UnitMoveType(type)), it->second.rate[type]);
            player->SetSpeed(UnitMoveType(type), it->second.rate[type], true);
        }
    }

    void OnPlayerLogin(Player* player) override { s_rates.erase(player->GetGUID().GetCounter()); }

    void OnPlayerLogout(Player* player) override { s_rates.erase(player->GetGUID().GetCounter()); }
};

void AddEmberfallGmCommandsScripts()
{
    new emberfall_gmcommands_command();
    new emberfall_gmcommands_player();
}
