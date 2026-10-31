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

// Emberfall class kits, the Hunter's Spirit Caller v2.0 (Emberfall docs/design/classes/hunter-spiritcaller.md: ankarna's
// "Spirit Caller - Developer Handoff v2.0", 2026-10-31). Generic like the scripts of src/emberfall_classkit.cpp: each reads
// its numbers from the spell's own effects or from the caster's rule passives (Emberfall's classkit/scripts_spirit.py
// checks the contracts); the arithmetic is src/classkit/EfSpiritLogic.h (tests/classkit_spirit_tests.cpp). The talent
// rules Focused Fury (_exact_count_damage) and Serpent's Judgment (_dot_burst) are read by spell_ef_kit_rules
// (src/emberfall_classkit_exec.cpp).
//
//   spell_ef_kit_refresh_window     a self buff that may be cast again only while at most a dummy's value in ms of it is
//                                   left (a dummy effect on the caster without misc; its trigger, an aura of the caster's,
//                                   is made to end with the buff at every cast) (Spirit of the Rabbit: 3000, Adaptive
//                                   Reflexes' stacks)
//   spell_ef_kit_hit_taken_stacks   a passive's proc aura on the melee hits taken: while its misc aura holds, a hit or a crit
//                                   adds value stacks of its trigger (its time set to what is left of the misc aura); a
//                                   dodge takes them all (Adaptive Reflexes: +10 % dodge a hit, no cap)
//   spell_ef_kit_renewal            a heal over time (effect 2) with an absorb (effect 3): the caster's
//                                   spell_ef_kit_rule_low_health_hot rules naming it raise its ticks by value % when it is
//                                   cast at or below misc B % health (Desperate Renewal: 100 at 50); with a
//                                   spell_ef_kit_rule_hot_cheat_death rule naming it, a hit that would kill while it runs
//                                   (after the other shields) leaves 1 health, the rule's trigger heals what its ticks had
//                                   left at once and it goes: once an activation (Undying Spirit)
//   spell_ef_kit_hit_cap            a passive absorb (effect 1) with a dummy (effect 2: misc = an aura, value = percent):
//                                   while the owner holds the aura, what any one hit has beyond value % of the owner's max
//                                   health is absorbed (Unbroken Spirit: 30 % in Nature's Wrath)
//   spell_ef_kit_becomes            an ability with a dummy on the caster (misc = an aura of the caster's, trigger = another
//                                   ability): while the aura holds, the cast strikes none of its own targets, the aura goes
//                                   and the trigger is cast at the caster instead, the cost and cooldown the ability's own
//                                   (Venomous Fang -> Venom Spit after a Bear Claw)
//   spell_ef_kit_rule_*             the rule markers of the four rules above (EfClassKit.h Rules): nothing happens in them;
//                                   and spell_ef_kit_rule_auto_attack_pct (a passive's dummy without misc: the owner's
//                                   white swing keeps value % of its damage on top of the level's rule; the Spirit Caller's
//                                   Spirit Bond, 50; read by src/emberfall_classkit_power.cpp's auto attack hook)

#include "EfClassKit.h"
#include "EfSpiritLogic.h"

#include "ObjectAccessor.h"
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
#include <climits>
#include <vector>

namespace SL = Emberfall::ClassKit::SpiritLogic;

namespace
{
    uint32 TriggerOf(AuraEffect const* eff)
    {
        return eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell;
    }

    // What the owner's other absorbs (with an amount of their own: not the unlimited ones that decide per hit) can still
    // take of a blow of `schoolMask` (spell_ef_kit_cheat_death's rule: a fatal hit is one the shields do not stop).
    uint32 OtherShields(Unit* owner, AuraEffect const* self, uint32 schoolMask)
    {
        uint32 others = 0;
        for (AuraEffect const* shield : owner->GetAuraEffectsByType(SPELL_AURA_SCHOOL_ABSORB))
            if (shield != self && shield->GetAmount() > 0 && (uint32(shield->GetMiscValue()) & schoolMask))
                others += uint32(shield->GetAmount());
        return others;
    }
}

class spell_ef_kit_refresh_window : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_refresh_window);

    // the window's dummy: on the caster, no misc (the charges' dummy names its talent)
    SpellEffectInfo const* Window()
    {
        for (SpellEffectInfo const& eff : GetSpellInfo()->Effects)
            if (eff.Effect == SPELL_EFFECT_DUMMY && eff.TargetA.GetTarget() == TARGET_UNIT_CASTER && !eff.MiscValue)
                return &eff;
        return nullptr;
    }

    SpellCastResult Check()
    {
        Unit* caster = GetCaster();
        SpellEffectInfo const* window = Window();
        if (!caster || !window)
            return SPELL_CAST_OK;
        Aura const* own = caster->GetAura(GetSpellInfo()->Id, caster->GetGUID());
        if (own && !SL::MayRefresh(own->GetDuration(), window->CalcValue(caster)))
            return SPELL_FAILED_NOT_READY;
        return SPELL_CAST_OK;
    }

    // the buff renewed: the aura that ends with it (Adaptive Reflexes' stacks) gets its new time
    void AfterHitTarget()
    {
        Unit* caster = GetCaster();
        SpellEffectInfo const* window = Window();
        if (!caster || !window || !window->TriggerSpell || GetHitUnit() != caster)
            return;
        Aura const* own = caster->GetAura(GetSpellInfo()->Id, caster->GetGUID());
        Aura* follows = caster->GetAura(window->TriggerSpell, caster->GetGUID());
        if (!own || !follows)
            return;
        follows->SetMaxDuration(own->GetMaxDuration());
        follows->SetDuration(own->GetDuration());
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_ef_kit_refresh_window::Check);
        AfterHit += SpellHitFn(spell_ef_kit_refresh_window::AfterHitTarget);
    }
};

class spell_ef_kit_hit_taken_stacks : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_hit_taken_stacks);

    bool CheckProc(ProcEventInfo& info)
    {
        Unit* owner = GetUnitOwner();
        if (!owner)
            return false;
        if (info.GetHitMask() & PROC_HIT_DODGE)
            return true;
        int32 const need = GetSpellInfo()->Effects[EFFECT_0].MiscValue;
        return (info.GetHitMask() & (PROC_HIT_NORMAL | PROC_HIT_CRITICAL)) && (!need || owner->HasAura(uint32(need)));
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& info)
    {
        PreventDefaultAction();
        Unit* owner = GetUnitOwner();
        uint32 const trigger = GetSpellInfo()->Effects[EFFECT_0].TriggerSpell;
        if (!owner || !trigger)
            return;
        // a dodge takes the bonus away
        if (info.GetHitMask() & PROC_HIT_DODGE)
        {
            owner->RemoveAurasDueToSpell(trigger, owner->GetGUID());
            return;
        }
        int32 const need = GetSpellInfo()->Effects[EFFECT_0].MiscValue;
        Aura const* holds = need ? owner->GetAura(uint32(need)) : nullptr;
        if (need && !holds)
            return;
        int32 add = std::max(1, aurEff->GetAmount());
        Aura* stacks = owner->GetAura(trigger, owner->GetGUID());
        if (!stacks)
        {
            stacks = owner->AddAura(trigger, owner);
            if (!stacks)
                return;
            --add;
        }
        if (add > 0)
            stacks->ModStackAmount(add);
        // the stacks end with the aura they need (Spirit of the Rabbit)
        if (holds)
        {
            stacks->SetMaxDuration(holds->GetDuration());
            stacks->SetDuration(holds->GetDuration());
        }
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_ef_kit_hit_taken_stacks::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_ef_kit_hit_taken_stacks::HandleProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

class spell_ef_kit_renewal : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_renewal);

    bool _spent = false;   // Undying Spirit: once an activation

    // the aura is made before the spell's instant heal lands, so the health read here is the health at the cast
    void CalcTick(AuraEffect const* /*aurEff*/, int32& amount, bool& canBeRecalculated)
    {
        canBeRecalculated = false;
        Unit* caster = GetCaster();
        if (!caster)
            return;
        for (AuraEffect const* rule : Emberfall::ClassKit::Rules(caster, "spell_ef_kit_rule_low_health_hot", GetId()))
            amount = SL::LowHealthTick(amount, caster->GetHealthPct(), rule->GetMiscValueB(), rule->GetAmount());
    }

    void CalcAbsorb(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        amount = -1; // unlimited: Absorb decides (only a fatal hit, with the rule)
    }

    void Absorb(AuraEffect* aurEff, DamageInfo& dmgInfo, uint32& absorbAmount)
    {
        absorbAmount = 0;
        Unit* owner = GetUnitOwner();
        if (!owner || _spent)
            return;
        std::vector<AuraEffect const*> const rules =
            Emberfall::ClassKit::Rules(owner, "spell_ef_kit_rule_hot_cheat_death", GetId());
        if (rules.empty())
            return;
        uint32 const damage = dmgInfo.GetDamage();
        uint32 const others = OtherShields(owner, aurEff, uint32(dmgInfo.GetSchoolMask()));
        if (damage <= others)
            return;
        uint32 const take = SL::FatalAbsorb(damage - others, owner->GetHealth());
        if (!take)
            return;
        _spent = true;
        absorbAmount = take;
        // what the ticks had left, at once (healed now: the hit then leaves 1 health and the heal on top)
        AuraEffect const* hot = GetAura()->GetEffect(EFFECT_1);
        int64 const left = hot ? SL::HealLeft(owner->GetMaxHealth(), hot->GetAmount(),
            SL::TicksLeft(uint32(std::max(0, hot->GetTotalTicks())), hot->GetTickNumber())) : 0;
        if (uint32 const trigger = TriggerOf(rules.front()); trigger && left > 0)
        {
            int32 const amount = int32(std::min<int64>(left, INT_MAX));
            owner->CastCustomSpell(owner, trigger, &amount, nullptr, nullptr, true);
        }
        // the heal over time is consumed: it goes once this hit is done (not inside the engine's absorb loop)
        ObjectGuid const guid = owner->GetGUID();
        uint32 const id = GetId();
        owner->m_Events.AddEventAtOffset([owner, guid, id]()
        {
            if (owner->IsInWorld())
                owner->RemoveAurasDueToSpell(id, guid);
        }, Milliseconds(1));
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_ef_kit_renewal::CalcTick, EFFECT_1, SPELL_AURA_ANY);
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_ef_kit_renewal::CalcAbsorb, EFFECT_2, SPELL_AURA_SCHOOL_ABSORB);
        OnEffectAbsorb += AuraEffectAbsorbFn(spell_ef_kit_renewal::Absorb, EFFECT_2);
    }
};

class spell_ef_kit_hit_cap : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_hit_cap);

    void CalcAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        amount = -1; // unlimited: Absorb decides
    }

    void Absorb(AuraEffect* /*aurEff*/, DamageInfo& dmgInfo, uint32& absorbAmount)
    {
        absorbAmount = 0;
        Unit* owner = GetUnitOwner();
        AuraEffect const* cap = GetAura()->GetEffect(EFFECT_1);
        if (!owner || !cap)
            return;
        int32 const need = cap->GetMiscValue();
        if (need && !owner->HasAura(uint32(need)))
            return;
        absorbAmount = SL::OverCap(dmgInfo.GetDamage(), owner->GetMaxHealth(), cap->GetAmount());
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_ef_kit_hit_cap::CalcAmount, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
        OnEffectAbsorb += AuraEffectAbsorbFn(spell_ef_kit_hit_cap::Absorb, EFFECT_0);
    }
};

class spell_ef_kit_becomes : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_becomes);

    bool _decided = false;
    uint32 _aura = 0;          // the aura that made this cast the other ability (Venom Spit Ready), 0: none
    uint32 _instead = 0;       // that ability (Venom Spit)

    // decided once, at the first look (the target selection runs before the cast's hooks on some paths)
    void Decide()
    {
        if (_decided)
            return;
        _decided = true;
        Unit* caster = GetCaster();
        if (!caster)
            return;
        for (SpellEffectInfo const& eff : GetSpellInfo()->Effects)
            if (eff.Effect == SPELL_EFFECT_DUMMY && eff.TargetA.GetTarget() == TARGET_UNIT_CASTER && eff.MiscValue
                && eff.TriggerSpell && caster->HasAura(uint32(eff.MiscValue)))
            {
                _aura = uint32(eff.MiscValue);
                _instead = eff.TriggerSpell;
                return;
            }
    }

    // the other ability instead: this cast's own enemy is not struck (no strike, no venom)
    void SelectTarget(WorldObject*& target)
    {
        Decide();
        if (_instead)
            target = nullptr;
    }

    void After()
    {
        Decide();
        Unit* caster = GetCaster();
        if (!caster || !_instead)
            return;
        caster->RemoveAurasDueToSpell(_aura);
        caster->CastSpell(caster, _instead, true);
    }

    void Register() override
    {
        OnObjectTargetSelect += SpellObjectTargetSelectFn(spell_ef_kit_becomes::SelectTarget, EFFECT_ALL, TARGET_UNIT_TARGET_ENEMY);
        AfterCast += SpellCastFn(spell_ef_kit_becomes::After);
    }
};

// The Spirit Caller's rule markers: nothing happens in them (EfClassKit.h Rules finds the passives by these names).
class spell_ef_kit_spirit_rule : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_spirit_rule);

    void Register() override { }
};

void AddEmberfallClassKitSpiritScripts()
{
    RegisterSpellScript(spell_ef_kit_refresh_window);
    RegisterSpellScript(spell_ef_kit_hit_taken_stacks);
    RegisterSpellScript(spell_ef_kit_renewal);
    RegisterSpellScript(spell_ef_kit_hit_cap);
    RegisterSpellScript(spell_ef_kit_becomes);
    for (char const* name : { "spell_ef_kit_rule_exact_count_damage", "spell_ef_kit_rule_low_health_hot",
                              "spell_ef_kit_rule_hot_cheat_death", "spell_ef_kit_rule_dot_burst",
                              "spell_ef_kit_rule_auto_attack_pct" })
        RegisterSpellScriptWithArgs(spell_ef_kit_spirit_rule, name);
}
