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

// Emberfall class kits, the Hunter Ranger's v1.0 (ankarna's and sonaetiger's "Developer Handoff v1.0" of 2026-11-19;
// Emberfall docs/design/classes/hunter-ranger.md): the mechanics no shared script had. Generic like the scripts of
// src/emberfall_classkit.cpp: each reads its numbers from the spell's own effects (Emberfall's classkit/scripts_ranger.py
// checks the contracts); the arithmetic is classkit/EfRangerLogic.h (tests/classkit_ranger_tests.cpp).
//
//   spell_ef_kit_stance_rest        a passive (the spec passive): effect 1 a periodic dummy whose misc is the resting stance
//                                   (an aura); the other dummies' misc the stances a fight's end takes off. Every tick (and
//                                   when it comes): out of a fight those come off, the resting stance goes on and the spec
//                                   power is full; in a fight the resting stance goes on while no stance holds (Falcon's
//                                   Eye and 100 Energy at every fight's start, a stance always on)
//   spell_ef_kit_hunt_strike        the Ranger's strikes: on every enemy struck the caster's hit bonuses (self triggers once
//                                   a cast); a dummy at the enemy whose misc is an aura of the caster's on it takes all its
//                                   stacks and adds value % a stack to this strike (Hunter's Opening on Savage Leap's impact;
//                                   taken whether the strike lands or not: the miss rules); the caster's rules naming the
//                                   spell: spell_ef_kit_rule_health_crit (a sure critical strike at or above (misc B 1) or at
//                                   or below (misc B 2) value % of the target's health as it strikes: Fresh Prey, Final
//                                   Prey), spell_ef_kit_rule_hit_stacks (a stack of the trigger for every enemy struck, at
//                                   most misc B: Sweeping Momentum), spell_ef_kit_rule_wound_echo (value % of the damage each
//                                   enemy took as a damage over time of its own: the trigger's pool of misc B instances,
//                                   consecutive IDs, a free one or the one with the least time left: Raking Wounds); and a
//                                   player caster's melee auto attack starts on the target when it stands in melee reach
//                                   (the Ranger swings in both stances; the adaptive button's Rending Strike is cast through
//                                   the server's override, so the client never asks for the swing)
//   spell_ef_kit_primary_area       effect 1 an area round the caster: the caster's selected enemy inside it takes the blow
//                                   whole, up to (the spell's most targets - 1) others nearest first misc B % of it; without
//                                   a selected enemy in the area only those (Retreat: your target + 4 at half; no target or
//                                   one out of the area: 4 at half)
//   spell_ef_kit_landing_bonuses    a leap (jump_dest): the caster's passive dummies naming it cast their trigger on the
//                                   caster as it starts (misc B 0: Clean Escape) or once it has landed (misc B 1: Swift
//                                   Retreat), at once when it did not move (a wall behind)
//   spell_ef_kit_pet_command        the companion's command (Unleash): refused without a living companion (the cooldown is
//                                   the spell's, taken at the command); effect 1 (dummy at the enemy): the companion goes for
//                                   it, runs while farther than value yards (or out of sight) and casts the trigger (its
//                                   charge) at it once within them; it waits while it cannot act; the command lapses after
//                                   misc B seconds, when the companion dies or the enemy is gone (nothing given back)
//   spell_ef_kit_pet_command_strike the companion's strike: when it lands (no miss, dodge, parry or immunity) its owner's
//                                   passive dummies naming it act: a trigger is cast on the companion (Pack Ferocity; a
//                                   stacking one at its full stacks), misc B 1 on its packmates too (Hunting Frenzy);
//                                   without a trigger misc B is a creature: a packmate of the companion's kind beside the
//                                   enemy for value ms (Call of the Pack: the companion's look, level and faction, its
//                                   target followed, every swing the companion's own)
//   spell_ef_kit_swing_bonus        a dummy aura on a unit: its auto attacks deal value % more while a stack is left, a swing
//                                   a stack (Pack Ferocity on the companion)
//   spell_ef_kit_rule_*             the rules above: markers (nothing happens in them)

#include "EfClassKit.h"
#include "EfClassKitLogic.h"
#include "EfRangerLogic.h"

#include "CharmInfo.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "DataMap.h"
#include "EventProcessor.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "TemporarySummon.h"
#include "Unit.h"

#include <algorithm>
#include <cmath>
#include <list>
#include <vector>

namespace
{
    namespace Ranger = Emberfall::ClassKit::Ranger;

    constexpr char const* kPackmateKey = "ef_packmate";
    constexpr char const* kPackKey = "ef_packmates";

    // On a packmate (Call of the Pack): the companion it copies.
    struct PackmateData : public DataMap::Base
    {
        ObjectGuid pet;
    };

    // On a companion: its packmates standing now.
    struct PackData : public DataMap::Base
    {
        std::vector<ObjectGuid> mates;
    };

    Pet* LivingPet(Unit* caster)
    {
        Player* player = caster ? caster->ToPlayer() : nullptr;
        Pet* pet = player ? player->GetPet() : nullptr;
        return pet && pet->IsAlive() && pet->IsInWorld() ? pet : nullptr;
    }

    // The companion goes for `target` as on the pet bar's Attack (PetHandler's COMMAND_ATTACK).
    void SendPet(Creature* pet, Unit* target)
    {
        if (!pet || !target)
            return;
        if (CharmInfo* ci = pet->GetCharmInfo())
        {
            ci->SetIsCommandAttack(true);
            ci->SetIsAtStay(false);
            ci->SetIsFollowing(false);
            ci->SetIsCommandFollow(false);
            ci->SetIsReturning(false);
        }
        if (pet->GetVictim() != target && pet->IsAIEnabled)
            pet->AI()->AttackStart(target);
    }

    bool CanAct(Unit const* unit)
    {
        return unit && !unit->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING | UNIT_STATE_ROOT);
    }

    // Casts `spell` on `unit` (an aura of its own); a stacking one at its full stacks (a new one renews it).
    void CastFull(Unit* unit, uint32 spell)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
        if (!unit || !info || !unit->IsAlive())
            return;
        unit->CastSpell(unit, spell, true);
        if (info->StackAmount > 1)
            if (Aura* aura = unit->GetAura(spell, unit->GetGUID()))
            {
                aura->SetStackAmount(uint8(info->StackAmount));
                aura->RefreshDuration();
            }
    }

    // ---- the command's clock (on the companion) ----
    class PetCommandEvent : public BasicEvent
    {
    public:
        PetCommandEvent(Creature* pet, ObjectGuid target, uint32 charge, float reach, uint32 timeoutMs, uint32 elapsedMs)
            : _pet(pet), _target(target), _charge(charge), _reach(reach), _timeoutMs(timeoutMs), _elapsedMs(elapsedMs) { }

        bool Execute(uint64 /*time*/, uint32 /*diff*/) override
        {
            constexpr uint32 kStepMs = 200;
            Unit* target = _pet->IsInWorld() ? ObjectAccessor::GetUnit(*_pet, _target) : nullptr;
            bool const valid = target && target->IsAlive() && target->IsInWorld() && target->GetMapId() == _pet->GetMapId()
                               && _pet->IsValidAttackTarget(target);
            float const distance = target ? _pet->GetExactDist(target) : 0.0f;
            bool const sight = target && _pet->IsWithinLOSInMap(target);
            switch (Ranger::PetCommand(_pet->IsAlive() && _pet->IsInWorld(), valid, CanAct(_pet), distance, _reach, sight,
                                       _elapsedMs, _timeoutMs))
            {
                case Ranger::PetStep::Lapse:
                    return true;
                case Ranger::PetStep::Charge:
                    _pet->SetFacingToObject(target);
                    _pet->CastSpell(target, _charge, true);
                    return true;
                case Ranger::PetStep::Run:
                    SendPet(_pet, target);
                    break;
                case Ranger::PetStep::Wait:
                    break;
            }
            _pet->m_Events.AddEventAtOffset(new PetCommandEvent(_pet, _target, _charge, _reach, _timeoutMs,
                                                                _elapsedMs + kStepMs), Milliseconds(kStepMs));
            return true;
        }

    private:
        Creature* _pet;
        ObjectGuid _target;
        uint32 _charge;
        float _reach;
        uint32 _timeoutMs;
        uint32 _elapsedMs;
    };

    // ---- a packmate's clock: it keeps to its companion's target ----
    class PackmateFollowEvent : public BasicEvent
    {
    public:
        explicit PackmateFollowEvent(Creature* mate) : _mate(mate) { }

        bool Execute(uint64 /*time*/, uint32 /*diff*/) override
        {
            if (!_mate->IsInWorld() || !_mate->IsAlive())
                return true;
            PackmateData const* data = _mate->CustomData.Get<PackmateData>(kPackmateKey);
            Unit* pet = data ? ObjectAccessor::GetUnit(*_mate, data->pet) : nullptr;
            Unit* victim = pet && pet->IsAlive() ? pet->GetVictim() : nullptr;
            if (victim && victim != _mate->GetVictim() && victim->IsAlive() && _mate->IsValidAttackTarget(victim)
                && _mate->IsAIEnabled)
                _mate->AI()->AttackStart(victim);
            _mate->m_Events.AddEventAtOffset(new PackmateFollowEvent(_mate), Milliseconds(250));
            return true;
        }

    private:
        Creature* _mate;
    };

    // Call of the Pack: a packmate of the companion's kind beside `target` for `ms`.
    Creature* SummonPackmate(Creature* pet, Player* owner, Unit* target, uint32 entry, uint32 ms)
    {
        if (!pet || !owner || !target || !entry || !ms)
            return nullptr;
        Position const spot = target->GetNearPosition(target->GetCombatReach() + 1.0f, float(rand_norm()) * 2.0f * float(M_PI));
        TempSummon* mate = pet->SummonCreature(entry, spot, TEMPSUMMON_TIMED_DESPAWN, ms);
        if (!mate)
            return nullptr;
        mate->SetLevel(pet->GetLevel());
        mate->SetFaction(owner->GetFaction());
        mate->SetCreatorGUID(owner->GetGUID());
        mate->SetOwnerGUID(owner->GetGUID());
        mate->SetDisplayId(pet->GetDisplayId());
        mate->SetNativeDisplayId(pet->GetDisplayId());
        mate->SetObjectScale(pet->GetObjectScale());
        mate->SetMaxHealth(pet->GetMaxHealth());
        mate->SetFullHealth();
        mate->SetAttackTime(BASE_ATTACK, pet->GetAttackTime(BASE_ATTACK));
        mate->SetReactState(REACT_AGGRESSIVE);
        mate->CustomData.GetDefault<PackmateData>(kPackmateKey)->pet = pet->GetGUID();
        PackData* pack = pet->CustomData.GetDefault<PackData>(kPackKey);
        pack->mates.push_back(mate->GetGUID());
        if (mate->IsAIEnabled)
            mate->AI()->AttackStart(pet->GetVictim() ? pet->GetVictim() : target);
        mate->m_Events.AddEventAtOffset(new PackmateFollowEvent(mate), Milliseconds(250));
        return mate;
    }

    // The packmates of `pet` standing now.
    std::vector<Creature*> Packmates(Creature* pet)
    {
        std::vector<Creature*> out;
        PackData* pack = pet ? pet->CustomData.Get<PackData>(kPackKey) : nullptr;
        if (!pack)
            return out;
        std::vector<ObjectGuid> kept;
        for (ObjectGuid const& guid : pack->mates)
            if (Creature* mate = ObjectAccessor::GetCreature(*pet, guid); mate && mate->IsAlive() && mate->IsInWorld())
            {
                out.push_back(mate);
                kept.push_back(guid);
            }
        pack->mates = kept;
        return out;
    }

    // ---- the landing clock (on the leaper) ----
    class LandingEvent : public BasicEvent
    {
    public:
        LandingEvent(Unit* unit, std::vector<uint32> triggers, uint32 polls) : _unit(unit), _triggers(std::move(triggers)), _polls(polls) { }

        bool Execute(uint64 /*time*/, uint32 /*diff*/) override
        {
            bool const moving = _unit->IsInWorld()
                && (_unit->HasUnitState(UNIT_STATE_JUMPING) || _unit->GetMotionMaster()->GetCurrentMovementGeneratorType() == EFFECT_MOTION_TYPE);
            if (moving && _polls > 0)
            {
                _unit->m_Events.AddEventAtOffset(new LandingEvent(_unit, _triggers, _polls - 1), Milliseconds(100));
                return true;
            }
            if (_unit->IsInWorld() && _unit->IsAlive())
                for (uint32 trigger : _triggers)
                    _unit->CastSpell(_unit, trigger, true);
            return true;
        }

    private:
        Unit* _unit;
        std::vector<uint32> _triggers;
        uint32 _polls;
    };
}

// The stance rest (the spec passive).
class spell_ef_kit_stance_rest : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_stance_rest);

    void Tick()
    {
        Player* player = GetUnitOwner() ? GetUnitOwner()->ToPlayer() : nullptr;
        if (!player || !player->IsInWorld() || !player->IsAlive())
            return;
        uint32 const rest = uint32(GetSpellInfo()->Effects[EFFECT_0].MiscValue);
        if (!rest)
            return;
        std::vector<uint32> others;
        for (uint8 i = EFFECT_1; i < MAX_SPELL_EFFECTS; ++i)
        {
            SpellEffectInfo const& eff = GetSpellInfo()->Effects[i];
            if (eff.IsAura(SPELL_AURA_DUMMY) && eff.MiscValue > 0)
                others.push_back(uint32(eff.MiscValue));
        }
        bool hasOther = false;
        for (uint32 o : others)
            hasOther = hasOther || player->HasAura(o);
        Powers const slot = player->getPowerType();
        Ranger::RestAction const act = Ranger::Rest(player->IsInCombat(), player->HasAura(rest), hasOther,
                                                    int32(player->GetPower(slot)), int32(player->GetMaxPower(slot)));
        if (act.removeOther)
            for (uint32 o : others)
                player->RemoveAurasDueToSpell(o);
        if (act.addRest)
            player->CastSpell(player, rest, true);
        if (act.fill)
            Emberfall::ClassKit::FillSpecPower(player);
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/) { Tick(); }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/) { Tick(); }

    // the passive goes (another spec, the spec unlearned, a logout): the stances go with it (the next login's first tick
    // puts the resting one back)
    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* owner = GetUnitOwner();
        if (!owner)
            return;
        for (SpellEffectInfo const& eff : GetSpellInfo()->Effects)
            if ((eff.IsAura(SPELL_AURA_DUMMY) || eff.IsAura(SPELL_AURA_PERIODIC_DUMMY)) && eff.MiscValue > 0)
                owner->RemoveAurasDueToSpell(uint32(eff.MiscValue));
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_ef_kit_stance_rest::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectApply += AuraEffectApplyFn(spell_ef_kit_stance_rest::HandleApply, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY,
                                              AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_ef_kit_stance_rest::HandleRemove, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY,
                                                AURA_EFFECT_HANDLE_REAL);
    }
};

// The Ranger's strikes.
class spell_ef_kit_hunt_strike : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_hunt_strike);

    bool _first = true;
    uint32 _hits = 0;
    bool _stacksGiven = false;
    std::vector<std::pair<ObjectGuid, int32>> _stackPct;   // per enemy: the percent its taken stacks add

    // the dummy at the enemy naming an aura of the caster's (Hunter's Opening on Savage Leap's impact)
    SpellEffectInfo const* StackEffect()
    {
        for (SpellEffectInfo const& eff : GetSpellInfo()->Effects)
            if (eff.Effect == SPELL_EFFECT_DUMMY && !eff.TriggerSpell && eff.MiscValue > 0
                && eff.TargetA.GetTarget() != TARGET_UNIT_CASTER)
                return &eff;
        return nullptr;
    }

    void BeforeHitTarget(SpellMissInfo missInfo)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || target == caster)
            return;
        // the target's stacks of the caster's aura: all taken, struck or not (the miss rules)
        if (SpellEffectInfo const* eff = StackEffect())
        {
            uint32 const aura = uint32(eff->MiscValue);
            if (Aura* held = target->GetAura(aura, caster->GetGUID()))
            {
                int32 const stacks = int32(held->GetStackAmount());
                held->Remove();
                _stackPct.emplace_back(target->GetGUID(), stacks * std::max(0, eff->CalcValue(caster)));
            }
        }
        if (missInfo != SPELL_MISS_NONE)
            return;
        // a sure critical strike by the target's health as it strikes (the launch rolled the crit already; the damage
        // takes it from the target info)
        float const health = target->GetHealthPct();
        bool sure = false;
        for (AuraEffect const* rule : Emberfall::ClassKit::Rules(caster, "spell_ef_kit_rule_health_crit", GetSpellInfo()->Id))
            sure = sure || Ranger::HealthCrit(health, rule->GetAmount(), rule->GetMiscValueB());
        if (sure)
            for (TargetInfo& info : *GetSpell()->GetUniqueTargetInfo())
                if (info.targetGUID == target->GetGUID())
                    info.crit = true;
    }

    void HitTarget()
    {
        Unit* target = GetHitUnit();
        if (!target || GetHitDamage() <= 0)
            return;
        for (auto const& [guid, pct] : _stackPct)
            if (guid == target->GetGUID() && pct > 0)
                SetHitDamage(Ranger::StackedStrike(GetHitDamage(), 1, pct));
    }

    void AfterHitTarget()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || target == caster || !Emberfall::ClassKit::StruckUnit(GetSpell(), target))
            return;
        ++_hits;
        Emberfall::ClassKit::HitBonuses(caster, target, GetSpellInfo()->Id, 0, _first);
        _first = false;
        // a share of the damage dealt as a damage over time of its own (Raking Wounds)
        int32 const dealt = GetHitDamage();
        if (dealt > 0 && target->IsAlive())
            for (AuraEffect const* rule : Emberfall::ClassKit::Rules(caster, "spell_ef_kit_rule_wound_echo", GetSpellInfo()->Id))
            {
                SpellEffectInfo const& eff = rule->GetSpellInfo()->Effects[rule->GetEffIndex()];
                SpellInfo const* dot = sSpellMgr->GetSpellInfo(eff.TriggerSpell);
                if (!dot)
                    continue;
                int32 const period = int32(dot->Effects[EFFECT_0].Amplitude);
                int32 const ticks = period > 0 ? dot->GetMaxDuration() / period : 0;
                int32 const tick = Ranger::EchoTick(dealt, rule->GetAmount(), ticks);
                if (tick <= 0)
                    continue;
                std::vector<int32> left;
                for (int32 i = 0; i < std::max(1, eff.MiscValueB); ++i)
                {
                    Aura const* held = target->GetAura(eff.TriggerSpell + uint32(i), caster->GetGUID());
                    left.push_back(held ? std::max(0, held->GetDuration()) : -1);
                }
                uint32 const spell = eff.TriggerSpell + uint32(Ranger::PickInstance(left));
                if (!sSpellMgr->GetSpellInfo(spell))
                    continue;
                target->RemoveAurasDueToSpell(spell, caster->GetGUID());
                caster->CastCustomSpell(target, spell, &tick, nullptr, nullptr, true);
            }
    }

    void After()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;
        // a stack for every enemy struck (Sweeping Momentum)
        if (!_stacksGiven)
        {
            _stacksGiven = true;
            for (AuraEffect const* rule : Emberfall::ClassKit::Rules(caster, "spell_ef_kit_rule_hit_stacks", GetSpellInfo()->Id))
            {
                uint32 const trigger = rule->GetSpellInfo()->Effects[rule->GetEffIndex()].TriggerSpell;
                int32 const gain = Ranger::HitStacks(_hits, rule->GetMiscValueB());
                if (trigger && gain > 0)
                    Emberfall::ClassKit::AddSelfStacks(caster, trigger, gain);
            }
        }
        // the melee swing on a target in reach (both stances)
        Player* player = caster->ToPlayer();
        Unit* target = GetExplTargetUnit();
        if (player && target && target != player && target->IsAlive() && player->IsValidAttackTarget(target)
            && player->IsWithinMeleeRange(target) && player->GetVictim() != target)
            player->Attack(target, true);
    }

    void Register() override
    {
        BeforeHit += BeforeSpellHitFn(spell_ef_kit_hunt_strike::BeforeHitTarget);
        OnHit += SpellHitFn(spell_ef_kit_hunt_strike::HitTarget);
        AfterHit += SpellHitFn(spell_ef_kit_hunt_strike::AfterHitTarget);
        AfterCast += SpellCastFn(spell_ef_kit_hunt_strike::After);
    }
};

// Retreat's blow round the caster: the selected enemy whole, the others a share.
class spell_ef_kit_primary_area : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_primary_area);

    ObjectGuid _primary;

    void SelectTargets(std::list<WorldObject*>& targets)
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;
        Unit* selected = caster->ToPlayer() ? caster->ToPlayer()->GetSelectedUnit() : caster->GetVictim();
        std::vector<WorldObject*> nearest(targets.begin(), targets.end());
        std::sort(nearest.begin(), nearest.end(), [caster](WorldObject const* a, WorldObject const* b)
        {
            return caster->GetExactDistSq(a) < caster->GetExactDistSq(b);
        });
        int32 primary = -1;
        for (size_t i = 0; i < nearest.size(); ++i)
            if (selected && nearest[i]->GetGUID() == selected->GetGUID())
                primary = int32(i);
        int32 const cap = GetSpellInfo()->MaxAffectedTargets ? int32(GetSpellInfo()->MaxAffectedTargets) : 5;
        std::list<WorldObject*> kept;
        for (size_t i : Ranger::AreaPick(nearest.size(), primary, cap))
            kept.push_back(nearest[i]);
        _primary = primary >= 0 ? nearest[size_t(primary)]->GetGUID() : ObjectGuid::Empty;
        targets = kept;
    }

    void HitTarget()
    {
        Unit* target = GetHitUnit();
        if (!target || GetHitDamage() <= 0)
            return;
        bool const primary = !_primary.IsEmpty() && target->GetGUID() == _primary;
        SetHitDamage(Ranger::AreaShare(GetHitDamage(), primary, GetSpellInfo()->Effects[EFFECT_0].MiscValueB));
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_ef_kit_primary_area::SelectTargets, EFFECT_0,
                                                                  TARGET_UNIT_SRC_AREA_ENEMY);
        OnHit += SpellHitFn(spell_ef_kit_primary_area::HitTarget);
    }
};

// A leap's start and landing bonuses.
class spell_ef_kit_landing_bonuses : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_landing_bonuses);

    void After()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;
        std::vector<uint32> start, landing;
        for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
        {
            uint32 const trigger = eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell;
            if (uint32(eff->GetMiscValue()) != GetSpellInfo()->Id || !trigger || !eff->GetBase()->IsPassive())
                continue;
            (eff->GetMiscValueB() == 1 ? landing : start).push_back(trigger);
        }
        for (uint32 trigger : start)
            caster->CastSpell(caster, trigger, true);
        if (!landing.empty())
            caster->m_Events.AddEventAtOffset(new LandingEvent(caster, landing, 30), Milliseconds(100));
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_ef_kit_landing_bonuses::After);
    }
};

// Unleash: the companion's command.
class spell_ef_kit_pet_command : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_pet_command);

    SpellCastResult Check()
    {
        return LivingPet(GetCaster()) ? SPELL_CAST_OK : SPELL_FAILED_NO_PET;
    }

    void HandleDummy(SpellEffIndex effIndex)
    {
        Unit* target = GetHitUnit();
        Pet* pet = LivingPet(GetCaster());
        SpellEffectInfo const& eff = GetSpellInfo()->Effects[effIndex];
        if (!target || !pet || !eff.TriggerSpell)
            return;
        float const reach = float(std::max(1, GetEffectValue()));
        uint32 const timeoutMs = uint32(std::max(1, eff.MiscValueB)) * IN_MILLISECONDS;
        SendPet(pet, target);
        // the first look at once: a companion already within reach charges now
        pet->m_Events.AddEventAtOffset(new PetCommandEvent(pet, target->GetGUID(), eff.TriggerSpell, reach, timeoutMs, 0),
                                       Milliseconds(1));
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_ef_kit_pet_command::Check);
        OnEffectHitTarget += SpellEffectFn(spell_ef_kit_pet_command::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

// Unleash: the companion's strike and its owner's talents.
class spell_ef_kit_pet_command_strike : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_pet_command_strike);

    void AfterHitTarget()
    {
        Creature* pet = GetCaster() ? GetCaster()->ToCreature() : nullptr;
        Unit* target = GetHitUnit();
        if (!pet || !target || target == pet || !Emberfall::ClassKit::StruckUnit(GetSpell(), target))
            return;
        Player* owner = pet->GetOwner() ? pet->GetOwner()->ToPlayer() : nullptr;
        if (!owner)
            return;
        struct Talent
        {
            uint32 trigger;
            int32 miscB;
            int32 amount;
        };
        std::vector<Talent> talents;
        for (AuraEffect const* eff : owner->GetAuraEffectsByType(SPELL_AURA_DUMMY))
            if (uint32(eff->GetMiscValue()) == GetSpellInfo()->Id && eff->GetBase()->IsPassive())
                talents.push_back({ eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell, eff->GetMiscValueB(),
                                    eff->GetAmount() });
        // the packmates first: the shared buffs reach them too
        for (Talent const& t : talents)
            if (!t.trigger && t.miscB > 1 && t.amount > 0 && target->IsAlive())
                SummonPackmate(pet, owner, target, uint32(t.miscB), uint32(t.amount));
        for (Talent const& t : talents)
        {
            if (!t.trigger)
                continue;
            CastFull(pet, t.trigger);
            if (t.miscB == 1)
                for (Creature* mate : Packmates(pet))
                    CastFull(mate, t.trigger);
        }
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_ef_kit_pet_command_strike::AfterHitTarget);
    }
};

// A swing bonus's marker (the unit script below reads it).
class spell_ef_kit_swing_bonus : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_swing_bonus);

    void Register() override { }
};

// The rules' markers: nothing happens in them (EfClassKit.h Rules finds the passives by these names).
class spell_ef_kit_ranger_rule : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_ranger_rule);

    void Register() override { }
};

// The auto attacks: a packmate swings as its companion would; a swing bonus adds its percent and uses a stack.
class EmberfallRangerUnitScript : public UnitScript
{
public:
    EmberfallRangerUnitScript() : UnitScript("EmberfallRangerUnitScript", true, { UNITHOOK_MODIFY_MELEE_DAMAGE }) { }

    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) override
    {
        if (!attacker || !target || attacker->IsPlayer())
            return;
        if (PackmateData const* data = attacker->CustomData.Get<PackmateData>(kPackmateKey))
            if (Unit* pet = ObjectAccessor::GetUnit(*attacker, data->pet); pet && pet->IsAlive())
            {
                uint32 own = pet->CalculateDamage(BASE_ATTACK, false, true);
                own = pet->MeleeDamageBonusDone(target, own, BASE_ATTACK, nullptr, SPELL_SCHOOL_MASK_NORMAL);
                own = target->MeleeDamageBonusTaken(pet, own, BASE_ATTACK, nullptr, SPELL_SCHOOL_MASK_NORMAL);
                damage = own;
            }
        if (!damage)
            return;
        for (AuraEffect const* eff : attacker->GetAuraEffectsByType(SPELL_AURA_DUMMY))
        {
            Aura* aura = eff->GetBase();
            if (!aura->GetScriptByName("spell_ef_kit_swing_bonus"))
                continue;
            Ranger::Swing const swing = Ranger::SwingBonus(int32(damage), eff->GetAmount(), int32(aura->GetStackAmount()));
            damage = uint32(std::max(0, swing.damage));
            aura->ModStackAmount(-1);
            break;
        }
    }
};

void AddEmberfallClassKitRangerScripts()
{
    RegisterSpellScript(spell_ef_kit_stance_rest);
    RegisterSpellScript(spell_ef_kit_hunt_strike);
    RegisterSpellScript(spell_ef_kit_primary_area);
    RegisterSpellScript(spell_ef_kit_landing_bonuses);
    RegisterSpellScript(spell_ef_kit_pet_command);
    RegisterSpellScript(spell_ef_kit_pet_command_strike);
    RegisterSpellScript(spell_ef_kit_swing_bonus);
    for (char const* name : { "spell_ef_kit_rule_health_crit", "spell_ef_kit_rule_hit_stacks", "spell_ef_kit_rule_wound_echo" })
        RegisterSpellScriptWithArgs(spell_ef_kit_ranger_rule, name);
    new EmberfallRangerUnitScript();
}
