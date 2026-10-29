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
//
// The finalized tree (ankarna's "Barbarian Tank" handoff, 2026-10-29):
//   spell_ef_kit_overheal_shield    the ability side of spell_ef_kit_rule_overheal_shield: the overheal of the spell's heal
//                                   on the caster goes into the rule's absorb (trigger, a school absorb on the caster), on
//                                   top of what it still holds, at most value percent of max health (Overflowing Fury)
//   spell_ef_kit_tally_hit          a damage helper whose damage is tallied per caster (Vengeful Roar's retaliation); the
//                                   caster's spell_ef_kit_rule_hit_gain rules naming it give value points of the spec power
//                                   a hit, at most misc B a second (0: no cap) (Vengeful Fury: 5, 5)
//   spell_ef_kit_tally_aura         a caster aura with an effect whose trigger is the tallied spell (Vengeful Roar): the tally
//                                   starts at its apply; when it runs out (expiry, not a cancel or a death) the caster's
//                                   spell_ef_kit_rule_tally_wave rules naming the aura cast their trigger on the caster with
//                                   value percent of the tally as base points (Final Reckoning: 50 %)
//   spell_ef_kit_count_scaled       also: the caster's spell_ef_kit_rule_count_scale rules naming the spell: value percent
//                                   against one enemy, 100 from misc B enemies on, linear between; the largest rule
//                                   replaces the table (Focused Devastation: 250, 5); no table dummies: 100
//   spell_ef_kit_rule_stack_boost   rule marker: misc = a timed-stack aura, value = more of its effect 1 a stack (Primal
//                                   Resilience: -3, with spell_ef_kit_conditional: only while Primal Fury holds)
//   spell_ef_kit_rule_full_stacks   rule marker: misc = a timed-stack aura, trigger = an aura on the owner while the stacks
//                                   are at their maximum, gone the moment they fall below (Savage Juggernaut)
//   spell_ef_kit_caster_ward        marker on an enemy debuff: its dummy auras' amounts are percent less damage the bearer
//                                   deals to the debuff's caster; several from one caster: the strongest only (Crippling
//                                   Blow 10 %, Bloodbound Warlord 15 %; EmberfallLustUnitScript below)
//   spell_ef_kit_spend_rules        a spender's side of spell_ef_kit_rule_spend_cut: every point of power the cast cost takes
//                                   value ms (negative) off the rule's trigger's cooldown (Bloodthirsty Berserker: -100 ms
//                                   a Battlelust on Primal Fury)
//   spell_ef_kit_pull_in            a helper at one enemy: effect 1's dummy (value = yards short of the caster, misc B =
//                                   yd/s) pulls it to the caster; never a player, a boss, an enemy immune to the effect
//                                   (its mechanic), a rooted one or one farther than the helper's range (Savage Attraction)

#include "EfClassKit.h"
#include "EfFlowLogic.h"
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

    uint32 TriggerOf(AuraEffect const* eff)
    {
        return eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell;
    }

    // Savage Juggernaut: the full-stack rules naming `auraId` follow the stacks `held` of the owner (cap: the most).
    void FollowFullStacks(Unit* owner, uint32 auraId, uint32 held, uint32 cap)
    {
        if (!owner)
            return;
        bool const full = LustLogic::AtFullStacks(held, cap);
        for (AuraEffect const* rule : Emberfall::ClassKit::Rules(owner, "spell_ef_kit_rule_full_stacks", auraId))
        {
            uint32 const trigger = TriggerOf(rule);
            if (!trigger)
                continue;
            Aura* bonus = owner->GetAura(trigger, owner->GetGUID());
            if (full && !bonus)
                owner->CastSpell(owner, trigger, true);
            else if (full)
                bonus->RefreshDuration();
            else if (bonus)
                owner->RemoveAurasDueToSpell(trigger, owner->GetGUID());
        }
    }

    // Per caster: the damage a tallied spell has done since its tally aura came, and the per-second gain windows.
    constexpr char const* kTallyKey = "ef_tally";
    struct Tally : public DataMap::Base
    {
        std::map<uint32, int64> sums;                            // tallied spell -> damage
        std::map<uint32, std::pair<uint64, int32>> windows;      // rule spell -> (window start, gained in it)
    };

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
        FollowFullStacks(unit, auraId, held, cap);
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

    int32 _boost = 0;   // the stack boost last put into effect 1's amount (a stack)

    int32 Boost()
    {
        int32 sum = 0;
        if (Unit* owner = GetUnitOwner())
            for (AuraEffect const* rule : Emberfall::ClassKit::Rules(owner, "spell_ef_kit_rule_stack_boost", GetId()))
                sum += rule->GetAmount();
        return sum;
    }

    uint32 Cap() const { return std::max<uint32>(1, GetSpellInfo()->StackAmount); }

    // Primal Resilience: the rules' boost on every stack (the core multiplies by the stacks after this hook)
    void CalcAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& canBeRecalculated)
    {
        canBeRecalculated = true;
        _boost = Boost();
        amount += _boost;
    }

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
        if (Boost() != _boost)
            if (AuraEffect* eff = GetAura()->GetEffect(EFFECT_0))
                eff->RecalculateAmount(owner);
        FollowFullStacks(owner, GetId(), left, Cap());
    }

    void Gone(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Unit* owner = GetUnitOwner())
        {
            if (TimedStacks* st = owner->CustomData.Get<TimedStacks>(kStacksKey))
                st->byAura.erase(GetId());
            FollowFullStacks(owner, GetId(), 0, Cap());
        }
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_ef_kit_timed_stack_aura::CalcAmount, EFFECT_0, SPELL_AURA_ANY);
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
        // Focused Devastation: the largest of the rules naming the spell replaces the table's percent
        bool ruled = false;
        int32 best = 0;
        for (AuraEffect const* rule : Emberfall::ClassKit::Rules(caster, "spell_ef_kit_rule_count_scale", GetSpellInfo()->Id))
        {
            int32 const pct = LustLogic::LinearCountPct(enemies, rule->GetAmount(), rule->GetMiscValueB());
            best = ruled ? std::max(best, pct) : pct;
            ruled = true;
        }
        if (ruled)
            _pct = std::max(0, best);
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

class spell_ef_kit_overheal_shield : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_overheal_shield);

    // before the heal lands (OnHit): what the caster lacks, so the overheal is what the heal would waste
    void HandleHit()
    {
        Unit* caster = GetCaster();
        if (!caster || GetHitUnit() != caster || GetHitHeal() <= 0)
            return;
        int64 const heal = GetHitHeal();
        int64 const missing = int64(caster->GetMaxHealth()) - int64(caster->GetHealth());
        for (AuraEffect const* rule : Emberfall::ClassKit::Rules(caster, "spell_ef_kit_rule_overheal_shield", GetSpellInfo()->Id))
        {
            uint32 const shield = TriggerOf(rule);
            if (!shield)
                continue;
            int64 const cap = int64(caster->GetMaxHealth()) * std::max(0, rule->GetAmount()) / 100;
            Aura* held = caster->GetAura(shield, caster->GetGUID());
            AuraEffect* heldEff = held ? held->GetEffect(EFFECT_0) : nullptr;
            int64 const existing = heldEff ? std::max(0, heldEff->GetAmount()) : 0;
            int32 const amount = int32(LustLogic::OverhealShield(heal, missing, existing, cap));
            if (amount <= 0)
                continue;
            if (heldEff)
            {
                heldEff->ChangeAmount(amount);
                held->SetDuration(held->GetMaxDuration());
            }
            else
                caster->CastCustomSpell(caster, shield, &amount, nullptr, nullptr, true);
        }
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_ef_kit_overheal_shield::HandleHit);
    }
};

class spell_ef_kit_tally_hit : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_tally_hit);

    void AfterHitTarget()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        int32 const damage = GetHitDamage();
        if (!caster || !target || target == caster || damage <= 0)
            return;
        uint32 const id = GetSpellInfo()->Id;
        Tally* tally = caster->CustomData.GetDefault<Tally>(kTallyKey);
        tally->sums[id] += damage;
        // Vengeful Fury: value a hit, at most misc B a second
        for (AuraEffect const* rule : Emberfall::ClassKit::Rules(caster, "spell_ef_kit_rule_hit_gain", id))
        {
            auto& window = tally->windows[rule->GetId()];
            int32 const granted = LustLogic::CappedGain(window.first, window.second, NowMs(), rule->GetAmount(),
                                                        rule->GetMiscValueB());
            if (granted > 0)
                Emberfall::ClassKit::GainSpecPower(caster, granted, id);
        }
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_ef_kit_tally_hit::AfterHitTarget);
    }
};

class spell_ef_kit_tally_aura : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_tally_aura);

    uint32 Tallied() const
    {
        for (SpellEffectInfo const& eff : GetSpellInfo()->Effects)
            if (eff.TriggerSpell)
                return eff.TriggerSpell;
        return 0;
    }

    void Applied(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Unit* caster = GetCaster())
            if (uint32 const id = Tallied())
                caster->CustomData.GetDefault<Tally>(kTallyKey)->sums[id] = 0;
    }

    void Gone(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* caster = GetCaster();
        uint32 const id = Tallied();
        if (!caster || !id)
            return;
        Tally* tally = caster->CustomData.Get<Tally>(kTallyKey);
        int64 sum = 0;
        if (tally)
        {
            auto it = tally->sums.find(id);
            if (it != tally->sums.end())
            {
                sum = it->second;
                tally->sums.erase(it);
            }
        }
        if (GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_EXPIRE || sum <= 0 || !caster->IsAlive())
            return;
        // Final Reckoning: value percent of what it dealt, when it ran its full time
        for (AuraEffect const* rule : Emberfall::ClassKit::Rules(caster, "spell_ef_kit_rule_tally_wave", GetId()))
            if (uint32 const wave = TriggerOf(rule))
                if (int32 const bp = LustLogic::Pct(sum, rule->GetAmount()); bp > 0)
                    caster->CastCustomSpell(caster, wave, &bp, nullptr, nullptr, true);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_ef_kit_tally_aura::Applied, EFFECT_0, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_ef_kit_tally_aura::Gone, EFFECT_0, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

class spell_ef_kit_spend_rules : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_spend_rules);

    void Spent()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;
        int32 const scale = std::max(1, Emberfall::ClassKit::PowerScale(uint8(GetSpellInfo()->PowerType)));
        int32 const points = GetSpell()->GetPowerCost() / scale;
        if (points <= 0)
            return;
        // Bloodthirsty Berserker: value ms (negative) a point spent off the trigger's cooldown
        for (AuraEffect const* rule : Emberfall::ClassKit::Rules(caster, "spell_ef_kit_rule_spend_cut", GetSpellInfo()->Id))
            if (uint32 const trigger = TriggerOf(rule))
                if (int32 const cut = LustLogic::SpendCutMs(points, rule->GetAmount()); cut > 0)
                    Emberfall::ClassKit::ShortenCooldown(caster, trigger, cut);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_ef_kit_spend_rules::Spent);
    }
};

class spell_ef_kit_pull_in : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_pull_in);

    void HandlePull(SpellEffIndex effIndex)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || target == caster || target->IsPlayer() || !target->IsAlive()
            || Emberfall::ClassKit::IsBoss(target) || target->HasUnitState(UNIT_STATE_ROOT)
            || target->IsImmunedToSpellEffect(GetSpellInfo(), effIndex, caster))
            return;
        float const dist = caster->GetExactDist2d(target);
        if (float const reach = GetSpellInfo()->GetMaxRange(false); reach > 0.0f && dist > reach)
            return;
        SpellEffectInfo const& eff = GetSpellInfo()->Effects[effIndex];
        float const stopShort = float(std::max(1, eff.CalcValue(caster)));
        if (Emberfall::ClassKit::Flow::HookTravel(dist, stopShort) <= 0.5f)
            return;
        float const speed = float(std::max(1, eff.MiscValueB));
        Position const dest = caster->GetFirstCollisionPosition(stopShort, caster->GetRelativeAngle(target));
        target->GetMotionMaster()->MoveCharge(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), speed);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_ef_kit_pull_in::HandlePull, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

// Crippling Blow, Bloodbound Warlord: the bearer of a caster-ward debuff deals its caster less damage (the strongest).
class EmberfallLustUnitScript : public UnitScript
{
public:
    EmberfallLustUnitScript() : UnitScript("EmberfallLustUnitScript", true,
        { UNITHOOK_MODIFY_MELEE_DAMAGE, UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN, UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK }) { }

    static int32 WardPct(Unit* victim, Unit* attacker)
    {
        if (!victim || !attacker || attacker == victim)
            return 0;
        std::vector<int32> pcts;
        for (AuraEffect const* eff : attacker->GetAuraEffectsByType(SPELL_AURA_DUMMY))
            if (eff->GetCasterGUID() == victim->GetGUID() && eff->GetBase()->GetScriptByName("spell_ef_kit_caster_ward"))
                pcts.push_back(eff->GetAmount());
        return pcts.empty() ? 0 : LustLogic::StrongestWard(pcts);
    }

    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) override
    {
        if (int32 const pct = damage ? WardPct(target, attacker) : 0; pct > 0)
            damage = uint32(LustLogic::Warded(damage, pct));
    }

    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* /*spellInfo*/) override
    {
        if (int32 const pct = damage > 0 ? WardPct(target, attacker) : 0; pct > 0)
            damage = int32(LustLogic::Warded(damage, pct));
    }

    // the core calls this for periodic heals too: only damage ticks
    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage, SpellInfo const* spellInfo) override
    {
        if (spellInfo && spellInfo->HasAura(SPELL_AURA_PERIODIC_HEAL))
            return;
        if (int32 const pct = damage ? WardPct(target, attacker) : 0; pct > 0)
            damage = uint32(LustLogic::Warded(damage, pct));
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
    RegisterSpellScript(spell_ef_kit_overheal_shield);
    RegisterSpellScript(spell_ef_kit_tally_hit);
    RegisterSpellScript(spell_ef_kit_tally_aura);
    RegisterSpellScript(spell_ef_kit_spend_rules);
    RegisterSpellScript(spell_ef_kit_pull_in);
    // the rule markers, and the caster ward's marker (not a rule: a debuff on the enemy, read by the unit hook)
    for (char const* name : { "spell_ef_kit_rule_timed_stacks", "spell_ef_kit_rule_build_pct",
                              "spell_ef_kit_rule_overheal_shield", "spell_ef_kit_rule_hit_gain",
                              "spell_ef_kit_rule_tally_wave", "spell_ef_kit_rule_count_scale",
                              "spell_ef_kit_rule_stack_boost", "spell_ef_kit_rule_full_stacks",
                              "spell_ef_kit_rule_spend_cut", "spell_ef_kit_caster_ward" })
        RegisterSpellScriptWithArgs(spell_ef_kit_lust_rule, name);
    new EmberfallLustUnitScript();
}
