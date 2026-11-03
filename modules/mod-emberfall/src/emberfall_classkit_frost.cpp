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

// The Mage Frostguard's rework (ankarna's handoff of 2026-11-03, Emberfall docs/design/classes/mage-frostguard.md §12):
// the two mechanics no shared script had. Contracts (Emberfall's classkit/model.py SCRIPTS checks the effects):
//
//   spell_ef_kit_marked_absorb        a passive: effect 1 a school absorb (misc: the schools) whose value is the percent
//                                     of a blow it takes, only from an attacker that carries effect 2's trigger aura of
//                                     the owner's (Frozen Weakness: Glacial Spear's mark, 10 % less physical damage from
//                                     the marked enemy to the mage alone); effect 2's dummy is also the hit bonus that lays
//                                     the mark (misc: the spell)
//   spell_ef_kit_proc_attacker_stacks a proc_trigger_spell aura on the blows taken: while the owner holds the aura of its
//                                     misc (the shell), the attacker gains value stacks of its trigger (EfClassKit.h
//                                     AddStacks); the chance and the internal cooldown are the spell_proc row's (Living
//                                     Glacier: 20 %, once a second for every attacker together)

#include "EfClassKit.h"

#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "Unit.h"

#include <algorithm>

// Takes value % of a blow of its schools, only from an attacker the owner has marked.
class spell_ef_kit_marked_absorb : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_marked_absorb);

    void CalcAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        amount = -1; // unlimited: Absorb decides
    }

    void Absorb(AuraEffect* /*aurEff*/, DamageInfo& dmgInfo, uint32& absorbAmount)
    {
        absorbAmount = 0;
        Unit* owner = GetUnitOwner();
        Unit* attacker = dmgInfo.GetAttacker();
        uint32 const mark = GetSpellInfo()->Effects[EFFECT_1].TriggerSpell;
        if (!owner || !attacker || attacker == owner || !mark || !attacker->HasAura(mark, owner->GetGUID()))
            return;
        int32 const pct = std::clamp(GetSpellInfo()->Effects[EFFECT_0].CalcValue(owner), 0, 100);
        absorbAmount = uint32(uint64(dmgInfo.GetDamage()) * uint64(pct) / 100);
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_ef_kit_marked_absorb::CalcAmount, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
        OnEffectAbsorb += AuraEffectAbsorbFn(spell_ef_kit_marked_absorb::Absorb, EFFECT_0);
    }
};

// A blow taken while the owner holds the misc aura: the attacker gains stacks.
class spell_ef_kit_proc_attacker_stacks : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_proc_attacker_stacks);

    bool CheckProc(ProcEventInfo& info)
    {
        Unit* owner = GetUnitOwner();
        Unit* attacker = info.GetActor();
        int32 const need = GetSpellInfo()->Effects[EFFECT_0].MiscValue;
        if (!owner || !attacker || attacker == owner || !attacker->IsAlive() || !owner->IsValidAttackTarget(attacker))
            return false;
        return !need || owner->HasAura(uint32(need), owner->GetGUID());
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& info)
    {
        PreventDefaultAction();
        Unit* owner = GetUnitOwner();
        Unit* attacker = info.GetActor();
        uint32 const trigger = GetSpellInfo()->Effects[EFFECT_0].TriggerSpell;
        if (!owner || !attacker || !trigger)
            return;
        Emberfall::ClassKit::AddStacks(owner, attacker, trigger, std::max(1, aurEff->GetAmount()));
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_ef_kit_proc_attacker_stacks::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_ef_kit_proc_attacker_stacks::HandleProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

void AddEmberfallClassKitFrostScripts()
{
    RegisterSpellScript(spell_ef_kit_marked_absorb);
    RegisterSpellScript(spell_ef_kit_proc_attacker_stacks);
}
