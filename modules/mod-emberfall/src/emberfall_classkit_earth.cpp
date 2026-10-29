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

// Emberfall class kits, the Druid's Earthwarden (Emberfall docs/design/classes/druid-earthwarden.md: ankarna's tank
// v1.0-v1.2, 2026-10-28). Generic like the scripts of src/emberfall_classkit.cpp: each reads its numbers from the
// spell's own effects and the caster's talent rules (Emberfall's classkit/scripts_earth.py checks the contracts):
//
//   spell_ef_kit_form          a form toggled on one button (Stone Elemental Form): effect 1 a `transform` aura (misc =
//                              the creature of growth stage 0; stage N is creature misc + N), effect 3 a periodic dummy
//                              (misc = an aura the owner loses with the form: Stone Shell). A cast while the form holds
//                              ends it instead (the cast is refused silently). The caster's passive dummies naming the
//                              form: a trigger is a rider aura kept on the owner while the form holds (Unyielding Earth,
//                              Earthen Regeneration, Unyielding Mountain), misc B the growth stages it adds (purely the
//                              look: each stage a creature with a larger display). The form's own other effects (effect
//                              2: the armour) follow the talents' spell mods on every sync (Unyielding Mountain's +50
//                              points on the armour, one sum with the form's +200 %: never a second percent aura, which
//                              the core multiplies). The form ends when its spell is no longer known (a spec switch)
//   spell_ef_kit_strike_channel a channel that strikes at its end (Mountainbreaker): effect 1 a dummy aura at the enemy
//                              whose trigger is the strike; when that aura runs out (the channel completed: not when the
//                              target died, left the channel's reach or the channel was broken) the strike is cast at the
//                              enemy if it is alive and within the spell's range. Effect 2 a trigger_spell on the caster:
//                              the channel's mark (a positive aura the spec's other abilities exclude), taken off when the
//                              channel ends however it ends. Never exclude the channel spell itself: it is negative (its
//                              aura at the enemy), and the core casts a negative ExcludeCasterAuraSpell on the targets of
//                              every family spell that names it (Spell::PrepareTriggersExecutedOnHit, Forbearance's
//                              rule): feedback #125, Stomp and Rockslide set off a Mountainbreaker on what they hit
//   spell_ef_kit_strike        the strike (Mountainbreaker's hit): a dummy on the caster with a trigger heals the caster
//                              its value in percent of max health (the trigger a heal cast with the amount; misc = the
//                              shell for Overflowing Stone); the rules naming the strike: _heal_low (value percentage
//                              points more below misc B % health: Deep Restoration), _heal_during (value % more while the
//                              caster holds the trigger aura: Relentless Fury), _overflow (at full health the heal goes
//                              into the shell named by the strike's misc, which must hold: Overflowing Stone),
//                              _heal_stacks (an aura of stacks on the caster, its dummy naming the strike: value % more
//                              healing a stack, all its stacks spent by the strike that lands: Mountain's Renewal, whose
//                              stacks Headbutt's and Stomp's hit bonuses lay),
//                              _quake_during (while the caster holds the aura of the passive's other dummy, the trigger is
//                              cast at the enemy's feet: Fury of the Earth), _every_nth (every value-th strike casts the
//                              trigger at the enemy, free and at once: Titan's Retribution)
//   spell_ef_kit_absorb_reflect an absorb (Stone Shell): the caster's rules _reflect naming it add their value in percent;
//                              the one with a trigger casts it at the attacker with that share of what was absorbed
//                              (Reflective Stone, Reflective Fury)
//   spell_ef_kit_drop_field    dummies on the caster with a trigger: the trigger (a ground field) is cast at the caster's
//                              feet; one whose misc names an aura the caster held as the cast went off replaces the plain
//                              one (Stomp's earthquake; Worldbreaker's empowered one)
//   spell_ef_kit_field_pull    a ground field's periodic dummy (effect 2): while the caster knows its misc (Gravitational
//                              Tremors) each tick moves every creature in the field (not a boss, not rooted) up to value
//                              yards towards the field's centre
//   spell_ef_kit_roll          a steerable roll (Rolling Boulder): effect 1 a speed aura, effect 2 a periodic dummy (value =
//                              the yards it rolls, misc = the contact strike, misc B = the contact reach), effect 3 a
//                              dummy (misc = the talent that gives charges, trigger = the charge aura, value = charges).
//                              Every enemy within reach is struck once a roll; the rules _momentum (value % faster each
//                              misc B yards: Gathering Momentum) and _end_wave (the trigger cast as the roll ends: Seismic
//                              Impact). A cast while rolling stops it; the cooldown counts from the start. Effect 3's
//                              misc B: a creature whose display the roller shows while it rolls (the boulder), the look
//                              of before (the form's growth stage) back when it ends
//   spell_ef_kit_rule_self_heal a rider aura (dummy, value = percent): every heal its owner casts on itself is raised by it
//                              (Earthen Regeneration while the form holds; the unit hook ModifyHealReceived)
//   spell_ef_kit_rule_*        the rules' own aura scripts (markers; src/emberfall_classkit_exec.cpp's pattern)

#include "EfClassKit.h"
#include "EfClassKitLogic.h"
#include "EfEarthLogic.h"

#include "CellImpl.h"
#include "Creature.h"
#include "DataMap.h"
#include "DynamicObject.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
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
#include "WorldPacket.h"

#include <algorithm>
#include <cmath>
#include <list>
#include <set>
#include <vector>

namespace
{
    namespace Earth = Emberfall::ClassKit::Earth;
    using Emberfall::ClassKit::Rules;

    uint64 NowMs()
    {
        return uint64(GameTime::GetGameTimeMS().count());
    }

    uint32 TriggerOf(AuraEffect const* eff)
    {
        return eff ? eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell : 0;
    }

    // Puts the player's cooldown of `spell` at `ms` left (the server's and the client's); 0: ready.
    void SetCooldown(Player* player, uint32 spell, int32 ms)
    {
        player->RemoveSpellCooldown(spell, true);
        if (ms <= 0)
            return;
        player->AddSpellCooldown(spell, 0, uint32(ms));
        WorldPacket data(SMSG_SPELL_COOLDOWN, 8 + 1 + 4 + 4);
        data << player->GetGUID();
        data << uint8(0);
        data << uint32(spell);
        data << uint32(ms);
        player->SendDirectMessage(&data);
    }

    // The passive dummy effects of `owner` naming `spell` (misc), rules excluded or not.
    std::vector<AuraEffect const*> Naming(Unit* owner, uint32 spell)
    {
        std::vector<AuraEffect const*> out;
        for (AuraEffect const* eff : owner->GetAuraEffectsByType(SPELL_AURA_DUMMY))
            if (uint32(eff->GetMiscValue()) == spell && eff->GetBase()->IsPassive())
                out.push_back(eff);
        return out;
    }

    // The owner's self-heal rules (Earthen Regeneration's ward while the form holds): their values in percent, added
    // to every heal the owner casts on itself (EmberfallEarthUnitScript::ModifyHealReceived).
    int32 SelfHealPct(Unit* owner)
    {
        int32 pct = 0;
        for (AuraEffect const* eff : owner->GetAuraEffectsByType(SPELL_AURA_DUMMY))
            if (eff->GetBase()->GetScriptByName("spell_ef_kit_rule_self_heal"))
                pct += eff->GetAmount();
        return pct;
    }

    // ---- the form ---------------------------------------------------------------------------------------------------------

    struct FormState
    {
        std::vector<uint32> riders;
        int32 stage = 0;
    };

    // What the owner's talents ask of the form now: the rider auras and the growth stage.
    FormState Wanted(Unit* owner, uint32 form)
    {
        FormState s;
        int32 points = 0;
        for (AuraEffect const* eff : Naming(owner, form))
        {
            if (uint32 const t = TriggerOf(eff))
                s.riders.push_back(t);
            points += std::max(0, eff->GetMiscValueB());
        }
        s.stage = Earth::GrowthStage(points, 3);
        return s;
    }

    void ShowStage(Unit* owner, int32 baseCreature, int32 stage)
    {
        if (baseCreature <= 0)
            return;
        CreatureTemplate const* ci = sObjectMgr->GetCreatureTemplate(uint32(baseCreature + stage));
        if (!ci)
            ci = sObjectMgr->GetCreatureTemplate(uint32(baseCreature));
        if (!ci)
            return;
        if (CreatureModel const* model = ObjectMgr::ChooseDisplayId(ci); model && model->CreatureDisplayID)
            owner->SetDisplayId(model->CreatureDisplayID);
    }
}

class spell_ef_kit_form : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_form);

    // pressed again while the form holds: the form ends, nothing is cast
    SpellCastResult CheckToggle()
    {
        Unit* caster = GetCaster();
        if (caster && caster->HasAura(GetSpellInfo()->Id, caster->GetGUID()))
        {
            caster->RemoveAurasDueToSpell(GetSpellInfo()->Id, caster->GetGUID());
            return SPELL_FAILED_DONT_REPORT;
        }
        return SPELL_CAST_OK;
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_ef_kit_form::CheckToggle);
    }
};

class spell_ef_kit_form_aura : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_form_aura);

    std::vector<uint32> _riders;
    int32 _stage = 0;

    void Sync(bool force)
    {
        Unit* owner = GetUnitOwner();
        if (!owner)
            return;
        FormState const want = Wanted(owner, GetId());
        for (uint32 rider : _riders)
            if (std::find(want.riders.begin(), want.riders.end(), rider) == want.riders.end())
                owner->RemoveAurasDueToSpell(rider, owner->GetGUID());
        for (uint32 rider : want.riders)
            if (!owner->HasAura(rider, owner->GetGUID()))
                owner->CastSpell(owner, rider, true);
        _riders = want.riders;
        FollowSpellMods(owner);
        if (force || want.stage != _stage)
        {
            _stage = want.stage;
            if (_stage > 0 || !force)
                ShowStage(owner, GetSpellInfo()->Effects[EFFECT_0].MiscValue, _stage);
        }
    }

    // the form's other effects (effect 2, the armour) with the spell mods the owner has now: a talent bought or reset in
    // the form counts at once (Unyielding Mountain's +50 points on the armour)
    void FollowSpellMods(Unit* owner)
    {
        AuraEffect* eff = GetEffect(EFFECT_1);
        if (!eff || !eff->CanBeRecalculated())
            return;
        if (int32 const want = eff->CalculateAmount(owner); want != eff->GetAmount())
            eff->ChangeAmount(want);
    }

    void Apply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Sync(true);
        Emberfall::ClassKit::RefreshConditionals(GetUnitOwner());
    }

    void Gone(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* owner = GetUnitOwner();
        if (!owner)
            return;
        for (uint32 rider : _riders)
            owner->RemoveAurasDueToSpell(rider, owner->GetGUID());
        for (AuraEffect const* eff : Naming(owner, GetId()))
            if (uint32 const t = TriggerOf(eff))
                owner->RemoveAurasDueToSpell(t, owner->GetGUID());
        _riders.clear();
        // what goes with the form (Stone Shell: its absorption is lost, never stored)
        if (int32 const strip = GetSpellInfo()->Effects[EFFECT_2].MiscValue; strip > 0)
            owner->RemoveAurasDueToSpell(uint32(strip));
        Emberfall::ClassKit::RefreshConditionals(owner);
    }

    // every tick: the form ends with its spell (a spec switch); talents bought or reset in the form follow
    void Tick(AuraEffect const* /*aurEff*/)
    {
        Player* player = GetUnitOwner() ? GetUnitOwner()->ToPlayer() : nullptr;
        if (player && !player->HasSpell(GetId()))
        {
            Remove(AURA_REMOVE_BY_CANCEL);
            return;
        }
        Sync(false);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_ef_kit_form_aura::Apply, EFFECT_0, SPELL_AURA_TRANSFORM, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_ef_kit_form_aura::Gone, EFFECT_0, SPELL_AURA_TRANSFORM, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_ef_kit_form_aura::Tick, EFFECT_2, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// ---- Mountainbreaker: the channel and its strike ------------------------------------------------------------------------

class spell_ef_kit_strike_channel : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_strike_channel);

    void RanOut(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
    {
        Unit* caster = GetCaster();
        // the channel's mark on the caster (effect 2, a trigger_spell on the caster: what the spec's other abilities
        // exclude while it channels) goes with the channel, however it ends
        if (SpellEffectInfo const& mark = GetSpellInfo()->Effects[EFFECT_1];
            caster && mark.Effect == SPELL_EFFECT_TRIGGER_SPELL && mark.TriggerSpell)
            caster->RemoveAurasDueToSpell(mark.TriggerSpell, caster->GetGUID());
        // only a channel that ran its full time strikes: death, leaving the reach and a broken channel end it otherwise
        if (GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_EXPIRE)
            return;
        Unit* target = GetTarget();
        uint32 const strike = GetSpellInfo()->Effects[aurEff->GetEffIndex()].TriggerSpell;
        if (!caster || !target || !strike || !caster->IsAlive() || !target->IsAlive() || target == caster)
            return;
        float const range = GetSpellInfo()->GetMaxRange(false, caster);
        if (!caster->IsWithinDistInMap(target, range))
            return;
        caster->CastSpell(target, strike, true);
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(spell_ef_kit_strike_channel::RanOut, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

namespace
{
    // Titan's Retribution's counter, per caster (kept until logout: the design leaves its persistence open).
    struct StrikeCount : public DataMap::Base
    {
        uint32 count = 0;
    };

    constexpr char const* kStrikeCountKey = "ef_earth_strike_count";
}

class spell_ef_kit_strike : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_strike);

    void AfterHitTarget()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || target == caster)
            return;
        uint32 const id = GetSpellInfo()->Id;
        Heal(caster);
        // Mountain's Renewal: the strike spends every stack (after its heal read them); a missed one in Finish
        SpendHealStacks(caster);
        // Fury of the Earth: an earthquake under the enemy while the fury holds
        for (AuraEffect const* eff : Rules(caster, "spell_ef_kit_rule_quake_during", id))
        {
            uint32 const quake = TriggerOf(eff);
            uint32 needs = 0;
            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                if (i != eff->GetEffIndex() && eff->GetSpellInfo()->Effects[i].Effect && eff->GetSpellInfo()->Effects[i].MiscValue > 0)
                    needs = uint32(eff->GetSpellInfo()->Effects[i].MiscValue);
            if (quake && (!needs || caster->HasAura(needs)))
                caster->CastSpell(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(), quake, true);
        }
        // Titan's Retribution: every value-th strike, the trigger at the enemy, free and at once
        for (AuraEffect const* eff : Rules(caster, "spell_ef_kit_rule_every_nth", id))
        {
            uint32 const trigger = TriggerOf(eff);
            if (!trigger)
                continue;
            StrikeCount* c = caster->CustomData.GetDefault<StrikeCount>(kStrikeCountKey);
            Earth::Count const next = Earth::EveryNth(c->count, eff->GetAmount());
            c->count = next.count;
            if (next.fires && target->IsAlive())
                caster->CastSpell(target, trigger, TRIGGERED_FULL_MASK);
            break;
        }
    }

    // the heal: the strike's own percent, the low-health ranks, the stacks it spends, the fury's share; at full health
    // into the shell
    void Heal(Unit* caster)
    {
        uint32 const id = GetSpellInfo()->Id;
        SpellEffectInfo const* heal = nullptr;
        for (SpellEffectInfo const& eff : GetSpellInfo()->Effects)
            if (eff.Effect == SPELL_EFFECT_DUMMY && eff.TargetA.GetTarget() == TARGET_UNIT_CASTER && eff.TriggerSpell)
                heal = &eff;
        if (!heal)
            return;
        float const healthPct = caster->GetHealthPct();
        int32 lowAt = 0, lowBonus = 0, during = 0, stacks = 0;
        for (AuraEffect const* eff : Rules(caster, "spell_ef_kit_rule_heal_low", id))
        {
            lowAt = std::max(lowAt, eff->GetMiscValueB());
            lowBonus += eff->GetAmount();
        }
        for (AuraEffect const* eff : Rules(caster, "spell_ef_kit_rule_heal_during", id))
            if (uint32 const aura = TriggerOf(eff); aura && caster->HasAura(aura))
                during += eff->GetAmount();
        // Mountain's Renewal: the stack aura's dummy, its amount already times its stacks (100 a stack)
        for (AuraEffect const* eff : Rules(caster, "spell_ef_kit_rule_heal_stacks", id))
            if (!eff->GetBase()->IsPassive())
                stacks += eff->GetAmount();
        int32 const pct = Earth::HealPct(heal->CalcValue(caster), healthPct, lowAt, lowBonus, during, stacks);
        int32 amount = int32(Earth::HealAmount(int64(caster->GetMaxHealth()), pct));
        if (amount <= 0)
            return;
        // Overflowing Stone: at full health the healing becomes absorption of the shell that already holds
        if (caster->IsFullHealth() && heal->MiscValue > 0 && !Rules(caster, "spell_ef_kit_rule_overflow", id).empty())
        {
            Aura* shell = caster->GetAura(uint32(heal->MiscValue), caster->GetGUID());
            AuraEffect* pool = shell ? shell->GetEffect(EFFECT_0) : nullptr;
            if (pool)
            {
                SpellEffectInfo const& rule = shell->GetSpellInfo()->Effects[EFFECT_1];
                int64 const cap = int64(caster->CountPctFromMaxHealth(rule.CalcValue(caster)));
                // the healing-done auras and the self-heal rules (Earthen Regeneration) count as for the heal
                int64 const add = Earth::SelfHealed(int64(float(amount) * caster->GetTotalAuraMultiplier(
                                                        SPELL_AURA_MOD_HEALING_DONE_PERCENT)), SelfHealPct(caster));
                pool->ChangeAmount(int32(Earth::Overflow(pool->GetAmount(), add, cap)));
                return;
            }
        }
        caster->CastCustomSpell(caster, heal->TriggerSpell, &amount, nullptr, nullptr, true);
    }

    // Mountain's Renewal's stacks go with the strike, landed or not (the miss rules, alpha1 feedback #143); a channel cut
    // short never casts the strike and keeps them
    void SpendHealStacks(Unit* caster)
    {
        for (AuraEffect const* eff : Rules(caster, "spell_ef_kit_rule_heal_stacks", GetSpellInfo()->Id))
            if (Aura* stacks = eff->GetBase(); stacks && !stacks->IsPassive() && !stacks->IsRemoved())
                stacks->Remove();
    }

    void Finish()
    {
        if (Unit* caster = GetCaster())
            SpendHealStacks(caster);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_ef_kit_strike::AfterHitTarget);
        AfterCast += SpellCastFn(spell_ef_kit_strike::Finish);
    }
};

// ---- Stone Shell's reflection -------------------------------------------------------------------------------------------

class spell_ef_kit_absorb_reflect : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_absorb_reflect);

    void AfterAbsorb(AuraEffect* /*aurEff*/, DamageInfo& dmgInfo, uint32& absorbAmount)
    {
        Unit* owner = GetUnitOwner();
        Unit* attacker = dmgInfo.GetAttacker();
        if (!owner || !attacker || attacker == owner || !attacker->IsAlive() || !absorbAmount)
            return;
        int32 pct = 0;
        uint32 trigger = 0;
        for (AuraEffect const* eff : Rules(owner, "spell_ef_kit_rule_reflect", GetId()))
        {
            pct += eff->GetAmount();
            if (uint32 const t = TriggerOf(eff))
                trigger = t;
        }
        if (!trigger)
            return;
        int32 back = Earth::Reflected(int32(absorbAmount), pct);
        if (back > 0)
            owner->CastCustomSpell(attacker, trigger, &back, nullptr, nullptr, true);
    }

    void Register() override
    {
        AfterEffectAbsorb += AuraEffectAbsorbFn(spell_ef_kit_absorb_reflect::AfterAbsorb, EFFECT_0);
    }
};

// ---- Stomp's earthquake -------------------------------------------------------------------------------------------------

class spell_ef_kit_drop_field : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_drop_field);

    uint32 _field = 0;

    // before the cast takes the charges of the auras it used (Worldbreaker's buff): which field this cast drops
    void Choose()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;
        uint32 plain = 0, alt = 0;
        for (SpellEffectInfo const& eff : GetSpellInfo()->Effects)
        {
            if (eff.Effect != SPELL_EFFECT_DUMMY || eff.TargetA.GetTarget() != TARGET_UNIT_CASTER || !eff.TriggerSpell)
                continue;
            if (eff.MiscValue > 0)
            {
                if (caster->HasAura(uint32(eff.MiscValue)))
                    alt = eff.TriggerSpell;
            }
            else
                plain = eff.TriggerSpell;
        }
        _field = alt ? alt : plain;
    }

    void Drop()
    {
        Unit* caster = GetCaster();
        if (caster && _field)
            caster->CastSpell(caster->GetPositionX(), caster->GetPositionY(), caster->GetPositionZ(), _field, true);
    }

    void Register() override
    {
        BeforeCast += SpellCastFn(spell_ef_kit_drop_field::Choose);
        AfterCast += SpellCastFn(spell_ef_kit_drop_field::Drop);
    }
};

class spell_ef_kit_field_pull : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_field_pull);

    void Tick(AuraEffect const* aurEff)
    {
        Unit* caster = GetCaster();
        Unit* target = GetTarget();
        if (!caster || !target || !target->IsAlive() || target == caster)
            return;
        int32 const talent = GetSpellInfo()->Effects[aurEff->GetEffIndex()].MiscValue;
        if (talent > 0 && !caster->HasAura(uint32(talent)))
            return;
        Creature* creature = target->ToCreature();
        if (!creature || Emberfall::ClassKit::IsBoss(creature) || creature->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED)
            || creature->IsImmunedToSpellEffect(GetSpellInfo(), aurEff->GetEffIndex()))
            return;
        if (GetAura()->GetType() != DYNOBJ_AURA_TYPE)
            return;
        DynamicObject const* field = GetAura()->GetDynobjOwner();
        if (!field)
            return;
        float const dx = field->GetPositionX() - target->GetPositionX();
        float const dy = field->GetPositionY() - target->GetPositionY();
        float const distance = std::sqrt(dx * dx + dy * dy);
        float const step = Earth::PullStep(distance, float(std::max(0, aurEff->GetAmount())), 1.5f);
        if (step <= 0.0f)
            return;
        float const x = target->GetPositionX() + dx / distance * step;
        float const y = target->GetPositionY() + dy / distance * step;
        float z = target->GetPositionZ();
        target->UpdateAllowedPositionZ(x, y, z);
        // a slow slide over most of the second, then the creature goes back to what it was doing
        creature->GetMotionMaster()->MoveCharge(x, y, z, std::max(1.0f, step / 0.8f));
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_ef_kit_field_pull::Tick, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// ---- Rolling Boulder ----------------------------------------------------------------------------------------------------

namespace
{
    // The roll's cooldown when it ends: with the charges (Living Avalanche) what the charge aura says, else the spell's
    // cooldown (spell mods in) less the time it has run since it started.
    void RestoreRollCooldown(Player* player, SpellInfo const* roll, uint64 startedMs)
    {
        for (SpellEffectInfo const& eff : roll->Effects)
        {
            if (eff.Effect != SPELL_EFFECT_DUMMY || !eff.TriggerSpell || eff.MiscValue <= 0)
                continue;
            if (!player->HasAura(uint32(eff.MiscValue)))
                break;
            Aura const* charges = player->GetAura(eff.TriggerSpell, player->GetGUID());
            int32 const max = std::max(1, eff.CalcValue(player));
            int32 const recharging = charges ? int32(charges->GetStackAmount()) : 0;
            SetCooldown(player, roll->Id, recharging >= max && charges ? charges->GetDuration() : 0);
            return;
        }
        int32 ms = int32(std::max(roll->RecoveryTime, roll->CategoryRecoveryTime));
        player->ApplySpellMod(roll->Id, SPELLMOD_COOLDOWN, ms);
        int64 const left = int64(ms) - int64(NowMs() - startedMs);
        SetCooldown(player, roll->Id, int32(std::max<int64>(0, left)));
    }

    struct RollStart : public DataMap::Base
    {
        uint64 ms = 0;
    };

    constexpr char const* kRollKey = "ef_earth_roll";
}

class spell_ef_kit_roll : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_roll);

    // pressed while rolling: the roll stops (its end does the rest), nothing is cast
    SpellCastResult CheckStop()
    {
        Unit* caster = GetCaster();
        if (caster && caster->HasAura(GetSpellInfo()->Id, caster->GetGUID()))
        {
            caster->RemoveAurasDueToSpell(GetSpellInfo()->Id, caster->GetGUID());
            return SPELL_FAILED_DONT_REPORT;
        }
        return SPELL_CAST_OK;
    }

    void Started()
    {
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!player)
            return;
        player->CustomData.GetDefault<RollStart>(kRollKey)->ms = NowMs();
        for (SpellEffectInfo const& eff : GetSpellInfo()->Effects)
            if (eff.Effect == SPELL_EFFECT_DUMMY && eff.TriggerSpell && eff.MiscValue > 0 && player->HasAura(uint32(eff.MiscValue)))
                Emberfall::ClassKit::SpendCharge(player, GetSpellInfo()->Id, eff.TriggerSpell, uint32(std::max(0, eff.CalcValue(player))));
        // ready again at once, so a second press can stop the roll; its end puts the cooldown back
        player->RemoveSpellCooldown(GetSpellInfo()->Id, true);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_ef_kit_roll::CheckStop);
        AfterCast += SpellCastFn(spell_ef_kit_roll::Started);
    }
};

class spell_ef_kit_roll_aura : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_roll_aura);

    float _x = 0.0f, _y = 0.0f;
    float _travelled = 0.0f;
    std::set<ObjectGuid> _struck;
    uint32 _formDisplay = 0;   // the look before the roll (the form's growth stage), back when it ends

    void Apply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Unit* owner = GetUnitOwner())
        {
            _x = owner->GetPositionX();
            _y = owner->GetPositionY();
            // the roller's look (effect 3's misc B: a creature whose display it takes while it rolls, the boulder)
            if (int32 const look = GetSpellInfo()->Effects[EFFECT_2].MiscValueB; look > 0)
            {
                _formDisplay = owner->GetDisplayId();
                ShowStage(owner, look, 0);
            }
        }
        _travelled = 0.0f;
        _struck.clear();
    }

    void Tick(AuraEffect const* aurEff)
    {
        Unit* owner = GetUnitOwner();
        if (!owner || !owner->IsAlive())
            return;
        float const dx = owner->GetPositionX() - _x;
        float const dy = owner->GetPositionY() - _y;
        _x = owner->GetPositionX();
        _y = owner->GetPositionY();
        // the server hears a player's position only with its movement heartbeats (half a second apart), so at the roll's
        // top speed (220 %, 15 yd a second) the reach ran 7 yd long: while the roller moves the distance is its speed
        // times the tick, the positions only when it does not (pushed, pulled)
        if (owner->isMoving())
            _travelled += owner->GetSpeed(MOVE_RUN) * float(aurEff->GetAmplitude()) / 1000.0f;
        else
            _travelled += std::sqrt(dx * dx + dy * dy);
        // Gathering Momentum: faster every misc B yards
        if (AuraEffect* speed = GetAura()->GetEffect(EFFECT_0))
        {
            int32 const base = GetSpellInfo()->Effects[EFFECT_0].CalcValue(owner);
            int32 want = base;
            for (AuraEffect const* eff : Rules(owner, "spell_ef_kit_rule_momentum", GetId()))
                want = std::max(want, Earth::RollSpeed(base, _travelled, eff->GetAmount(), eff->GetMiscValueB()));
            if (want != speed->GetAmount())
                speed->ChangeAmount(want);
        }
        // every enemy within reach, once a roll
        SpellEffectInfo const& rule = GetSpellInfo()->Effects[aurEff->GetEffIndex()];
        uint32 const contact = uint32(std::max(0, rule.MiscValue));
        float const reach = float(std::max(1, rule.MiscValueB));
        if (contact)
        {
            std::list<Unit*> near;
            Acore::AnyUnfriendlyUnitInObjectRangeCheck check(owner, owner, reach);
            Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(owner, near, check);
            Cell::VisitObjects(owner, searcher, reach);
            for (Unit* unit : near)
            {
                if (!unit->IsAlive() || !owner->IsValidAttackTarget(unit) || _struck.count(unit->GetGUID()))
                    continue;
                _struck.insert(unit->GetGUID());
                owner->CastSpell(unit, contact, true);
            }
        }
        if (Earth::RollDone(_travelled, aurEff->GetAmount()))
            Remove(AURA_REMOVE_BY_EXPIRE);
    }

    void End(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* owner = GetUnitOwner();
        if (!owner)
            return;
        // Seismic Impact: the shockwave as the roll ends, however it ends (the distance, a second press, its time)
        if (owner->IsAlive())
            for (AuraEffect const* eff : Rules(owner, "spell_ef_kit_rule_end_wave", GetId()))
                if (uint32 const wave = TriggerOf(eff))
                    owner->CastSpell(owner, wave, true);
        // the look of before (the form's, at its growth stage); without a form any more the unit's own
        if (_formDisplay)
        {
            if (owner->HasAuraType(SPELL_AURA_TRANSFORM))
                owner->SetDisplayId(_formDisplay);
            else
                owner->RestoreDisplayId();
            _formDisplay = 0;
        }
        if (Player* player = owner->ToPlayer())
        {
            RollStart const* start = player->CustomData.Get<RollStart>(kRollKey);
            RestoreRollCooldown(player, GetSpellInfo(), start ? start->ms : NowMs());
        }
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_ef_kit_roll_aura::Apply, EFFECT_0, SPELL_AURA_MOD_INCREASE_SPEED, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_ef_kit_roll_aura::Tick, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_ef_kit_roll_aura::End, EFFECT_0, SPELL_AURA_MOD_INCREASE_SPEED, AURA_EFFECT_HANDLE_REAL);
    }
};

// A rule passive's own script: a marker the rules are found by (EfClassKit.h Rules); nothing happens in it.
class spell_ef_kit_earth_rule : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_earth_rule);

    void Register() override { }
};

// Earthen Regeneration's self-heal rule (the unit hook ModifyHealReceived). Mountain's Vengeance's damage hook went with
// the talent (ankarna's defensive rebalancing, 2026-10-29: Mountain's Renewal in its place).
class EmberfallEarthUnitScript : public UnitScript
{
public:
    EmberfallEarthUnitScript() : UnitScript("EmberfallEarthUnitScript", true, { UNITHOOK_MODIFY_HEAL_RECEIVED }) { }

    // Earthen Regeneration: the owner's heals on itself (the core passes the healer first: Unit::HealBySpell) are raised
    // by its self-heal rules; a heal from anyone else, or the owner's on an ally, is not
    void ModifyHealReceived(Unit* healer, Unit* target, uint32& heal, SpellInfo const* /*spellInfo*/) override
    {
        if (!healer || healer != target || !heal || !healer->IsPlayer())
            return;
        if (int32 const pct = SelfHealPct(healer); pct > 0)
            heal = uint32(Earth::SelfHealed(int64(heal), pct));
    }
};

void AddEmberfallClassKitEarthScripts()
{
    RegisterSpellAndAuraScriptPair(spell_ef_kit_form, spell_ef_kit_form_aura);
    RegisterSpellScript(spell_ef_kit_strike_channel);
    RegisterSpellScript(spell_ef_kit_strike);
    RegisterSpellScript(spell_ef_kit_absorb_reflect);
    RegisterSpellScript(spell_ef_kit_drop_field);
    RegisterSpellScript(spell_ef_kit_field_pull);
    RegisterSpellAndAuraScriptPair(spell_ef_kit_roll, spell_ef_kit_roll_aura);
    for (char const* name : { "spell_ef_kit_rule_heal_low", "spell_ef_kit_rule_heal_during", "spell_ef_kit_rule_overflow",
                              "spell_ef_kit_rule_heal_stacks", "spell_ef_kit_rule_quake_during", "spell_ef_kit_rule_every_nth",
                              "spell_ef_kit_rule_reflect", "spell_ef_kit_rule_momentum", "spell_ef_kit_rule_end_wave",
                              "spell_ef_kit_rule_self_heal" })
        RegisterSpellScriptWithArgs(spell_ef_kit_earth_rule, name);
    new EmberfallEarthUnitScript();
}
