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

// Emberfall class kits, the Warlock's (Emberfall docs/design/classes/warlock-harvester.md): the Harvester heals with
// what it reaps from its enemies and with its own blood. Generic like the scripts of src/emberfall_classkit.cpp: each
// reads its numbers from the spell's own effects (Emberfall's classkit/model.py SCRIPTS checks the contracts):
//
//   spell_ef_kit_health_price     effect 2 (dummy on the caster): its value (spell mods in: Thinned Blood) in percent of
//                                 the caster's max health is paid when the cast goes off; never on oneself, refused
//                                 unless the caster keeps at least 1 health (Blood Price)
//   spell_ef_kit_transfuse        proc_trigger_spell auras: a share of the owner's damage (the effect's value in percent;
//                                 misc = the spell whose damage counts, none: any) is cast as the effect's trigger (a heal
//                                 of the amount it is cast with) on the most injured ally within 40 yards, the owner
//                                 included (EfClassKit.h MostInjuredAlly: group, pets and guardians, the .efparty
//                                 companions; Overflow, Harvest Tide)

#include "EfClassKit.h"
#include "EfClassKitLogic.h"

#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "Unit.h"

namespace
{
    constexpr float kTransfuseRange = 40.0f;
}

class spell_ef_kit_health_price : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_health_price);

    int32 Price()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return 0;
        int32 const pct = GetSpellInfo()->Effects[EFFECT_1].CalcValue(caster);
        return Emberfall::ClassKit::Logic::HealthPrice(caster->GetMaxHealth(), pct);
    }

    SpellCastResult Check()
    {
        Unit* caster = GetCaster();
        Unit* target = GetExplTargetUnit();
        if (!caster || !target || target == caster)
            return SPELL_FAILED_BAD_TARGETS;
        if (!Emberfall::ClassKit::Logic::CanPay(caster->GetHealth(), Price()))
            return SPELL_FAILED_FIZZLE;
        return SPELL_CAST_OK;
    }

    void Pay()
    {
        Unit* caster = GetCaster();
        int32 const price = Price();
        if (!caster || price <= 0)
            return;
        // never the last point (the check refuses a price the caster cannot pay; health may have dropped since)
        int32 const paid = std::min<int32>(price, int32(caster->GetHealth()) - 1);
        if (paid > 0)
            caster->ModifyHealth(-paid);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_ef_kit_health_price::Check);
        OnCast += SpellCastFn(spell_ef_kit_health_price::Pay);
    }
};

class spell_ef_kit_transfuse : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_transfuse);

    bool CheckProc(ProcEventInfo& info)
    {
        DamageInfo* damage = info.GetDamageInfo();
        return damage && damage->GetDamage() > 0 && GetUnitOwner() && info.GetActor() == GetUnitOwner();
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& info)
    {
        PreventDefaultAction();
        Unit* owner = GetUnitOwner();
        DamageInfo* damage = info.GetDamageInfo();
        uint32 const trigger = GetSpellInfo()->Effects[aurEff->GetEffIndex()].TriggerSpell;
        if (!owner || !damage || !trigger)
            return;
        int32 const only = aurEff->GetMiscValue();
        SpellInfo const* by = damage->GetSpellInfo();
        if (only && (!by || by->Id != uint32(only)))
            return;
        int32 amount = Emberfall::ClassKit::Logic::Transfused(int32(damage->GetDamage()), aurEff->GetAmount());
        if (amount <= 0)
            return;
        Unit* ally = Emberfall::ClassKit::MostInjuredAlly(owner, kTransfuseRange);
        if (!ally)
            return;
        owner->CastCustomSpell(ally, trigger, &amount, nullptr, nullptr, true, nullptr, aurEff);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_ef_kit_transfuse::CheckProc);
        // one hook per proc effect the spell has (Overflow three, Harvest Tide one): a hook on an effect the spell lacks
        // is a load error
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(m_scriptSpellId))
            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                if (info->Effects[i].ApplyAuraName == SPELL_AURA_PROC_TRIGGER_SPELL)
                    OnEffectProc += AuraEffectProcFn(spell_ef_kit_transfuse::HandleProc, i, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

void AddEmberfallClassKitHealScripts()
{
    RegisterSpellScript(spell_ef_kit_health_price);
    RegisterSpellScript(spell_ef_kit_transfuse);
}
