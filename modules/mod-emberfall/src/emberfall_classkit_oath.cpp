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

// Emberfall class kits, the Paladin's Oathkeeper (Emberfall docs/design/classes/paladin-oathkeeper.md, ankarna's cards
// as Nathanael decided them on 2026-10-23): the chain of cooldowns, Bastion's Resolve and its charges, Solenne's Aegis,
// Solenne's Favor, Judgment's Echo. Generic like the scripts of src/emberfall_classkit.cpp: each reads its numbers from
// the spell's own effects (Emberfall's classkit/model.py SCRIPTS checks the contracts).
//
//   spell_ef_kit_cooldown_cut_pct  every dummy effect on the caster with a trigger takes its value in percent of the
//                                  trigger's BASE cooldown (Spell.dbc's; ankarna's Q4 of 2026-10-23) off the caster's
//                                  running cooldown of it, never more than is left (100: all of it), misc B: that cooldown is
//                                  never cut below misc B seconds in all (EfClassKit.h CutCooldownPct); then the caster's
//                                  dummy auras whose misc is this spell and that have a trigger do the same with their
//                                  amount (a set bonus's extra link: the Sunward Bastion's 2 and 8 pieces). The chain:
//                                  Shieldbrand, the ricochet's bounce, Bastion Breaker's burst, Aegis of Judgment's wave;
//                                  Judgment's Echo's reset; Oathbound Rush's slow immunity for the Oathkeeper
//   spell_ef_kit_helper_links      the links of the spell's trigger_spell helpers (their dummies on the caster with a
//                                  trigger, as spell_ef_kit_cooldown_cut_pct reads them, spell mods in) are taken at the
//                                  cast, whether the helper lands or not: Bastion Breaker's link to Aegis of Judgment sits
//                                  on its burst, which a missed, dodged or parried blow never casts (Emberfall feedback
//                                  #47); the helper carries no cut script of its own, so a hit takes it once
//   spell_ef_kit_exclude_target    the spell's areas round its target leave the target out (Aegis Ricochet's bounces)
//   spell_ef_kit_capped_extend     a cast while the caster's own aura of the spell still runs on the unit it hits adds the
//                                  spell's duration to what is left, at most effect 3's value in seconds (spell mods in: Long
//                                  Resolve; no effect 3: no cap); one aura, never a second (Bastion's Resolve on the caster;
//                                  the Executioner's Deathblow bleed on an enemy, +8 s an application)
//   spell_ef_kit_consume_aura      after the cast, one charge (or one stack) of the caster's aura the spell requires
//                                  (Spell.dbc CasterAuraSpell) is taken: Bastion's Resolve's charges, Echo Ready, the
//                                  Executioner's Deathblow window; not when every enemy target avoided the strike
//   spell_ef_kit_add_charge        a dummy effect on the caster with a trigger: one more charge of that aura on the caster,
//                                  at most the effect's value; misc: a spell the caster must know (Aegis of Judgment's
//                                  shield and Solenne's Favor)
//   spell_ef_kit_absorb_burst      a school absorb (effect 1) whose absorbed blows cast effect 2's trigger at the owner, at
//                                  most once in effect 2's value in ms (Solenne's Aegis's bursts)
//   spell_ef_kit_stack_refill      a passive's periodic dummy: after misc B seconds out of combat the owner's self stacks of
//                                  misc are filled to their cap (Shield Oath: Bastion's Resolve's charges)

#include "EfClassKit.h"
#include "EfClassKitLogic.h"

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

// A link of the chain: a percent of the next ability's base cooldown off what is left of it.
class spell_ef_kit_cooldown_cut_pct : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_cooldown_cut_pct);

    bool _auraLinksDone = false;

    void HandleDummy(SpellEffIndex effIndex)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        SpellEffectInfo const& eff = GetSpellInfo()->Effects[effIndex];
        if (!caster || target != caster || !eff.TriggerSpell)
            return;
        Emberfall::ClassKit::CutCooldownPct(caster, eff.TriggerSpell, GetEffectValue(), eff.MiscValueB * IN_MILLISECONDS);
    }

    // the caster's auras that link this spell to another (a set bonus): once per cast
    void AuraLinks()
    {
        Unit* caster = GetCaster();
        if (!caster || _auraLinksDone)
            return;
        _auraLinksDone = true;
        uint32 const id = GetSpellInfo()->Id;
        std::vector<std::pair<uint32, int32>> links;
        for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
        {
            SpellEffectInfo const& info = eff->GetSpellInfo()->Effects[eff->GetEffIndex()];
            if (uint32(eff->GetMiscValue()) == id && info.TriggerSpell && eff->GetAmount() > 0)
                links.emplace_back(info.TriggerSpell, eff->GetAmount());
        }
        for (auto const& [spell, pct] : links)
            Emberfall::ClassKit::CutCooldownPct(caster, spell, pct);
    }

    void Register() override
    {
        // a spell with no link of its own (Bastion's Resolve: only the set's aura links) binds no effect hook
        bool links = false;
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(m_scriptSpellId))
            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                links = links || info->Effects[i].Effect == SPELL_EFFECT_DUMMY;
        if (links)
            OnEffectHitTarget += SpellEffectFn(spell_ef_kit_cooldown_cut_pct::HandleDummy, EFFECT_ALL, SPELL_EFFECT_DUMMY);
        AfterCast += SpellCastFn(spell_ef_kit_cooldown_cut_pct::AuraLinks);
    }
};

// Bastion Breaker: its burst's link to Aegis of Judgment at the cast, hit or miss (the burst only comes with a hit).
class spell_ef_kit_helper_links : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_helper_links);

    void Links()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            SpellEffectInfo const& eff = GetSpellInfo()->Effects[i];
            SpellInfo const* helper = eff.Effect == SPELL_EFFECT_TRIGGER_SPELL && eff.TriggerSpell
                ? sSpellMgr->GetSpellInfo(eff.TriggerSpell) : nullptr;
            if (!helper)
                continue;
            for (uint8 j = 0; j < MAX_SPELL_EFFECTS; ++j)
            {
                SpellEffectInfo const& link = helper->Effects[j];
                if (link.Effect != SPELL_EFFECT_DUMMY || !link.TriggerSpell || link.TargetA.GetTarget() != TARGET_UNIT_CASTER)
                    continue;
                // the helper's value with the caster's spell mods (Linked Oaths raises the burst's effect 2)
                Emberfall::ClassKit::CutCooldownPct(caster, link.TriggerSpell, link.CalcValue(caster),
                                                    link.MiscValueB * IN_MILLISECONDS);
            }
        }
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_ef_kit_helper_links::Links);
    }
};

// Aegis Ricochet's bounces: the first target (the spell's explicit one) is left out of its areas.
class spell_ef_kit_exclude_target : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_exclude_target);

    void FilterTargets(std::list<WorldObject*>& targets)
    {
        Unit* main = GetExplTargetUnit();
        targets.remove_if([main](WorldObject* object) { return object == main; });
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_ef_kit_exclude_target::FilterTargets, EFFECT_ALL, TARGET_UNIT_DEST_AREA_ENEMY);
    }
};

// Bastion's Resolve: each cast adds its duration to what is left of the one aura, at most effect 3's value in seconds.
class spell_ef_kit_capped_extend : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_capped_extend);

    int32 _leftMs = 0;

    // what is left of the caster's aura on the unit about to be hit (the caster itself: Bastion's Resolve; an enemy: the
    // Executioner's bleed); Before and After run for each unit in turn
    void BeforeHitTarget(SpellMissInfo /*missInfo*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        Aura const* aura = caster && target ? target->GetAura(GetSpellInfo()->Id, caster->GetGUID()) : nullptr;
        _leftMs = aura ? std::max(0, aura->GetDuration()) : 0;
    }

    void AfterHitTarget()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        int32 const left = _leftMs;
        _leftMs = 0;
        if (!caster || !target || left <= 0)
            return;
        Aura* aura = target->GetAura(GetSpellInfo()->Id, caster->GetGUID());
        if (!aura)
            return;
        int32 const add = aura->CalcMaxDuration(caster);
        // the cap: effect 3's seconds (spell mods in: Long Resolve); no effect 3, no cap (the bleed)
        int32 const capMs = GetSpellInfo()->Effects[EFFECT_2].Effect
            ? GetSpellInfo()->Effects[EFFECT_2].CalcValue(caster) * IN_MILLISECONDS : 0;
        int32 const total = Emberfall::ClassKit::Logic::ExtendCapped(left, add, capMs);
        aura->SetMaxDuration(std::max(total, add));
        aura->SetDuration(total);
    }

    void Register() override
    {
        BeforeHit += BeforeSpellHitFn(spell_ef_kit_capped_extend::BeforeHitTarget);
        AfterHit += SpellHitFn(spell_ef_kit_capped_extend::AfterHitTarget);
    }
};

// One charge (or stack) of the aura the spell requires of its caster is spent by the cast.
class spell_ef_kit_consume_aura : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_consume_aura);

    void Consume()
    {
        Unit* caster = GetCaster();
        uint32 const need = GetSpellInfo()->CasterAuraSpell;
        Aura* aura = caster && need ? caster->GetAura(need) : nullptr;
        if (!aura)
            return;
        // a strike that every enemy dodged, parried or evaded keeps it (the Executioner's Deathblow window, 2026-10-24)
        bool enemies = false, landed = false;
        for (TargetInfo const& info : *GetSpell()->GetUniqueTargetInfo())
        {
            if (info.targetGUID == caster->GetGUID())
                continue;
            enemies = true;
            landed = landed || info.missCondition == SPELL_MISS_NONE;
        }
        if (enemies && !landed)
            return;
        if (aura->IsUsingCharges() && aura->GetCharges() > 0)
            aura->DropCharge();
        else if (aura->GetStackAmount() > 1)
            aura->ModStackAmount(-1);
        else
            aura->Remove();
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_ef_kit_consume_aura::Consume);
    }
};

// Solenne's Favor: one more charge of the trigger aura on the caster, at most the effect's value.
class spell_ef_kit_add_charge : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_add_charge);

    void HandleDummy(SpellEffIndex effIndex)
    {
        Unit* caster = GetCaster();
        SpellEffectInfo const& eff = GetSpellInfo()->Effects[effIndex];
        if (!caster || GetHitUnit() != caster || !eff.TriggerSpell)
            return;
        if (eff.MiscValue > 0)
            if (Player* player = caster->ToPlayer(); player && !player->HasSpell(uint32(eff.MiscValue)))
                return;
        if (Aura* aura = caster->GetAura(eff.TriggerSpell, caster->GetGUID()))
        {
            int32 const want = Emberfall::ClassKit::Logic::ChargesAfter(aura->GetCharges(), GetEffectValue());
            if (want > aura->GetCharges())
                aura->ModCharges(want - aura->GetCharges());
            aura->RefreshDuration();
            return;
        }
        caster->CastSpell(caster, eff.TriggerSpell, true);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_ef_kit_add_charge::HandleDummy, EFFECT_ALL, SPELL_EFFECT_DUMMY);
    }
};

// Solenne's Aegis: every blow it absorbs bursts at the enemies round its owner, at most once in effect 2's value in ms.
class spell_ef_kit_absorb_burst : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_absorb_burst);

    uint64 _lastMs = 0;

    void AfterAbsorb(AuraEffect* /*aurEff*/, DamageInfo& /*dmgInfo*/, uint32& absorbAmount)
    {
        Unit* owner = GetUnitOwner();
        SpellEffectInfo const& burst = GetSpellInfo()->Effects[EFFECT_1];
        if (!owner || !owner->IsAlive() || !absorbAmount || !burst.TriggerSpell)
            return;
        AuraEffect const* gap = GetEffect(EFFECT_1);
        uint64 const now = uint64(GameTime::GetGameTimeMS().count());
        if (!Emberfall::ClassKit::Logic::BurstDue(now, _lastMs, gap ? gap->GetAmount() : 1000))
            return;
        _lastMs = now;
        Unit* caster = GetCaster();
        (caster ? caster : owner)->CastSpell(owner, burst.TriggerSpell, true);
    }

    void Register() override
    {
        AfterEffectAbsorb += AuraEffectAbsorbFn(spell_ef_kit_absorb_burst::AfterAbsorb, EFFECT_0);
    }
};

// Shield Oath: out of combat for misc B seconds, the owner's self stacks of misc are filled to their cap.
class spell_ef_kit_stack_refill : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_stack_refill);

    uint32 _outMs = 0;

    void Tick(AuraEffect const* aurEff)
    {
        Unit* owner = GetUnitOwner();
        if (!owner || !owner->IsAlive() || owner->IsInCombat())
        {
            _outMs = 0;
            return;
        }
        uint32 const stacks = uint32(aurEff->GetMiscValue());
        int32 const afterMs = GetSpellInfo()->Effects[aurEff->GetEffIndex()].MiscValueB * IN_MILLISECONDS;
        _outMs += aurEff->GetAmplitude();
        if (!stacks || int32(_outMs) < afterMs)
            return;
        // already full: nothing (no refresh every second)
        if (Aura const* held = owner->GetAura(stacks, owner->GetGUID()))
            if (AuraEffect const* cap = held->GetEffect(EFFECT_0); cap && int32(held->GetStackAmount()) >= cap->GetAmount())
                return;
        // the cap is the stack aura's own (effect 1, spell mods in): ask for more than it holds
        Emberfall::ClassKit::AddSelfStacks(owner, stacks, 255);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_ef_kit_stack_refill::Tick, EFFECT_ALL, SPELL_AURA_PERIODIC_DUMMY);
    }
};

void AddEmberfallClassKitOathScripts()
{
    RegisterSpellScript(spell_ef_kit_cooldown_cut_pct);
    RegisterSpellScript(spell_ef_kit_helper_links);
    RegisterSpellScript(spell_ef_kit_exclude_target);
    RegisterSpellScript(spell_ef_kit_capped_extend);
    RegisterSpellScript(spell_ef_kit_consume_aura);
    RegisterSpellScript(spell_ef_kit_add_charge);
    RegisterSpellScript(spell_ef_kit_absorb_burst);
    RegisterSpellScript(spell_ef_kit_stack_refill);
}
