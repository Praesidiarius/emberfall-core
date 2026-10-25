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

// Emberfall class kits: the Paladin's Sunwarden (Emberfall docs/design/classes/paladin-sunwarden.md, ankarna's and
// sonaetiger's developer handoff v1.0). Generic scripts, their numbers in the spells' own effects (Emberfall's
// classkit/scripts_sun.py checks the contracts); the arithmetic is classkit/EfClassKitSunLogic.h
// (tests/classkit_sun_tests.cpp).
//
//   spell_ef_kit_restore_charge  a dummy on the caster with misc = a charged spell (spell_ef_kit_charges), trigger = its
//                                charge aura, value = the charges restored (0: all, the running recharge stopped): the
//                                charges come back at once, a running recharge runs on (EfClassKit.h RestoreCharges;
//                                at full charges nothing). misc B 1: only once, when the spell first strikes an enemy
//                                (Solar Reckoning's shockwave), else when the dummy lands on the caster (Solenne's
//                                Salvation, Dawn's Renewal's charge). After the cast the caster's hit bonuses for the
//                                spell on the caster (a self trigger: Solenne's Favor after Salvation)
//   spell_ef_kit_heal_circle     an aura script on a persistent_area_aura (a periodic dummy on allies): its allies are
//                                the caster, the caster's group (a raid: the whole raid) and what they control (pets,
//                                guardians: the .efparty companions); each tick the trigger of the spell's dummy effect
//                                whose trigger heals (the tick heal) is cast at every one of them, in a raid at most the
//                                effect's value, the most injured first (Q5: 6); after the last tick its misc (the final
//                                burst) at the same ones (Solenne's Embrace)
//   spell_ef_kit_heal_riders     on a heal (effect 1): a dummy beside it with a trigger that is a school absorb turns
//                                its value in percent of the overheal into that shield (Embrace's tick and burst: 75 %,
//                                Overflowing Light by its effect 2 mod); the caster's dummy auras then add:
//                                  misc = this heal, no trigger, misc B > 0: value % more on a target below misc B %
//                                    health (Guiding Light)
//                                  misc = this heal, a trigger: a rider, value % of the heal (crit included) cast as the
//                                    trigger's base points, spread over its ticks when it is periodic (Lingering
//                                    Radiance); misc B bit 1: crits only, bit 2: at the most injured other group member
//                                    within the trigger's range instead of the target (Shared Radiance); a trigger that
//                                    is a charge aura: -value ms off its next charge (Boundless Mercy)
//                                  misc = a stacking aura, a class mask covering this heal, a trigger: a counter, each
//                                    heal one stack, at value stacks (spell mods in: Renewed Devotion) they go and the
//                                    trigger is cast on the caster (Dawn's Renewal)
//   spell_ef_kit_pooled_absorb   a school absorb (effect 1) that adds up: each application adds its amount to what is
//                                left, at most effect 2's value in % of the owner's max health, and refreshes the
//                                duration. Effect 2's misc 1: effect 1's value is a % of the owner's max health per
//                                application (Dawnbreak's shields, Sheltering Dawn by its effect 1 mod); misc 0: the
//                                amount the shield is cast with (Embrace's overheal)
//   spell_ef_kit_combo_button    a combo on one button (Dawnstrike; its later steps carry spell_ef_kit_combo_step):
//                                a dummy on the caster with misc = the combo aura
//                                (a fight marker whose dummy effects' triggers are steps 2, 3, ...) and value = its
//                                stacks after this spell hits (0: the first step again; a trigger on that dummy is cast
//                                on the caster then: Dawnbreak's shields). The button (no combo aura stacks: step 1)
//                                strikes itself; at N stacks its own strike leaves the target and it casts step N+1 at
//                                it instead. Only a hit moves the combo on (a miss, dodge or parry does not, Q7); a dummy
//                                at the enemy with a trigger casts that at every enemy struck (Radiant Blow's holy part)
//   spell_ef_kit_mana_on_hit     a dummy on the caster: value = % of max mana back when the spell strikes an enemy
//                                (into an absorb too; an immune enemy is no hit), misc B 1: double on a crit, misc 1: % of
//                                base mana instead of max (Sunbreaker since playtest #67; Restoring Strike by its effect 2
//                                mod)

#include "EfClassKit.h"
#include "EfClassKitSunLogic.h"

#include "Group.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "Unit.h"

#include <algorithm>
#include <vector>

namespace SL = Emberfall::ClassKit::SunLogic;

namespace
{
    // Whether `info` is a charge aura (spell_ef_kit_charge_aura): a stacking dummy aura whose effect 1 misc names the
    // charged spell. Returns that spell or 0.
    uint32 ChargedSpellOf(SpellInfo const* info)
    {
        if (!info || info->StackAmount < 2 || info->Effects[EFFECT_0].ApplyAuraName != SPELL_AURA_DUMMY)
            return 0;
        int32 const misc = info->Effects[EFFECT_0].MiscValue;
        return misc > 0 && sSpellMgr->GetSpellInfo(uint32(misc)) ? uint32(misc) : 0;
    }

    // The ally rule of the Sunwarden's group heals: the caster, its group (a raid: the whole raid) and what any of them
    // controls (pets, guardians: the .efparty companions), alive and friendly.
    bool GroupAlly(Unit* caster, Unit* unit)
    {
        if (!caster || !unit || !unit->IsAlive() || !unit->IsInWorld() || unit->GetMap() != caster->GetMap())
            return false;
        if (unit == caster)
            return true;
        if (!caster->IsFriendlyTo(unit))
            return false;
        Player* me = caster->GetCharmerOrOwnerPlayerOrPlayerItself();
        Player* them = unit->GetCharmerOrOwnerPlayerOrPlayerItself();
        if (!me || !them)
            return unit->GetCharmerOrOwnerOrSelf() == caster;
        return me == them || me->IsInSameRaidWith(them);
    }

    // The most injured group ally of `caster` within `range` of it, `other` left out (Shared Radiance); nullptr when every
    // one is full.
    Unit* MostInjuredOther(Unit* caster, Unit* other, float range)
    {
        std::vector<Unit*> allies;
        auto consider = [&](Unit* unit)
        {
            if (!unit || unit == other || std::find(allies.begin(), allies.end(), unit) != allies.end())
                return;
            if (!GroupAlly(caster, unit) || unit->IsFullHealth() || (unit != caster && !caster->IsWithinDistInMap(unit, range)))
                return;
            allies.push_back(unit);
        };
        auto withControlled = [&](Unit* unit)
        {
            consider(unit);
            for (Unit* controlled : unit->m_Controlled)
                consider(controlled);
        };
        withControlled(caster);
        if (Player* player = caster->ToPlayer())
            if (Group* group = player->GetGroup())
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                    if (Player* member = ref->GetSource(); member && member != player)
                        withControlled(member);
        Unit* best = nullptr;
        for (Unit* unit : allies)
            if (!best || unit->GetHealthPct() < best->GetHealthPct())
                best = unit;
        return best;
    }

    // Whether the spell's hit on `target` was a critical one (the target info the launch rolled).
    bool HitCrit(Spell* spell, Unit* target)
    {
        if (!spell || !target)
            return false;
        for (TargetInfo const& info : *spell->GetUniqueTargetInfo())
            if (info.targetGUID == target->GetGUID())
                return info.crit;
        return false;
    }

    bool Hostile(Unit* caster, Unit* target)
    {
        return caster && target && target != caster && !caster->IsFriendlyTo(target);
    }
}

namespace Emberfall::ClassKit
{
    uint32 RestoreCharges(Player* player, uint32 spell, uint32 chargeAura, uint32 count)
    {
        if (!player || !spell || !chargeAura)
            return 0;
        Aura* aura = player->GetAura(chargeAura, player->GetGUID());
        if (!aura)
            return 0;   // every charge at hand: the restore is lost
        uint32 const recharging = aura->GetStackAmount();
        uint32 const after = SL::RechargingAfterRestore(recharging, count);
        if (!after)
            aura->Remove(AURA_REMOVE_BY_CANCEL);   // not an expiry: no charge back on top (ChargeBack)
        else
            aura->SetStackAmount(uint8(after));    // the running recharge runs on
        // a charge is at hand now: the spell can be cast again
        player->RemoveSpellCooldown(spell, true);
        return recharging - after;
    }

    void ShortenCharge(Player* player, uint32 spell, uint32 chargeAura, int32 ms)
    {
        if (!player || !spell || !chargeAura || ms <= 0)
            return;
        Aura* aura = player->GetAura(chargeAura, player->GetGUID());
        if (!aura)
            return;
        int32 const left = aura->GetDuration() - ms;
        // no charge at hand: the spell's cooldown is the next charge's time, it moves with it
        bool const waiting = player->HasSpellCooldown(spell);
        if (left <= 0)
        {
            if (waiting)
                player->RemoveSpellCooldown(spell, true);
            aura->Remove(AURA_REMOVE_BY_EXPIRE);   // the charge is back (ChargeBack starts the next)
            return;
        }
        aura->SetDuration(left);
        if (waiting)
            player->ModifySpellCooldown(spell, -ms);
    }
}

class spell_ef_kit_restore_charge : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_restore_charge);

    bool _enemyDone = false;

    void Restore(SpellEffectInfo const& eff)
    {
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!player)
            return;
        Emberfall::ClassKit::RestoreCharges(player, uint32(eff.MiscValue), eff.TriggerSpell,
            uint32(std::max(0, eff.CalcValue(player))));
    }

    void HandleDummy(SpellEffIndex effIndex)
    {
        SpellEffectInfo const& eff = GetSpellInfo()->Effects[effIndex];
        if (GetHitUnit() != GetCaster() || !eff.MiscValue || !eff.TriggerSpell || eff.MiscValueB == 1)
            return;
        Restore(eff);
    }

    void AfterHitTarget()
    {
        if (_enemyDone || !Hostile(GetCaster(), GetHitUnit()))
            return;
        _enemyDone = true;
        for (SpellEffectInfo const& eff : GetSpellInfo()->Effects)
            if (eff.Effect == SPELL_EFFECT_DUMMY && eff.MiscValue && eff.TriggerSpell && eff.MiscValueB == 1)
                Restore(eff);
    }

    void Finish()
    {
        if (Unit* caster = GetCaster())
            Emberfall::ClassKit::HitBonuses(caster, caster, GetSpellInfo()->Id, 0, true);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_ef_kit_restore_charge::HandleDummy, EFFECT_ALL, SPELL_EFFECT_DUMMY);
        AfterHit += SpellHitFn(spell_ef_kit_restore_charge::AfterHitTarget);
        AfterCast += SpellCastFn(spell_ef_kit_restore_charge::Finish);
    }
};

class spell_ef_kit_heal_circle : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_heal_circle);

    uint32 _pickedTick = 0;
    std::vector<ObjectGuid> _picked;

    // the spell's dummy effect whose trigger heals: the tick, the raid cap, the burst
    SpellEffectInfo const* Circle()
    {
        for (SpellEffectInfo const& eff : GetSpellInfo()->Effects)
            if (eff.Effect == SPELL_EFFECT_DUMMY && eff.TriggerSpell)
                if (SpellInfo const* trig = sSpellMgr->GetSpellInfo(eff.TriggerSpell); trig && trig->HasEffect(SPELL_EFFECT_HEAL))
                    return &eff;
        return nullptr;
    }

    bool CheckTarget(Unit* target)
    {
        return GroupAlly(GetCaster(), target);
    }

    void Pick(Unit* caster, uint32 tick, int32 cap)
    {
        _pickedTick = tick;
        _picked.clear();
        std::vector<Unit*> units;
        std::vector<float> health;
        for (auto const& [guid, app] : GetAura()->GetApplicationMap())
            if (Unit* unit = app->GetTarget(); unit && unit->IsAlive())
            {
                units.push_back(unit);
                health.push_back(unit->GetHealthPct());
            }
        Player* player = caster->GetCharmerOrOwnerPlayerOrPlayerItself();
        bool const raid = player && player->GetGroup() && player->GetGroup()->isRaidGroup();
        for (size_t i : SL::CircleTargets(health, raid, cap))
            _picked.push_back(units[i]->GetGUID());
    }

    void Tick(AuraEffect const* aurEff)
    {
        Unit* caster = GetCaster();
        Unit* target = GetTarget();
        SpellEffectInfo const* circle = Circle();
        if (!caster || !target || !circle || !target->IsAlive())
            return;
        uint32 const tick = aurEff->GetTickNumber();
        if (tick != _pickedTick)
            Pick(caster, tick, circle->CalcValue(caster));
        if (std::find(_picked.begin(), _picked.end(), target->GetGUID()) == _picked.end())
            return;
        caster->CastSpell(target, circle->TriggerSpell, true);
        if (int32(tick) >= aurEff->GetTotalTicks() && circle->MiscValue > 0)
            caster->CastSpell(target, uint32(circle->MiscValue), true);
    }

    void Register() override
    {
        DoCheckAreaTarget += AuraCheckAreaTargetFn(spell_ef_kit_heal_circle::CheckTarget);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_ef_kit_heal_circle::Tick, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

class spell_ef_kit_heal_riders : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_heal_riders);

    int32 _heal = 0;

    // Guiding Light: a low-health bonus before the crit
    void OnHitTarget()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target)
            return;
        int32 heal = GetHitHeal();
        for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
            if (uint32(eff->GetMiscValue()) == GetSpellInfo()->Id && eff->GetMiscValueB() > 0
                && !eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell)
                heal = SL::LowHealthBonus(heal, target->GetHealthPct(), eff->GetMiscValueB(), eff->GetAmount());
        if (heal != GetHitHeal())
            SetHitHeal(heal);
        _heal = heal;
    }

    void Rider(Unit* caster, Unit* target, AuraEffect const* eff, int32 full, bool crit)
    {
        uint32 const trigger = eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(trigger);
        int32 const bits = eff->GetMiscValueB();
        if (!info || ((bits & 1) && !crit))
            return;
        if (uint32 const charged = ChargedSpellOf(info))
        {
            // Boundless Mercy: the charged spell's next charge sooner
            if (eff->GetAmount() < 0)
                Emberfall::ClassKit::ShortenCharge(caster->ToPlayer(), charged, trigger, -eff->GetAmount());
            return;
        }
        int32 amount = SL::Pct(full, eff->GetAmount());
        if (amount <= 0)
            return;
        Unit* dest = target;
        if (bits & 2)
            dest = MostInjuredOther(caster, target, info->GetMaxRange(true, caster));
        if (!dest)
            return;
        if (info->Effects[EFFECT_0].ApplyAuraName == SPELL_AURA_PERIODIC_HEAL)
            amount = SL::PerTick(amount, info->GetMaxDuration(), int32(info->Effects[EFFECT_0].Amplitude));
        caster->CastCustomSpell(dest, trigger, &amount, nullptr, nullptr, true);
    }

    void Count(Unit* caster, AuraEffect const* eff)
    {
        uint32 const counter = uint32(eff->GetMiscValue());
        SpellInfo const* passive = eff->GetSpellInfo();
        SpellEffectInfo const& def = passive->Effects[eff->GetEffIndex()];
        Aura* aura = caster->GetAura(counter, caster->GetGUID());
        uint32 const before = aura ? aura->GetStackAmount() : 0;
        SL::CountStep const step = SL::Count(before, def.CalcValue(caster));
        if (step.fire)
        {
            caster->RemoveAurasDueToSpell(counter, caster->GetGUID());
            caster->CastSpell(caster, def.TriggerSpell, true);
            return;
        }
        if (!aura)
            aura = caster->AddAura(counter, caster);
        if (aura)
        {
            aura->SetStackAmount(uint8(step.count));
            aura->RefreshDuration();
        }
    }

    void AfterHitTarget()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || Hostile(caster, target))
            return;
        bool const crit = HitCrit(GetSpell(), target);
        int32 const full = crit ? int32(Unit::SpellCriticalHealingBonus(caster, GetSpellInfo(), uint32(std::max(0, _heal)), target))
                                : _heal;
        int32 const effective = GetHitHeal();
        // the overheal shield: a dummy beside the heal whose trigger is a school absorb
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            SpellEffectInfo const& eff = GetSpellInfo()->Effects[i];
            SpellInfo const* shield = eff.Effect == SPELL_EFFECT_DUMMY && eff.TriggerSpell ? sSpellMgr->GetSpellInfo(eff.TriggerSpell) : nullptr;
            if (!shield || shield->Effects[EFFECT_0].ApplyAuraName != SPELL_AURA_SCHOOL_ABSORB)
                continue;
            int32 share = SL::OverhealShare(full, effective, eff.CalcValue(caster));
            if (share > 0)
                caster->CastCustomSpell(target, eff.TriggerSpell, &share, nullptr, nullptr, true);
        }
        // the caster's riders and counters (copied first: a rider may change the aura list)
        std::vector<AuraEffect const*> riders, counters;
        for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
        {
            uint32 const trigger = eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell;
            if (!trigger)
                continue;
            uint32 const misc = uint32(eff->GetMiscValue());
            if (misc == GetSpellInfo()->Id)
                riders.push_back(eff);
            else if (misc)
                if (SpellInfo const* stack = sSpellMgr->GetSpellInfo(misc); stack && stack->StackAmount > 1
                    && eff->IsAffectedOnSpell(GetSpellInfo()))
                    counters.push_back(eff);
        }
        for (AuraEffect const* eff : riders)
            Rider(caster, target, eff, full, crit);
        for (AuraEffect const* eff : counters)
            Count(caster, eff);
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_ef_kit_heal_riders::OnHitTarget);
        AfterHit += SpellHitFn(spell_ef_kit_heal_riders::AfterHitTarget);
    }
};

class spell_ef_kit_pooled_absorb : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_pooled_absorb);

    bool _made = false;

    void CalcAmount(AuraEffect const* aurEff, int32& amount, bool& canBeRecalculated)
    {
        canBeRecalculated = false;
        Unit* owner = GetUnitOwner();
        if (!owner)
            return;
        SpellEffectInfo const& rule = GetSpellInfo()->Effects[EFFECT_1];
        int32 const add = rule.MiscValue == 1 ? int32(owner->CountPctFromMaxHealth(amount)) : amount;
        int32 const cap = int32(owner->CountPctFromMaxHealth(rule.CalcValue(GetCaster())));
        // the shield's first amount is made in the aura's constructor (nothing left yet); a refresh adds to what is left
        int32 const left = _made ? aurEff->GetAmount() : 0;
        _made = true;
        amount = SL::PooledAbsorb(left, add, cap);
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_ef_kit_pooled_absorb::CalcAmount, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
    }
};

class spell_ef_kit_combo_step : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_combo_step);

protected:
    uint32 _combo = 0;      // the combo aura
    int32 _after = 0;       // its stacks after a hit
    uint32 _afterCast = 0;  // cast on the caster then
    uint32 _redirect = 0;   // the button at N stacks: the step it casts instead of striking
    bool _advanced = false;

    bool Load() override
    {
        for (SpellEffectInfo const& eff : GetSpellInfo()->Effects)
            if (eff.Effect == SPELL_EFFECT_DUMMY && eff.MiscValue > 0 && eff.TargetA.GetTarget() == TARGET_UNIT_CASTER)
                if (SpellInfo const* combo = sSpellMgr->GetSpellInfo(uint32(eff.MiscValue)); combo && combo->StackAmount > 0)
                {
                    _combo = uint32(eff.MiscValue);
                    _after = eff.BasePoints;
                    _afterCast = eff.TriggerSpell;
                    break;
                }
        return _combo != 0;
    }

    // the button (step 1, no stacks of its own combo aura) at N stacks: step N+1 instead
    void Before()
    {
        Unit* caster = GetCaster();
        if (!caster || _after != 1)
            return;
        SpellInfo const* combo = sSpellMgr->GetSpellInfo(_combo);
        Aura const* aura = caster->GetAura(_combo, caster->GetGUID());
        int32 const stacks = aura ? aura->GetStackAmount() : 0;
        int32 const step = SL::ComboStep(stacks, MAX_SPELL_EFFECTS);
        if (step && combo && combo->Effects[step - 1].TriggerSpell)
            _redirect = combo->Effects[step - 1].TriggerSpell;
    }

    void DropTarget(WorldObject*& target)
    {
        if (_redirect)
            target = nullptr;
    }

    void Redirect()
    {
        Unit* caster = GetCaster();
        Unit* target = GetExplTargetUnit();
        if (_redirect && caster && target)
            caster->CastSpell(target, _redirect, true);
    }

    void AfterHitTarget()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!Hostile(caster, target))
            return;
        // a rider at every enemy struck (Radiant Blow's holy part)
        for (SpellEffectInfo const& eff : GetSpellInfo()->Effects)
            if (eff.Effect == SPELL_EFFECT_DUMMY && eff.TriggerSpell && eff.TargetA.GetTarget() == TARGET_UNIT_TARGET_ENEMY)
                caster->CastSpell(target, eff.TriggerSpell, true);
        if (_advanced)
            return;
        _advanced = true;
        if (_after <= 0)
            caster->RemoveAurasDueToSpell(_combo, caster->GetGUID());
        else
        {
            Aura* aura = caster->GetAura(_combo, caster->GetGUID());
            if (!aura)
                aura = caster->AddAura(_combo, caster);
            if (aura)
                aura->SetStackAmount(uint8(_after));
        }
        if (_afterCast)
            caster->CastSpell(caster, _afterCast, true);
    }

    // the later steps (Radiant Blow, Dawnbreak): only their hits
    void Register() override
    {
        AfterHit += SpellHitFn(spell_ef_kit_combo_step::AfterHitTarget);
    }
};

// The button (Dawnstrike, the first step, its effect 1 at its unit target): at N stacks it casts step N+1 instead.
class spell_ef_kit_combo_button : public spell_ef_kit_combo_step
{
    PrepareSpellScript(spell_ef_kit_combo_button);

    void Register() override
    {
        BeforeCast += SpellCastFn(spell_ef_kit_combo_button::Before);
        OnObjectTargetSelect += SpellObjectTargetSelectFn(spell_ef_kit_combo_button::DropTarget, EFFECT_0, TARGET_UNIT_TARGET_ENEMY);
        AfterCast += SpellCastFn(spell_ef_kit_combo_button::Redirect);
        AfterHit += SpellHitFn(spell_ef_kit_combo_button::AfterHitTarget);
    }
};

class spell_ef_kit_mana_on_hit : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_mana_on_hit);

    bool _done = false;

    void AfterHitTarget()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (_done || !Hostile(caster, target))
            return;
        _done = true;
        for (SpellEffectInfo const& eff : GetSpellInfo()->Effects)
            if (eff.Effect == SPELL_EFFECT_DUMMY && eff.TargetA.GetTarget() == TARGET_UNIT_CASTER && !eff.TriggerSpell)
            {
                uint32 const pool = SL::ManaPool(caster->GetMaxPower(POWER_MANA), caster->GetCreateMana(), eff.MiscValue == 1);
                int32 const mana = SL::ManaOnHit(pool, eff.CalcValue(caster),
                    HitCrit(GetSpell(), target), eff.MiscValueB == 1);
                if (mana > 0)
                    caster->EnergizeBySpell(caster, GetSpellInfo()->Id, uint32(mana), POWER_MANA);
                return;
            }
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_ef_kit_mana_on_hit::AfterHitTarget);
    }
};

void AddEmberfallClassKitSunScripts()
{
    RegisterSpellScript(spell_ef_kit_restore_charge);
    RegisterSpellScript(spell_ef_kit_heal_circle);
    RegisterSpellScript(spell_ef_kit_heal_riders);
    RegisterSpellScript(spell_ef_kit_pooled_absorb);
    RegisterSpellScript(spell_ef_kit_combo_step);
    RegisterSpellScript(spell_ef_kit_combo_button);
    RegisterSpellScript(spell_ef_kit_mana_on_hit);
}
