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

// Emberfall class kits, the Mage's Pyromancer (Emberfall docs/design/classes/mage-pyromancer.md, ankarna's card sheet as
// Nathanael adopted it on 2026-10-23): Heat, a spec power with tiers (Blazing at 30, Inferno at 70, Maximum at 100;
// EfSpecPower.cpp keeps the tier auras, whose spell mods carry most of the tier bonuses as data), and what data cannot
// do. Generic like the scripts of src/emberfall_classkit.cpp: each reads its numbers from the spell's own effects
// (Emberfall's classkit/model.py SCRIPTS checks the contracts):
//
//   spell_ef_kit_power_tiers     tier effects: the spell's dummy effects on the caster whose misc is an aura the caster
//                                carries when the cast goes off (a tier aura: Blazing, Inferno, Maximum). With a trigger:
//                                the trigger is cast once, at the caster when it targets its caster (after the cast:
//                                Burning Sigil's Heat at Blazing), else at the spell's target when the spell strikes it
//                                (Solar Flare's Supernova at Maximum); misc B 1: instead of the spell, whose damage effects
//                                then deal nothing. Without a trigger: the spell's damage is the value in percent higher,
//                                its direct hits and its persistent area aura's ticks alike (Burning Sigil at Inferno: set
//                                when it is cast, kept for its duration)
//   spell_ef_kit_ramp_channel    a channel whose effect 1 is periodic damage on the target (Sunlance): effect 2 (dummy on
//                                the caster) value = each tick that many percent more than the last, misc = an aura the
//                                caster carries at the cast for the final tick to deal misc B percent more (Blazing);
//                                effect 3 (dummy on the caster) misc = an aura the caster carries at the cast (Maximum) for
//                                every tick to strike the enemies on the line behind the target too: the trigger (school
//                                damage) is cast at each with the tick's damage; value = the line's length in yards
//                                behind the target (half width 2.5 yd)
//   spell_ef_kit_power_hold      Overheat: while the aura holds, the caster's spec power is never drained (its spenders are
//                                free by the aura's own spell mod: nothing consumes it); when it ends the power is set to
//                                the value of its first dummy aura effect (0)

#include "EfClassKit.h"
#include "EfClassKitLogic.h"

#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"

#include <list>

namespace
{
    // A tier effect: a dummy on the caster whose misc names an aura (Blazing, Inferno, Maximum).
    bool IsTierEffect(SpellEffectInfo const& eff)
    {
        return eff.Effect == SPELL_EFFECT_DUMMY && eff.TargetA.GetTarget() == TARGET_UNIT_CASTER && eff.MiscValue > 0;
    }

    // The tier effects of `info` whose aura the caster carries now, as a mask of effect indices.
    uint8 ActiveTiers(Unit const* caster, SpellInfo const* info)
    {
        uint8 mask = 0;
        if (!caster)
            return 0;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (IsTierEffect(info->Effects[i]) && caster->HasAura(uint32(info->Effects[i].MiscValue)))
                mask |= uint8(1u << i);
        return mask;
    }

    // The damage bonus in percent of the active tier effects without a trigger.
    int32 TierBonusPct(Unit* caster, SpellInfo const* info, uint8 active)
    {
        int32 pct = 0;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if ((active & (1u << i)) && !info->Effects[i].TriggerSpell)
                pct += info->Effects[i].CalcValue(caster);
        return pct;
    }
}

class spell_ef_kit_power_tiers : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_power_tiers);

    uint8 _active = 0;
    bool _instead = false;
    int32 _bonusPct = 0;
    bool _targetDone = false;

    void AtCast()
    {
        Unit* caster = GetCaster();
        _active = ActiveTiers(caster, GetSpellInfo());
        _bonusPct = TierBonusPct(caster, GetSpellInfo(), _active);
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if ((_active & (1u << i)) && GetSpellInfo()->Effects[i].TriggerSpell && GetSpellInfo()->Effects[i].MiscValueB == 1
                && !Emberfall::ClassKit::TargetsCaster(GetSpellInfo()->Effects[i].TriggerSpell))
                _instead = true;
    }

    // Supernova instead of Solar Flare: the flare's own damage is never computed (AzerothCore computes school damage at
    // the launch).
    void PreventDamage(SpellEffIndex effIndex)
    {
        if (_instead)
            PreventHitDefaultEffect(effIndex);
    }

    void HandleHit()
    {
        Unit* target = GetHitUnit();
        if (!target || target == GetCaster() || _bonusPct <= 0 || GetHitDamage() <= 0)
            return;
        SetHitDamage(Emberfall::ClassKit::Logic::Empowered(GetHitDamage(), _bonusPct));
    }

    void AfterHitTarget()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (_targetDone || !caster || !target || target == caster || target != GetExplTargetUnit())
            return;
        _targetDone = true;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            uint32 const trigger = GetSpellInfo()->Effects[i].TriggerSpell;
            if ((_active & (1u << i)) && trigger && !Emberfall::ClassKit::TargetsCaster(trigger))
                caster->CastSpell(target, trigger, true);
        }
    }

    void AfterCastSelf()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            uint32 const trigger = GetSpellInfo()->Effects[i].TriggerSpell;
            if ((_active & (1u << i)) && trigger && Emberfall::ClassKit::TargetsCaster(trigger))
                caster->CastSpell(caster, trigger, true);
        }
    }

    void Register() override
    {
        OnCast += SpellCastFn(spell_ef_kit_power_tiers::AtCast);
        // only on a spell with school damage (Solar Flare; Burning Sigil has none): the core warns of a hook without its effect
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(m_scriptSpellId); info && info->HasEffect(SPELL_EFFECT_SCHOOL_DAMAGE))
            OnEffectLaunchTarget += SpellEffectFn(spell_ef_kit_power_tiers::PreventDamage, EFFECT_ALL, SPELL_EFFECT_SCHOOL_DAMAGE);
        OnHit += SpellHitFn(spell_ef_kit_power_tiers::HandleHit);
        AfterHit += SpellHitFn(spell_ef_kit_power_tiers::AfterHitTarget);
        AfterCast += SpellCastFn(spell_ef_kit_power_tiers::AfterCastSelf);
    }
};

// The persistent area aura's ticks (Burning Sigil): the tier bonus the caster had when it was cast. The dynamic
// object's aura computes its amount once, at the cast.
class aura_ef_kit_power_tiers : public AuraScript
{
    PrepareAuraScript(aura_ef_kit_power_tiers);

    void CalcAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& canBeRecalculated)
    {
        canBeRecalculated = false;
        Unit* caster = GetCaster();
        int32 const pct = TierBonusPct(caster, GetSpellInfo(), ActiveTiers(caster, GetSpellInfo()));
        if (pct > 0)
            amount = Emberfall::ClassKit::Logic::Empowered(amount, pct);
    }

    void Register() override
    {
        // only on a spell with a periodic damage aura (Burning Sigil's ground; Solar Flare has none)
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(m_scriptSpellId); info && info->HasAura(SPELL_AURA_PERIODIC_DAMAGE))
            DoEffectCalcAmount += AuraEffectCalcAmountFn(aura_ef_kit_power_tiers::CalcAmount, EFFECT_ALL, SPELL_AURA_PERIODIC_DAMAGE);
    }
};

// Sunlance: six ticks, each 10 % more than the last; Blazing's final tick +25 %; at Maximum every tick pierces.
class spell_ef_kit_ramp_channel : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_ramp_channel);

    int32 _base = 0;
    bool _final = false;
    bool _pierce = false;

    void CalcAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& canBeRecalculated)
    {
        canBeRecalculated = false;
        _base = amount;
        Unit* caster = GetCaster();
        if (!caster)
            return;
        SpellEffectInfo const& ramp = GetSpellInfo()->Effects[EFFECT_1];
        SpellEffectInfo const& pierce = GetSpellInfo()->Effects[EFFECT_2];
        _final = ramp.MiscValue > 0 && caster->HasAura(uint32(ramp.MiscValue));
        _pierce = pierce.MiscValue > 0 && pierce.TriggerSpell && caster->HasAura(uint32(pierce.MiscValue));
    }

    // before every tick (AuraEffect::UpdatePeriodic): this tick's amount
    void UpdateTick(AuraEffect* aurEff)
    {
        Unit* caster = GetCaster();
        SpellEffectInfo const& ramp = GetSpellInfo()->Effects[EFFECT_1];
        uint32 const tick = aurEff->GetTickNumber();
        bool const last = tick >= uint32(aurEff->GetTotalTicks());
        aurEff->SetAmount(Emberfall::ClassKit::Logic::RampTick(_base, caster ? ramp.CalcValue(caster) : 0, tick, last && _final,
                                                               ramp.MiscValueB));
    }

    void Pierce(AuraEffect const* aurEff)
    {
        Unit* caster = GetCaster();
        Unit* target = GetUnitOwner();
        if (!_pierce || !caster || !target)
            return;
        SpellEffectInfo const& pierce = GetSpellInfo()->Effects[EFFECT_2];
        float const length = float(std::max(1, pierce.CalcValue(caster)));
        float constexpr halfWidth = 2.5f;
        std::list<Unit*> near;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(target, caster, length + halfWidth);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(target, near, check);
        Cell::VisitObjects(target, searcher, length + halfWidth);
        int32 const amount = aurEff->GetAmount();
        for (Unit* unit : near)
        {
            if (unit == target || !unit->IsAlive() || !caster->IsValidAttackTarget(unit)
                || !Emberfall::ClassKit::Logic::BehindOnLine(caster->GetPositionX(), caster->GetPositionY(), target->GetPositionX(),
                                                             target->GetPositionY(), unit->GetPositionX(), unit->GetPositionY(),
                                                             length, halfWidth, unit->GetCombatReach()))
                continue;
            caster->CastCustomSpell(unit, pierce.TriggerSpell, &amount, nullptr, nullptr, true);
        }
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_ef_kit_ramp_channel::CalcAmount, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE);
        OnEffectUpdatePeriodic += AuraEffectUpdatePeriodicFn(spell_ef_kit_ramp_channel::UpdateTick, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_ef_kit_ramp_channel::Pierce, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE);
    }
};

// Overheat: Heat is never lost while it holds; when it ends Heat is set to its dummy effect's value (0).
class spell_ef_kit_power_hold : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_power_hold);

    void Apply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Emberfall::ClassKit::HoldSpecPower(GetUnitOwner(), true);
    }

    void Remove(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
    {
        Unit* owner = GetUnitOwner();
        Emberfall::ClassKit::HoldSpecPower(owner, false);
        Emberfall::ClassKit::SetSpecPowerPoints(owner, aurEff->GetAmount());
    }

    void Register() override
    {
        uint8 index = EFFECT_0;
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(m_scriptSpellId))
            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                if (info->Effects[i].ApplyAuraName == SPELL_AURA_DUMMY)
                {
                    index = i;
                    break;
                }
        AfterEffectApply += AuraEffectApplyFn(spell_ef_kit_power_hold::Apply, index, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_ef_kit_power_hold::Remove, index, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

void AddEmberfallClassKitHeatScripts()
{
    RegisterSpellAndAuraScriptPair(spell_ef_kit_power_tiers, aura_ef_kit_power_tiers);
    RegisterSpellScript(spell_ef_kit_ramp_channel);
    RegisterSpellScript(spell_ef_kit_power_hold);
}
