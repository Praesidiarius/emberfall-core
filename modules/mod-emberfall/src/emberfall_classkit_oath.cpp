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
//                                  Resolve; no effect 3: no cap; effect 3's misc B 1: its value in percent of the duration as
//                                  cast, the Stormbringer's Stormbrand); one aura, never a second (Bastion's Resolve on the
//                                  caster; the Executioner's Deathblow bleed on an enemy, +8 s an application)
//   spell_ef_kit_consume_aura      after the cast, one charge (or one stack) of the caster's aura the spell requires
//                                  (Spell.dbc CasterAuraSpell) is taken: Bastion's Resolve's charges, Echo Ready, the
//                                  Executioner's Deathblow window; also when every enemy avoided the strike (the miss
//                                  rules, alpha1 feedback #143: a missed, dodged or parried Deathblow spends its proc)
//   spell_ef_kit_add_charge        a dummy effect on the caster with a trigger: one more charge of that aura on the caster,
//                                  at most the effect's value; misc: a spell the caster must know (Aegis of Judgment's
//                                  shield and Solenne's Favor; Crimson Flow's proc). A second dummy of the spell without a
//                                  trigger is a bonus roll: with its misc known, value % for one charge more (Twin
//                                  Currents, Emberfall feedback #212)
//   spell_ef_kit_absorb_burst      a school absorb (effect 1) whose absorbed blows cast effect 2's trigger at the owner, at
//                                  most once in effect 2's value in ms (Solenne's Aegis's bursts)
//   spell_ef_kit_stack_refill      a passive's periodic dummy: every misc B seconds out of combat one of the owner's self stacks of
//                                  misc comes back, up to their cap (Shield Oath: Bastion's Resolve's charges, one per 20 s, #331)
//
// ankarna's update of 2026-11-01 (Emberfall paladin-oathkeeper.md §20):
//
//   spell_ef_kit_talent_gate       every trigger_spell effect whose misc names a spell does nothing unless the (player)
//                                  caster knows that spell: Solenne's Retribution's Solenne's Aegis and Judgment's Echo,
//                                  Solenne's Sanctuary's cleanse, each with its talent
//   spell_ef_kit_cast_override     a marker on an aura of the caster's: while it holds, a cast request (the client's
//                                  CMSG_CAST_SPELL) of effect 1's misc is cast as its trigger instead, from the same action
//                                  button (Echo Ready: the button of Solenne's Retribution casts Judgment's Echo; the
//                                  MiscScript emberfall_classkit_cast_override, the core's ValidateSpellAtCastSpell hook)
//   spell_ef_kit_max_mana_cost     the cost is effect 2's value in percent of the caster's MAXIMUM mana (spell mods in:
//                                  Efficient Mercy), effect 3's value in points less with an aura of effect 3's misc on the
//                                  caster (one of its charges goes with the cast: Solenne's Favor); refused without the mana; nothing for
//                                  a triggered cast (Divine Intervention). Spell.dbc's own cost stays 0 (Solenne's Mercy)
//   spell_ef_kit_low_health_proc   a proc_trigger_spell aura (the taken-damage procs; the spell_proc row's cooldown as its
//                                  internal cooldown) that procs only when the blow leaves its owner alive at or below its
//                                  value in percent of the maximum health, and then casts its trigger at the owner,
//                                  triggered (Divine Intervention: Solenne's Mercy at 20 %, free, every 4 min)

#include "EfClassKit.h"
#include "EfClassKitLogic.h"

#include "GameTime.h"
#include "MiscScript.h"
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
        // the cap: effect 3's seconds (spell mods in: Long Resolve), or with its misc B 1 its value in percent of the
        // duration as cast (the Stormbringer's Stormbrand: 130 %, a refresh window that follows Long Brand and Supercell's
        // doubling); no effect 3, no cap (the bleed)
        SpellEffectInfo const& capEff = GetSpellInfo()->Effects[EFFECT_2];
        int32 capMs = 0;
        if (capEff.Effect)
            capMs = capEff.MiscValueB == 1 ? Emberfall::ClassKit::Logic::PctCapMs(add, capEff.CalcValue(caster))
                                           : capEff.CalcValue(caster) * IN_MILLISECONDS;
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

// One charge (or stack) of the aura the spell requires of its caster is spent by the cast, struck or not (the miss rules,
// #143: a missed Deathblow spends its proc as a hit does; until then a strike every enemy avoided kept it).
class spell_ef_kit_consume_aura : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_consume_aura);

    void Consume()
    {
        Emberfall::ClassKit::ConsumeOne(GetCaster(), GetSpellInfo()->CasterAuraSpell);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_ef_kit_consume_aura::Consume);
    }
};

// Solenne's Favor, Crimson Flow: one more charge of the trigger aura on the caster, at most the effect's value (above the
// aura's own ProcCharges: the charges a fresh application starts with).
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
        int32 const cap = GetEffectValue();
        int32 gain = BonusRoll(caster, effIndex) ? 2 : 1;
        Aura* aura = caster->GetAura(eff.TriggerSpell, caster->GetGUID());
        if (!aura)
        {
            caster->CastSpell(caster, eff.TriggerSpell, true);   // a fresh buff: its ProcCharges (the first charge)
            if (gain < 2 || !(aura = caster->GetAura(eff.TriggerSpell, caster->GetGUID())))
                return;
            gain = 1;                                             // the bonus roll's charge on top
        }
        // SetCharges, not ModCharges: ModCharges clamps a gain to the charges of a fresh application (the aura's
        // ProcCharges + SPELLMOD_CHARGES: Crimson Flow's 1; Solenne's Favor's 1), so the cap in the effect's value was
        // never reached (Emberfall feedback #211).
        int32 const want = Emberfall::ClassKit::Logic::ChargesAfter(aura->GetCharges(), cap, gain);
        if (want > aura->GetCharges())
            aura->SetCharges(uint8(want));
        aura->RefreshDuration();
    }

    // The bonus roll (Twin Currents): another dummy of the spell without a trigger whose misc the caster knows; value %.
    bool BonusRoll(Unit* caster, SpellEffIndex charge)
    {
        Player* player = caster->ToPlayer();
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            SpellEffectInfo const& e = GetSpellInfo()->Effects[i];
            if (i == charge || e.Effect != SPELL_EFFECT_DUMMY || e.TriggerSpell || e.MiscValue <= 0)
                continue;
            if (!player || !player->HasSpell(uint32(e.MiscValue)))
                continue;
            return roll_chance_i(e.CalcValue(caster));
        }
        return false;
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

// Shield Oath: out of combat, one of the owner's self stacks of misc back every misc B seconds, up to their cap
// (Emberfall alpha1 feedback #331: slowly, one charge every 20 s, not all at once after 10 s).
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
        int32 const everyMs = GetSpellInfo()->Effects[aurEff->GetEffIndex()].MiscValueB * IN_MILLISECONDS;
        if (!stacks || everyMs <= 0)
            return;
        // already full: nothing, and the next charge's time starts only once one is spent
        if (Aura const* held = owner->GetAura(stacks, owner->GetGUID()))
            if (AuraEffect const* cap = held->GetEffect(EFFECT_0); cap && int32(held->GetStackAmount()) >= cap->GetAmount())
            {
                _outMs = 0;
                return;
            }
        _outMs += aurEff->GetAmplitude();
        if (int32(_outMs) < everyMs)
            return;
        _outMs = 0;
        Emberfall::ClassKit::AddSelfStacks(owner, stacks, 1);   // one more, the stack aura's own cap holds
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_ef_kit_stack_refill::Tick, EFFECT_ALL, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Solenne's Retribution and Sanctuary: a trigger_spell effect with misc = a talent comes only with that talent known.
class spell_ef_kit_talent_gate : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_talent_gate);

    void Gate(SpellEffIndex effIndex)
    {
        int32 const talent = GetSpellInfo()->Effects[effIndex].MiscValue;
        Unit* caster = GetCaster();
        Player* player = caster ? caster->ToPlayer() : nullptr;
        if (talent > 0 && player && !player->HasSpell(uint32(talent)))
            PreventHitDefaultEffect(effIndex);
    }

    void Register() override
    {
        // the core triggers in the launch modes (Spell::EffectTriggerSpell)
        OnEffectLaunch += SpellEffectFn(spell_ef_kit_talent_gate::Gate, EFFECT_ALL, SPELL_EFFECT_TRIGGER_SPELL);
        OnEffectLaunchTarget += SpellEffectFn(spell_ef_kit_talent_gate::Gate, EFFECT_ALL, SPELL_EFFECT_TRIGGER_SPELL);
    }
};

// The marker of a cast override (Echo Ready): nothing happens in it; emberfall_classkit_cast_override reads it.
class spell_ef_kit_cast_override : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_cast_override);

    void Register() override { }
};

namespace
{
    char const* const CAST_OVERRIDE = "spell_ef_kit_cast_override";

    // The spell a request for `spellId` is cast as: the trigger of a dummy of the player's own whose misc is it, on an
    // aura bound to spell_ef_kit_cast_override; 0: as asked.
    uint32 CastOverride(Player* player, uint32 spellId)
    {
        if (!player || !spellId)
            return 0;
        for (AuraEffect const* eff : player->GetAuraEffectsByType(SPELL_AURA_DUMMY))
        {
            if (uint32(eff->GetMiscValue()) != spellId || eff->GetCasterGUID() != player->GetGUID())
                continue;
            uint32 const instead = eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell;
            if (instead && instead != spellId && eff->GetBase()->GetScriptByName(CAST_OVERRIDE)
                && sSpellMgr->GetSpellInfo(instead))
                return instead;
        }
        return 0;
    }
}

// Judgment's Echo on Solenne's Retribution's own button: the request comes as the Retribution (its cooldown running) and
// is cast as the Echo. After the spellbook check (the player knows the Retribution), before the spell is made; a request
// the server's spell queue held comes through here again when it goes.
class emberfall_classkit_cast_override : public MiscScript
{
public:
    emberfall_classkit_cast_override() : MiscScript("emberfall_classkit_cast_override", { MISCHOOK_VALIDATE_SPELL_AT_CAST_SPELL }) { }

    void ValidateSpellAtCastSpell(Player* player, uint32& /*oldSpellId*/, uint32& spellId, uint8& /*castCount*/,
                                  uint8& /*castFlags*/) override
    {
        if (uint32 const instead = CastOverride(player, spellId))
            spellId = instead;
    }
};

// Solenne's Mercy: a share of the maximum mana, paid by the script (Spell.dbc's cost is a share of BASE mana).
class spell_ef_kit_max_mana_cost : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_max_mana_cost);

    // the mana this cast takes and the aura that made it cheaper (0: none)
    std::pair<int32, uint32> Price()
    {
        Unit* caster = GetCaster();
        if (!caster || GetSpell()->IsTriggered())
            return { 0, 0 };
        SpellEffectInfo const& share = GetSpellInfo()->Effects[EFFECT_1];
        SpellEffectInfo const& cheap = GetSpellInfo()->Effects[EFFECT_2];
        uint32 const favor = cheap.Effect == SPELL_EFFECT_DUMMY && cheap.MiscValue > 0
                                 && caster->HasAura(uint32(cheap.MiscValue), caster->GetGUID())
                             ? uint32(cheap.MiscValue) : 0;
        int32 const cost = Emberfall::ClassKit::Logic::MaxManaCost(int32(caster->GetMaxPower(POWER_MANA)),
                                                                   share.CalcValue(caster), favor != 0,
                                                                   cheap.Effect ? cheap.CalcValue(caster) : 0);
        return { cost, favor };
    }

    SpellCastResult Check()
    {
        Unit* caster = GetCaster();
        int32 const cost = Price().first;
        if (caster && cost > 0 && int32(caster->GetPower(POWER_MANA)) < cost)
            return SPELL_FAILED_NO_POWER;
        return SPELL_CAST_OK;
    }

    void Pay()
    {
        Unit* caster = GetCaster();
        auto const [cost, favor] = Price();
        if (!caster)
            return;
        if (cost > 0)
        {
            caster->ModifyPower(POWER_MANA, -cost);
            caster->SetLastManaUse(GameTime::GetGameTimeMS().count());   // the five-second rule, as Spell::TakePower
        }
        if (favor)
            Emberfall::ClassKit::ConsumeOne(caster, favor);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_ef_kit_max_mana_cost::Check);
        OnCast += SpellCastFn(spell_ef_kit_max_mana_cost::Pay);
    }
};

// Divine Intervention: at or below the value's percent of health after a blow, the trigger on the owner (the spell_proc
// row's cooldown: once in 4 min).
class spell_ef_kit_low_health_proc : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_low_health_proc);

    bool Check(ProcEventInfo& /*eventInfo*/)
    {
        Unit* owner = GetUnitOwner();
        AuraEffect const* eff = GetEffect(EFFECT_0);
        return owner && eff && owner->IsAlive()
               && Emberfall::ClassKit::Logic::LowHealthDue(owner->GetHealth(), owner->GetMaxHealth(), eff->GetAmount());
    }

    void Proc(AuraEffect const* aurEff, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();
        Unit* owner = GetUnitOwner();
        uint32 const trigger = GetSpellInfo()->Effects[aurEff->GetEffIndex()].TriggerSpell;
        if (owner && trigger)
            owner->CastSpell(owner, trigger, true);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_ef_kit_low_health_proc::Check);
        OnEffectProc += AuraEffectProcFn(spell_ef_kit_low_health_proc::Proc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
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
    RegisterSpellScript(spell_ef_kit_talent_gate);
    RegisterSpellScript(spell_ef_kit_cast_override);
    RegisterSpellScript(spell_ef_kit_max_mana_cost);
    RegisterSpellScript(spell_ef_kit_low_health_proc);
    new emberfall_classkit_cast_override();
}
