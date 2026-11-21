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

// Emberfall's rides carry a group (feedback #693; Emberfall tools/py/emberfall_content/quests/rides.py, the rule in
// rides/EfRideLogic.h). A ride's cart is a summon of its clicker, its SmartAI drives it; this file is the part SmartAI
// cannot do, on the ride spell (the control-vehicle aura every rider holds on the cart; Emberfall spells/quests.yaml
// 90375, `script: spell_ef_ride_seat`, spell_script_names):
//   - the clicker sits down: on the cart's next update the members of their group the rule takes along cast the same
//     spell into the cart's free rider seats (its amount, seat + 1, set per cast), nearest first;
//   - a player gets down (the leave button, a logout, a death, the end): when no player is left on the cart it hears
//     kEmptyData (its SmartAI's data_set: on the way, the driver's `early` line and the cart goes; at the end nothing).
// Which quest a cart is the ride of: the quest whose kill credit is the cart's entry (the ride's objective,
// rides.py expand_quest). Any data: the script is bound only where a world names it.

#include "CreatureAI.h"
#include "EmberfallOwnData.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "TemporarySummon.h"
#include "Vehicle.h"
#include "rides/EfRideLogic.h"

#include <vector>

namespace
{
    using namespace Emberfall::Ride;

    // The ride quest of a cart: the quest whose kill credit is its entry, and that objective's index; 0 when none.
    std::pair<uint32, uint8> RideOf(uint32 cartEntry)
    {
        for (auto const& [id, quest] : sObjectMgr->GetQuestTemplates())
            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                if (quest->RequiredNpcOrGo[i] > 0 && uint32(quest->RequiredNpcOrGo[i]) == cartEntry)
                    return { id, i };
        return { 0, 0 };
    }

    bool HasRideToDo(Player* player, uint32 questId, uint8 objective)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            return false;
        uint32 credit = 0;
        auto it = player->getQuestStatusMap().find(questId);
        if (it != player->getQuestStatusMap().end())
            credit = it->second.CreatureOrGOCount[objective];
        return RideToDo(player->GetQuestStatus(questId) == QUEST_STATUS_INCOMPLETE, credit,
                        quest->RequiredNpcOrGoCount[objective]);
    }

    uint32 PlayersOn(Vehicle const* kit)
    {
        uint32 n = 0;
        for (auto const& [index, seat] : kit->Seats)
            if (seat.Passenger.Guid.IsPlayer())
                ++n;
        return n;
    }

    // The clicker sat down: the members of their group the rule takes along sit down beside them.
    void SeatGroup(Creature* cart, ObjectGuid clickerGuid, uint32 spellId)
    {
        Vehicle* kit = cart->GetVehicleKit();
        Player* clicker = ObjectAccessor::GetPlayer(*cart, clickerGuid);
        if (!cart->IsInWorld() || !kit || !clicker || clicker->GetVehicleBase() != cart)
            return;
        Group* group = clicker->GetGroup();
        if (!group)
            return;
        auto [questId, objective] = RideOf(cart->GetEntry());
        if (!questId)
            return;
        std::vector<int8> free;
        for (auto const& [index, seat] : kit->Seats)
            if (seat.IsEmpty() && seat.SeatInfo && seat.SeatInfo->CanEnterOrExit())
                free.push_back(index);
        std::vector<Player*> players;
        std::vector<Member> members;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* p = ref->GetSource();
            if (!p || p == clicker)
                continue;
            Member m;
            m.sameMap = p->IsInWorld() && p->IsInMap(cart) && p->InSamePhase(cart);
            if (m.sameMap)
            {
                m.distance = p->GetDistance(cart);
                m.alive = p->IsAlive();
                m.inCombat = p->IsInCombat();
                m.onVehicle = p->GetVehicle() != nullptr;
                m.inFlight = p->IsInFlight();
                m.rideToDo = HasRideToDo(p, questId, objective);
            }
            members.push_back(m);
            players.push_back(p);
        }
        for (auto const& [i, seat] : Assign(members, free))
        {
            players[i]->CastCustomSpell(spellId, SPELLVALUE_BASE_POINT0, seat + 1, cart, true);
            LOG_DEBUG("module", "Emberfall rides: {} rides with {} (quest {}, seat {})", players[i]->GetName(),
                      clicker->GetName(), questId, int32(seat));
        }
    }
}

class spell_ef_ride_seat : public AuraScript
{
    PrepareAuraScript(spell_ef_ride_seat);

    void AfterApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (!Emberfall::OwnData())
            return;
        Unit* target = GetTarget();
        Unit* caster = GetCaster();
        Creature* cart = target ? target->ToCreature() : nullptr;
        if (!cart || !caster || !cart->GetVehicleKit() || caster->GetVehicleBase() != cart)
            return;
        TempSummon const* summon = cart->ToTempSummon();
        if (!Emberfall::Ride::BringsGroup(caster->IsPlayer(), summon && summon->GetSummonerGUID() == caster->GetGUID()))
            return;
        if (!caster->ToPlayer()->GetGroup())
            return;
        // on the cart's next update, not inside the clicker's own sitting down (its SmartAI's boarding rows first)
        ObjectGuid const clicker = caster->GetGUID();
        uint32 const spellId = GetId();
        cart->m_Events.AddEventAtOffset([cart, clicker, spellId]() { SeatGroup(cart, clicker, spellId); },
                                        Milliseconds(0));
    }

    void AfterRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (!Emberfall::OwnData() || !GetCasterGUID().IsPlayer())
            return;
        Creature* cart = GetTarget() ? GetTarget()->ToCreature() : nullptr;
        if (!cart || !cart->IsInWorld() || cart->IsDuringRemoveFromWorld() || !cart->GetVehicleKit() || !cart->IsAIEnabled)
            return;
        if (Emberfall::Ride::CartEmpty(PlayersOn(cart->GetVehicleKit())))
            cart->AI()->SetData(Emberfall::Ride::kEmptyDataField, Emberfall::Ride::kEmptyDataValue);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_ef_ride_seat::AfterApply, EFFECT_0, SPELL_AURA_CONTROL_VEHICLE,
                                              AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_ef_ride_seat::AfterRemove, EFFECT_0, SPELL_AURA_CONTROL_VEHICLE,
                                                AURA_EFFECT_HANDLE_REAL);
    }
};

void AddEmberfallRideScripts()
{
    RegisterSpellScript(spell_ef_ride_seat);
}
