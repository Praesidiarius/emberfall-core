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

// Emberfall absorbs (Emberfall docs/design/cooldown-manager.md §9.3): the client's Cooldown Manager shows how much a
// shield on the player has left; 3.3.5 never tells the client, so this module does. Every 100 ms (at most) a player
// with absorb auras (school absorb, mana shield) is told, as an addon whisper to themselves (the EFP way), each one
// whose numbers changed: `EFA\t1;0;ABS;<spellId>;<left>;<cap>`; one that went away: `EFA\t1;0;ABS;<spellId>;0;0`.
//   left  the aura's current amount (its absorb effects added up; several auras of one spell add up)
//   cap   a class kit shell (EfClassKit.h IsShellSpell: Frost Shell): ShellCap, a full shell (the stack maximum x what
//         one stack absorbs); any other absorb: the highest amount seen since the aura was (re)applied
// Skipped: passive auras (Last Ward's cheat death) and unlimited ones (a negative amount: Glacial Bulwark's ward).
// Other units (Emberfall feedback #48: the absorb segment on the client's health bars): every 200 ms (at most) a player
// is told the total absorbs left of the units it watches, the group's members on its map and its selection, on a
// change: `EFA\t1;0;UNI;<guid>;<total>`; `...;0` once a unit has none or is no longer watched (the same skips).
// The logic without the core is src/absorbs/EfAbsorbLogic.h (tests/absorb_tests.cpp).
//
// Config: Emberfall.Absorbs.Enable (1). GM command `.efabsorb`: the selected player's tracked values.

#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "Config.h"
#include "DataMap.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "WorldPacket.h"
#include "WorldScript.h"
#include "WorldSession.h"

#include "absorbs/EfAbsorbLogic.h"
#include "classkit/EfClassKit.h"

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    constexpr char const* kDataKey = "emberfall_absorbs";

    bool s_enable = true;

    // The per-player state rides on the player (Object::CustomData): only its map's thread touches it.
    struct PlayerAbsorbs : public DataMap::Base
    {
        Emberfall::Absorbs::State state;
        Emberfall::Absorbs::UnitState units;
    };

    // A unit's absorbs left in all (the same auras as Collect's: no passives, no unlimited ones).
    std::int64_t Total(Unit const* unit)
    {
        std::int64_t total = 0;
        for (AuraType type : { SPELL_AURA_SCHOOL_ABSORB, SPELL_AURA_MANA_SHIELD })
            for (AuraEffect const* eff : unit->GetAuraEffectsByType(type))
                if (eff->GetBase() && !eff->GetBase()->IsPassive() && eff->GetAmount() > 0)
                    total += eff->GetAmount();
        return total;
    }

    // The absorb auras on the player now, one snapshot per aura (not merged).
    void Collect(Player* player, std::vector<Emberfall::Absorbs::Snapshot>& out)
    {
        struct Seen
        {
            Aura const* aura;
            std::int64_t amount;
            bool unlimited;
        };
        std::vector<Seen> seen;
        for (AuraType type : { SPELL_AURA_SCHOOL_ABSORB, SPELL_AURA_MANA_SHIELD })
        {
            for (AuraEffect const* eff : player->GetAuraEffectsByType(type))
            {
                Aura const* aura = eff->GetBase();
                if (!aura || aura->IsPassive())
                    continue;
                auto it = std::find_if(seen.begin(), seen.end(), [&](Seen const& s) { return s.aura == aura; });
                if (it == seen.end())
                {
                    seen.push_back({ aura, 0, false });
                    it = seen.end() - 1;
                }
                if (eff->GetAmount() < 0)
                    it->unlimited = true;
                else
                    it->amount += eff->GetAmount();
            }
        }
        for (Seen const& s : seen)
        {
            if (s.unlimited)
                continue;
            Emberfall::Absorbs::Snapshot snap;
            snap.spellId = s.aura->GetId();
            snap.left = s.amount;
            if (Emberfall::ClassKit::IsShellSpell(snap.spellId))
                if (Unit* caster = s.aura->GetCaster())
                    snap.fixedCap = Emberfall::ClassKit::ShellCap(caster, snap.spellId);
            snap.applyTime = std::int64_t(s.aura->GetApplyTime());
            snap.instance = std::uint64_t(reinterpret_cast<std::uintptr_t>(s.aura));
            out.push_back(snap);
        }
    }

    void Send(Player* player, std::string const& text)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, text);
        player->SendDirectMessage(&data);
    }

    void Report(Player* player, uint32 diff)
    {
        bool const any = !player->GetAuraEffectsByType(SPELL_AURA_SCHOOL_ABSORB).empty()
            || !player->GetAuraEffectsByType(SPELL_AURA_MANA_SHIELD).empty();
        PlayerAbsorbs* data = any ? player->CustomData.GetDefault<PlayerAbsorbs>(kDataKey)
                                  : player->CustomData.Get<PlayerAbsorbs>(kDataKey);
        // no absorbs now and none reported: nothing to do
        if (!data || (!any && data->state.spells.empty()))
            return;
        if (!data->state.Tick(diff))
            return;
        std::vector<Emberfall::Absorbs::Snapshot> raw;
        if (any)
            Collect(player, raw);
        for (Emberfall::Absorbs::Line const& line : Emberfall::Absorbs::Diff(data->state, Emberfall::Absorbs::Merge(raw)))
            Send(player, Emberfall::Absorbs::Text(line));
    }

    // The other units the player watches: the group's members on its map (the same map: its thread) and its selection.
    void ReportUnits(Player* player, uint32 diff)
    {
        Group* group = player->GetGroup();
        ObjectGuid const selection = player->GetTarget();
        PlayerAbsorbs* data = (group || selection) ? player->CustomData.GetDefault<PlayerAbsorbs>(kDataKey)
                                                   : player->CustomData.Get<PlayerAbsorbs>(kDataKey);
        if (!data || (!group && !selection && data->units.told.empty()))
            return;
        if (!data->units.Tick(diff))
            return;
        std::map<std::uint64_t, std::int64_t> now;
        if (group)
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource(); member && member != player && member->IsInWorld()
                    && member->IsInMap(player) && member->IsAlive())
                    now[member->GetGUID().GetRawValue()] = Total(member);
        if (selection && selection != player->GetGUID())
            if (Unit* unit = ObjectAccessor::GetUnit(*player, selection); unit && unit->IsInWorld() && unit->IsAlive())
                now[selection.GetRawValue()] = Total(unit);
        for (Emberfall::Absorbs::UnitLine const& line : Emberfall::Absorbs::DiffUnits(data->units, now))
            Send(player, Emberfall::Absorbs::UnitText(line));
    }

    class emberfall_absorbs_world : public WorldScript
    {
    public:
        emberfall_absorbs_world()
            : WorldScript("emberfall_absorbs_world", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED }) { }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            s_enable = sConfigMgr->GetOption<bool>("Emberfall.Absorbs.Enable", true, false);
        }

        void OnBeforeWorldInitialized() override
        {
            if (s_enable)
                LOG_INFO("server.loading", ">> Emberfall absorbs: on (EFA, every {} ms)", Emberfall::Absorbs::kPeriodMs);
            else
                LOG_INFO("server.loading", ">> Emberfall absorbs: off (Emberfall.Absorbs.Enable = 0)");
        }
    };

    class emberfall_absorbs_player : public PlayerScript
    {
    public:
        emberfall_absorbs_player()
            : PlayerScript("emberfall_absorbs_player", { PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT }) { }

        void OnPlayerUpdate(Player* player, uint32 diff) override
        {
            if (s_enable && player && player->IsInWorld())
            {
                Report(player, diff);
                ReportUnits(player, diff);
            }
        }

        // A fresh start: the client knows nothing yet (the first update reports whatever absorb is present).
        void OnPlayerLogin(Player* player) override { player->CustomData.Erase(kDataKey); }
        void OnPlayerLogout(Player* player) override { player->CustomData.Erase(kDataKey); }
    };

    class emberfall_absorbs_commands : public CommandScript
    {
    public:
        emberfall_absorbs_commands() : CommandScript("emberfall_absorbs_commands") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable commandTable =
            {
                { "efabsorb", HandleEfAbsorb, SEC_GAMEMASTER, Console::No },
            };
            return commandTable;
        }

        // `EFABS <spellId> <left> <cap>` per tracked absorb of the selected player (or oneself), then `EFABS end`.
        static bool HandleEfAbsorb(ChatHandler* handler)
        {
            Player* player = handler->getSelectedPlayerOrSelf();
            if (!player)
                return false;
            if (PlayerAbsorbs const* data = player->CustomData.Get<PlayerAbsorbs>(kDataKey))
                for (auto const& [spellId, t] : data->state.spells)
                    handler->PSendSysMessage("EFABS {} {} {}", spellId, t.left, t.cap);
            handler->SendSysMessage("EFABS end");
            return true;
        }
    };
}

void AddEmberfallAbsorbsScripts()
{
    new emberfall_absorbs_world();
    new emberfall_absorbs_player();
    new emberfall_absorbs_commands();
}
