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
// Nathanael adopted it on 2026-10-23; ankarna's DEV handoff of 2026-11-15 and its revision of 2026-11-20): Heat, a spec
// power with tiers (since 2026-11-20 only at 100: Maximum, Supernova and Sunpiercer with their talents; EfSpecPower.cpp
// keeps the tier auras), and what data cannot do. Generic like the scripts of src/emberfall_classkit.cpp: each reads its numbers from the spell's own effects
// (Emberfall's classkit/model.py SCRIPTS checks the contracts):
//
//   spell_ef_kit_power_tiers     tier effects: the spell's dummy effects on the caster whose misc is an aura the caster
//                                carries when the cast goes off (a tier aura: Maximum; a talent passive: Solar Terror). With
//                                a trigger: the trigger is cast once, at the caster when it targets its caster (after the
//                                cast), else at the spell's target when the spell strikes it (Falling Sun's Solar Terror);
//                                misc B 1: instead of the spell, whose damage effects then deal nothing, cast at the spell's
//                                target as the cast goes off (since 2026-11-15: its damage and spell mods are set before the
//                                tier auras follow the cost; given the spell's missile speed it lands with it: Solar Flare's
//                                Supernova at Maximum). A trigger that a cast override of the caster's turns into another
//                                spell is cast as that one (Concentrated Supernova over Supernova). Without a trigger: the spell's
//                                damage is the value in percent higher, its direct hits and its persistent area aura's
//                                ticks alike, set when it is cast
//   spell_ef_kit_ramp_channel    a channel whose effect 1 is periodic damage on the target (Sunlance): effect 2 (dummy on
//                                the caster) value = each tick that many percent more than the last, misc = an aura the
//                                caster carries at the cast for the final tick to deal misc B percent more (Final
//                                Incineration); effect 3 (dummy on the caster) misc = an aura the caster carries at the cast
//                                (Sunpiercer's tier aura) for every tick to strike the enemies on the line behind the target
//                                too: the trigger (school damage) is cast at each with the tick's damage; value = the line's
//                                length in yards behind the target (half width 2.5 yd). At the last tick (the channel ran to
//                                its end) the caster's spell_ef_kit_rule_channel_extend rules naming the channel lengthen
//                                their trigger aura on the caster by their value in ms, no cap (Endless Inferno: Overheat)
//   spell_ef_kit_power_hold      Overheat: while the aura holds, the caster's spec power is never drained (its spenders are
//                                free by the aura's own spell mod: nothing consumes it); when it ends the power is set to
//                                the value of its first dummy aura effect (0). As it starts, a spell_ef_kit_rule_fill_power
//                                rule naming it fills the power to its maximum (Flash Ignition)
//   spell_ef_kit_snapshot_field  a ground field (effect 1 a persistent area aura with periodic damage: Burning Sigil): a
//                                dummy on the caster with a trigger (a self aura: the field's Heat) and misc (its second
//                                instance) casts the trigger on the caster at the cast as its own instance (the misc while
//                                the first still runs); the caster's dummy auras naming the spell are read as
//                                the field is placed and kept for its life: no trigger, their value in percent more on every
//                                tick (Inferno Sigil); a trigger, cast once at each enemy the field reaches, at most once
//                                per enemy per field (Burning Chains' root)
//   spell_ef_kit_spend_all       a spender that, once its Spell.dbc cost was paid, takes the rest of its power too (Falling
//                                Sun: needs 100 Heat, consumes all; free in Overheat, it takes nothing); what it takes counts
//                                for the spend rules naming it (Undying Flame, EfClassKit.h SpendCuts)
//   spell_ef_kit_rule_*          rule markers (EfClassKit.h Rules): _channel_extend and _fill_power (above), _power_floor
//                                (EfSpecPower.cpp: Eternal Ember's floor out of a fight: never cools at or below it)

#include "EfClassKit.h"
#include "EfClassKitLogic.h"
#include "EfHeatLogic.h"

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
    // A tier effect: a dummy on the caster whose misc names an aura (Maximum, Supernova's tier aura, a talent).
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
            if ((_active & (1u << i)) && trigger && !Emberfall::ClassKit::TargetsCaster(trigger)
                && GetSpellInfo()->Effects[i].MiscValueB != 1)
                caster->CastSpell(target, Overridden(caster, trigger), true);
        }
    }

    // A cast override of the caster's on the trigger (Concentrated Supernova over Supernova), or the trigger.
    static uint32 Overridden(Unit* caster, uint32 trigger)
    {
        Player* player = caster ? caster->ToPlayer() : nullptr;
        uint32 const instead = player ? Emberfall::ClassKit::CastOverride(player, trigger) : 0;
        return instead ? instead : trigger;
    }

    void AfterCastSelf()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            uint32 const trigger = GetSpellInfo()->Effects[i].TriggerSpell;
            if (!(_active & (1u << i)) || !trigger)
                continue;
            if (Emberfall::ClassKit::TargetsCaster(trigger))
                caster->CastSpell(caster, trigger, true);
            // instead of the spell (Supernova at Maximum): cast now, before the tier auras follow the cost (the handoff:
            // the threshold and its talents are read before the cost), at the spell's target; with the spell's missile
            // speed it lands as the spell would have
            else if (GetSpellInfo()->Effects[i].MiscValueB == 1)
                if (Unit* target = GetExplTargetUnit(); target && target != caster && target->IsAlive())
                    caster->CastSpell(target, Overridden(caster, trigger), true);
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

// Sunlance: six ticks, each 10 % more than the last; Final Incineration's final tick; with Sunpiercer at 100 every tick pierces.
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

    // The channel's last tick (it ran to its end): Endless Inferno lengthens Overheat while it holds, no cap.
    void LastTick(AuraEffect const* aurEff)
    {
        Unit* caster = GetCaster();
        if (!caster || !Emberfall::ClassKit::Heat::LastTick(aurEff->GetTickNumber(), aurEff->GetTotalTicks()))
            return;
        for (AuraEffect const* rule : Emberfall::ClassKit::Rules(caster, "spell_ef_kit_rule_channel_extend", GetId()))
        {
            uint32 const auraId = rule->GetSpellInfo()->Effects[rule->GetEffIndex()].TriggerSpell;
            Aura* held = auraId ? caster->GetAura(auraId, caster->GetGUID()) : nullptr;
            if (!held)
                continue;
            Emberfall::ClassKit::Heat::Lengthened const l =
                Emberfall::ClassKit::Heat::Lengthen(held->GetDuration(), held->GetMaxDuration(), rule->GetAmount());
            held->SetMaxDuration(l.maxMs);
            held->SetDuration(l.leftMs);
        }
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_ef_kit_ramp_channel::CalcAmount, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE);
        OnEffectUpdatePeriodic += AuraEffectUpdatePeriodicFn(spell_ef_kit_ramp_channel::UpdateTick, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_ef_kit_ramp_channel::Pierce, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_ef_kit_ramp_channel::LastTick, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE);
    }
};

// Overheat: Heat is never lost while it holds; when it ends Heat is set to its dummy effect's value (0).
class spell_ef_kit_power_hold : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_power_hold);

    void Apply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* owner = GetUnitOwner();
        Emberfall::ClassKit::HoldSpecPower(owner, true);
        // Flash Ignition: the power to its maximum as the hold starts
        if (!Emberfall::ClassKit::Rules(owner, "spell_ef_kit_rule_fill_power", GetId()).empty())
            Emberfall::ClassKit::FillSpecPower(owner);
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

// Burning Sigil: its Heat as its own instance, its bonuses set when it is placed.
class spell_ef_kit_snapshot_field : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_snapshot_field);

    void AfterCastSelf()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;
        for (SpellEffectInfo const& eff : GetSpellInfo()->Effects)
        {
            if (eff.Effect != SPELL_EFFECT_DUMMY || eff.TargetA.GetTarget() != TARGET_UNIT_CASTER || !eff.TriggerSpell || !eff.MiscValue)
                continue;
            SpellInfo const* first = sSpellMgr->GetSpellInfo(eff.TriggerSpell);
            if (!first || first->StackAmount > 1)   // a charge aura (spell_ef_kit_charges), not the field's own aura
                continue;
            uint32 const second = uint32(eff.MiscValue);
            Aura const* a = caster->GetAura(eff.TriggerSpell, caster->GetGUID());
            Aura const* b = caster->GetAura(second, caster->GetGUID());
            int32 const which = Emberfall::ClassKit::Heat::FieldInstance(a != nullptr, a ? a->GetDuration() : 0, b != nullptr,
                                                                         b ? b->GetDuration() : 0);
            caster->CastSpell(caster, which == 0 ? eff.TriggerSpell : second, true);
            return;
        }
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_ef_kit_snapshot_field::AfterCastSelf);
    }
};

// The field's aura (one per field: the dynamic object's): the caster's dummy auras naming the spell as it was placed.
class aura_ef_kit_snapshot_field : public AuraScript
{
    PrepareAuraScript(aura_ef_kit_snapshot_field);

    Emberfall::ClassKit::Heat::FieldSnapshot _snap;
    bool _taken = false;

    void Take()
    {
        if (_taken)
            return;
        _taken = true;
        Unit* caster = GetCaster();
        if (!caster)
            return;
        for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
        {
            if (uint32(eff->GetMiscValue()) != GetId() || eff->GetCasterGUID() != caster->GetGUID())
                continue;
            if (uint32 const trigger = eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell)
                _snap.triggers.push_back(trigger);
            else
                _snap.pct += eff->GetAmount();
        }
    }

    void CalcAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& canBeRecalculated)
    {
        canBeRecalculated = false;
        Take();
        if (_snap.pct > 0)
            amount = Emberfall::ClassKit::Logic::Empowered(amount, _snap.pct);
    }

    // every enemy the field reaches (an application of its aura): the triggers, once per enemy
    void Reach(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Take();
        Unit* caster = GetCaster();
        Unit* target = GetTarget();
        if (!caster || !target || target == caster || _snap.triggers.empty() || !target->IsAlive())
            return;
        if (!Emberfall::ClassKit::Heat::FirstReach(_snap, target->GetGUID().GetRawValue()))
            return;
        for (uint32 trigger : _snap.triggers)
            caster->CastSpell(target, trigger, true);
    }

    void Register() override
    {
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(m_scriptSpellId); info && info->HasAura(SPELL_AURA_PERIODIC_DAMAGE))
        {
            DoEffectCalcAmount += AuraEffectCalcAmountFn(aura_ef_kit_snapshot_field::CalcAmount, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE);
            AfterEffectApply += AuraEffectApplyFn(aura_ef_kit_snapshot_field::Reach, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE,
                                                  AURA_EFFECT_HANDLE_REAL);
        }
    }
};

// Falling Sun consumes all: once its cost was paid the rest of the bar goes too (nothing in Overheat, where it is free).
class spell_ef_kit_spend_all : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_spend_all);

    void Finish()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;
        Powers const slot = Powers(GetSpellInfo()->PowerType);
        if (caster->getPowerType() != slot)
            return;
        int32 const units = Emberfall::ClassKit::Heat::SpendAllExtra(GetSpell()->GetPowerCost(), int32(caster->GetPower(slot)));
        if (units <= 0)
            return;
        caster->ModifyPower(slot, -units);
        // what it took counts for Undying Flame like the cost (spell_ef_kit_spend_rules)
        int32 const scale = std::max(1, Emberfall::ClassKit::PowerScale(uint8(slot)));
        Emberfall::ClassKit::SpendCuts(caster, GetSpellInfo()->Id, units / scale);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_ef_kit_spend_all::Finish);
    }
};

// The rule markers of this file: nothing happens in them (EfClassKit.h Rules finds the passives by these names).
class spell_ef_kit_heat_rule : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_heat_rule);

    void Register() override { }
};

void AddEmberfallClassKitHeatScripts()
{
    RegisterSpellAndAuraScriptPair(spell_ef_kit_power_tiers, aura_ef_kit_power_tiers);
    RegisterSpellScript(spell_ef_kit_ramp_channel);
    RegisterSpellScript(spell_ef_kit_power_hold);
    RegisterSpellAndAuraScriptPair(spell_ef_kit_snapshot_field, aura_ef_kit_snapshot_field);
    RegisterSpellScript(spell_ef_kit_spend_all);
    for (char const* name : { "spell_ef_kit_rule_channel_extend", "spell_ef_kit_rule_fill_power", "spell_ef_kit_rule_power_floor" })
        RegisterSpellScriptWithArgs(spell_ef_kit_heat_rule, name);
}
