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

// Emberfall class kits, the Warrior's Barbarian (Emberfall docs/design/classes/warrior-barbarian.md: ankarna's
// "Warrior - Barbarian Tank spec", 2026-10-24): Battlelust as the spec's power (the generic spec power of
// src/classkit/EfSpecPower.cpp, built by spell_ef_kit_build_power and spell_ef_kit_gain_power), Savage Resilience's
// stacks with a timer each, Thunderous Bellow's heal from the enemies struck hardest, Ruinous Cleave's damage by the
// number of enemies. Generic like the scripts of src/emberfall_classkit.cpp: each reads its numbers from the spell's own
// effects or from the caster's rule passives (Emberfall's classkit/scripts_lust.py checks the contracts); the arithmetic
// is src/classkit/EfLustLogic.h (tests/classkit_lust_tests.cpp).
//
//   spell_ef_kit_timed_stacks       the ability's side of spell_ef_kit_rule_timed_stacks: once a cast, on the first enemy it
//                                   strikes (a charge's hit on arrival), every rule of the caster's naming the spell (misc,
//                                   or misc 0 and its class mask) adds its value in stacks of its trigger (Savage Cleave 1,
//                                   Bonecrusher and Ruinous Cleave 2, Worldshaker 3)
//   spell_ef_kit_timed_stack_aura   the stacking aura of those stacks (Savage Resilience: effect 1 its bonus a stack, e.g.
//                                   -3 % damage taken; effect 2 a periodic dummy that drops the stacks that ran out; its
//                                   duration and StackAmount the stack's timer and the most stacks): every stack keeps a
//                                   timer of its own, gaining stacks never refreshes the old ones, at the cap the new ones
//                                   replace those with the least time left; the aura lasts until the last one runs out
//   spell_ef_kit_top_heal           a dummy on the caster with a trigger (the heal), value = percent, misc B = how many
//                                   enemies count (0: all): once the cast is done, the trigger heals the caster for that
//                                   percent of the damage the spell dealt to the misc B enemies it struck hardest (crits
//                                   in; nothing struck, nothing healed) (Thunderous Bellow: 30 % of the top 5)
//   spell_ef_kit_count_scaled       the damage on every enemy by how many enemies the cast strikes: the dummies on the
//                                   caster hold the percents in order, value, misc and misc B each (effect 2: 1, 2, 3
//                                   enemies; effect 3: 4, 5+ enemies; a 0 ends the table, more enemies take the last)
//                                   (Ruinous Cleave: 250 / 200 / 150 / 120 / 100)
//   spell_ef_kit_arrival_strike     a charge's dummies at the enemy with a trigger cast it at that enemy when the charge's
//                                   hit lands, on arrival (a spell speed: Worldshaker's slam where the warrior lands)
//   spell_ef_kit_rule_timed_stacks  rule marker (EfClassKit.h Rules): a passive's dummy, misc or affects = the abilities,
//                                   value = stacks, trigger = the timed-stack aura (Savage Resilience)
//   spell_ef_kit_rule_build_pct     rule marker read by spell_ef_kit_build_power: while the aura holds, the builders it
//                                   names build value percent more (Primal Fury: +50 %)

#include "EfClassKit.h"
#include "EfLustLogic.h"

#include "DataMap.h"
#include "GameTime.h"
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
#include <map>
#include <vector>

namespace
{
    namespace LustLogic = Emberfall::ClassKit::LustLogic;

    constexpr char const* kStacksKey = "ef_timed_stacks";

    // Per unit: the expiry time of every stack of each timed-stack aura it carries.
    struct TimedStacks : public DataMap::Base
    {
        std::map<uint32, std::vector<uint64>> byAura;
    };

    uint64 NowMs() { return uint64(GameTime::GetGameTimeMS().count()); }

    // `count` stacks of the timed-stack aura `auraId` on `unit` (its own aura), each with the aura's duration (spell mods
    // in) as its own timer; returns the stacks held.
    uint32 AddTimedStacks(Unit* unit, uint32 auraId, uint32 count)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(auraId);
        if (!unit || !info || !count)
            return 0;
        int32 duration = info->GetMaxDuration();
        if (Player* modOwner = unit->GetSpellModOwner())
            modOwner->ApplySpellMod(auraId, SPELLMOD_DURATION, duration);
        uint32 const cap = std::max<uint32>(1, info->StackAmount);
        uint64 const now = NowMs();
        std::vector<uint64>& stacks = unit->CustomData.GetDefault<TimedStacks>(kStacksKey)->byAura[auraId];
        Aura* aura = unit->GetAura(auraId, unit->GetGUID());
        if (!aura)
            stacks.clear();
        uint32 const held = LustLogic::AddStacks(stacks, now, count, uint32(std::max(0, duration)), cap);
        if (!held)
            return 0;
        if (!aura)
            aura = unit->AddAura(auraId, unit);
        if (!aura)
        {
            stacks.clear();
            return 0;
        }
        if (aura->GetStackAmount() != held)
            aura->SetStackAmount(uint8(held));
        int32 const left = int32(LustLogic::LongestLeft(stacks, now));
        aura->SetMaxDuration(left);
        aura->SetDuration(left);
        return held;
    }
}

class spell_ef_kit_timed_stacks : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_timed_stacks);

    bool _granted = false;

    void AfterHitTarget()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (_granted || !caster || !target || target == caster)
            return;
        _granted = true;
        for (AuraEffect const* rule : Emberfall::ClassKit::Rules(caster, "spell_ef_kit_rule_timed_stacks", GetSpellInfo()->Id))
            if (uint32 const aura = rule->GetSpellInfo()->Effects[rule->GetEffIndex()].TriggerSpell)
                AddTimedStacks(caster, aura, uint32(std::max(0, rule->GetAmount())));
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_ef_kit_timed_stacks::AfterHitTarget);
    }
};

class spell_ef_kit_timed_stack_aura : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_timed_stack_aura);

    // the stacks that ran out go; the aura's own duration is the last one's, so it ends with it
    void Tick(AuraEffect const* /*aurEff*/)
    {
        Unit* owner = GetUnitOwner();
        TimedStacks* st = owner ? owner->CustomData.Get<TimedStacks>(kStacksKey) : nullptr;
        if (!st)
            return;
        std::vector<uint64>& stacks = st->byAura[GetId()];
        uint32 const left = LustLogic::PruneStacks(stacks, NowMs());
        if (!left)
        {
            Remove(AURA_REMOVE_BY_EXPIRE);
            return;
        }
        if (GetStackAmount() != left)
            GetAura()->SetStackAmount(uint8(left));
    }

    void Gone(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Unit* owner = GetUnitOwner())
            if (TimedStacks* st = owner->CustomData.Get<TimedStacks>(kStacksKey))
                st->byAura.erase(GetId());
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_ef_kit_timed_stack_aura::Tick, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_ef_kit_timed_stack_aura::Gone, EFFECT_0, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

class spell_ef_kit_top_heal : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_top_heal);

    std::vector<int32> _dealt;   // the damage to each enemy struck (AfterHit: after mitigation and the crit)

    void AfterHitTarget()
    {
        Unit* target = GetHitUnit();
        if (target && target != GetCaster() && GetHitDamage() > 0)
            _dealt.push_back(GetHitDamage());
    }

    void Finish()
    {
        Unit* caster = GetCaster();
        if (!caster || _dealt.empty())
            return;
        for (SpellEffectInfo const& eff : GetSpellInfo()->Effects)
        {
            if (eff.Effect != SPELL_EFFECT_DUMMY || eff.TargetA.GetTarget() != TARGET_UNIT_CASTER || !eff.TriggerSpell)
                continue;
            uint32 const top = eff.MiscValueB > 0 ? uint32(eff.MiscValueB) : uint32(_dealt.size());
            int32 const heal = LustLogic::TopHeal(_dealt, top, eff.CalcValue(caster));
            if (heal > 0)
                caster->CastCustomSpell(caster, eff.TriggerSpell, &heal, nullptr, nullptr, true);
            return;
        }
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_ef_kit_top_heal::AfterHitTarget);
        AfterCast += SpellCastFn(spell_ef_kit_top_heal::Finish);
    }
};

class spell_ef_kit_count_scaled : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_count_scaled);

    int32 _pct = -1;   // the percent for this cast, worked out at the first hit (every target is chosen by then)

    int32 Percent(Unit* caster)
    {
        if (_pct >= 0)
            return _pct;
        std::vector<int32> table;
        bool ended = false;
        for (SpellEffectInfo const& eff : GetSpellInfo()->Effects)
        {
            if (ended || eff.Effect != SPELL_EFFECT_DUMMY || eff.TargetA.GetTarget() != TARGET_UNIT_CASTER)
                continue;
            for (int32 v : { eff.CalcValue(caster), eff.MiscValue, eff.MiscValueB })
            {
                if (v <= 0)
                {
                    ended = true;
                    break;
                }
                table.push_back(v);
            }
        }
        uint32 enemies = 0;
        for (TargetInfo const& t : *GetSpell()->GetUniqueTargetInfo())
            if (t.targetGUID != caster->GetGUID())
                ++enemies;
        _pct = LustLogic::CountScalePct(enemies, table);
        return _pct;
    }

    void HandleHit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || target == caster || GetHitDamage() <= 0)
            return;
        int32 const pct = Percent(caster);
        if (pct != 100)
            SetHitDamage(LustLogic::Pct(GetHitDamage(), pct));
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_ef_kit_count_scaled::HandleHit);
    }
};

class spell_ef_kit_arrival_strike : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_arrival_strike);

    void HandleDummy(SpellEffIndex effIndex)
    {
        SpellEffectInfo const& eff = GetSpellInfo()->Effects[effIndex];
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (eff.TriggerSpell && eff.TargetA.GetTarget() != TARGET_UNIT_CASTER && caster && target && target != caster
            && target->IsAlive())
            caster->CastSpell(target, eff.TriggerSpell, true);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_ef_kit_arrival_strike::HandleDummy, EFFECT_ALL, SPELL_EFFECT_DUMMY);
    }
};

// The Barbarian's rule markers: nothing happens in them (EfClassKit.h Rules finds the passives by these names).
class spell_ef_kit_lust_rule : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_lust_rule);

    void Register() override { }
};

void AddEmberfallClassKitLustScripts()
{
    RegisterSpellScript(spell_ef_kit_timed_stacks);
    RegisterSpellScript(spell_ef_kit_timed_stack_aura);
    RegisterSpellScript(spell_ef_kit_top_heal);
    RegisterSpellScript(spell_ef_kit_count_scaled);
    RegisterSpellScript(spell_ef_kit_arrival_strike);
    for (char const* name : { "spell_ef_kit_rule_timed_stacks", "spell_ef_kit_rule_build_pct" })
        RegisterSpellScriptWithArgs(spell_ef_kit_lust_rule, name);
}
