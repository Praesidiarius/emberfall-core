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

// Emberfall class kits, the Hunter's (Emberfall docs/design/classes/hunter-marksman.md): the Marksman's Beads and Full
// Draw, the companion sent by the hunter's buttons, Gentling (taming) and the companion's share of the hunter's power.
// Generic like the scripts of src/emberfall_classkit.cpp: each reads its numbers from the spell's own effects (Emberfall's
// classkit/model.py SCRIPTS checks the contracts):
//
//   spell_ef_kit_spend_self_stacks  a spender of the caster's own stacks (Full Draw): effect 2 (dummy on the caster):
//                                   misc = the self aura, value = percent more damage for each stack (spell mods in:
//                                   Patient Kill); the cast takes every stack and effect 1's damage grows by them; effect
//                                   3 (dummy on the caster): with the aura's full stacks (its cap) spent, its value (spell
//                                   mods in: Killing Draw) in points of the caster's power back. Spend bonuses: the caster's
//                                   passive dummy auras with misc = this spell and misc B > 0 act when at least misc B
//                                   stacks were spent: with a trigger it is cast at the first enemy with its value in
//                                   percent of the damage dealt there (Second Arrow; an area trigger from the caster:
//                                   Through and Through), without one its value in stacks stays (Unbroken Aim). The hit
//                                   bonuses for this spell run on every enemy struck (EfClassKit.h HitBonuses: Broadhead)
//   spell_ef_kit_pet_strike         the hunter sends the companion (Set On, Pin Down): refused without a living companion
//                                   or with it farther than effect 1's value in yards from the target; on the hit the
//                                   companion turns to the target and casts effect 1's trigger at it (its leap and bite)
//   spell_ef_kit_gentling           taming (Gentling): the checks of a tame (a tameable beast of the hunter's level or
//                                   below, no companion out, none waiting); when the channel's aura runs its full time
//                                   the hunter casts effect 1's trigger at the beast (TAME_CREATURE, the class quest's
//                                   credit)
//   spell_ef_kit_dash               a dash ahead along the caster's facing (Verdant Dash): the engine's CHARGE_DEST to
//                                   TARGET_DEST_CASTER_FRONT_LEAP (the blink's destination, stepped along the ground and
//                                   stopped short of walls and solid terrain; MoveCharge at its own speed, so slows change
//                                   neither speed nor distance); this script refuses it while rooted (the engine checks
//                                   roots for charges and jumps but not for CHARGE_DEST) and when not on the ground
//                                   (falling or jumping, flying, swimming)
//   spell_ef_pet_owner_scaling      the companion's share of its hunter (spells/pets.yaml, a family passive on every
//                                   companion): attack power (effect value: percent of the hunter's ranged attack power),
//                                   stamina and armour (percent of the hunter's), recomputed every 2 s
//
// A PetScript keeps the companion at its hunter's level when it comes into the world; its family abilities autocast from
// the start (the core learns them with autocast on, PATCHES.md #27: a hunter never needs the pet bar in a normal fight).

#include "EfClassKit.h"
#include "EfClassKitLogic.h"

#include "CharmInfo.h"
#include "ObjectAccessor.h"
#include "CreatureAI.h"
#include "Pet.h"
#include "PetDefines.h"
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

namespace
{
    // The hunter's living companion in the world, or nullptr.
    Pet* LivingPet(Unit* caster)
    {
        Player* player = caster ? caster->ToPlayer() : nullptr;
        Pet* pet = player ? player->GetPet() : nullptr;
        return pet && pet->IsAlive() && pet->IsInWorld() ? pet : nullptr;
    }

    // The companion goes for `target` as on the pet bar's Attack (PetHandler's COMMAND_ATTACK).
    void SendPet(Pet* pet, Unit* target)
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
}

class spell_ef_kit_spend_self_stacks : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_spend_self_stacks);

    uint32 _spent = 0;
    int32 _pct = 0;
    bool _first = true;
    int32 _firstDamage = 0;
    ObjectGuid _firstTarget;
    bool _cast = false;          // AfterCast ran: a later hit is the missile's (Spell.dbc speed, feedback #131)
    bool _bonusesDone = false;
    std::vector<std::pair<uint32, int32>> _bonuses;   // the spend bonuses' triggers and percents, read at the cast

    uint32 StackSpell() { return uint32(GetSpellInfo()->Effects[EFFECT_1].MiscValue); }

    bool Validate(SpellInfo const* spellInfo) override
    {
        return ValidateSpellInfo({ uint32(spellInfo->Effects[EFFECT_1].MiscValue) });
    }

    void Spend()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;
        _pct = GetSpellInfo()->Effects[EFFECT_1].CalcValue(caster);
        _spent = Emberfall::ClassKit::TakeStacks(caster, caster, StackSpell(), 0);
    }

    void HandleHit()
    {
        Unit* target = GetHitUnit();
        if (!target || target == GetCaster())
            return;
        int32 const damage = Emberfall::ClassKit::Logic::StackedDamage(GetHitDamage(), _pct, _spent);
        if (_spent && GetHitDamage() > 0)
            SetHitDamage(damage);
        if (_firstTarget.IsEmpty())
        {
            _firstTarget = target->GetGUID();
            _firstDamage = damage;
        }
    }

    void AfterHitTarget()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || target == caster)
            return;
        // the hit bonuses with no stack need (Broadhead's bleed); the spend bonuses below need their stacks
        Emberfall::ClassKit::HitBonuses(caster, target, GetSpellInfo()->Id, 0, _first);
        _first = false;
        if (_cast)
            CastBonuses(caster);
    }

    // the spend bonuses with a trigger, at the first enemy struck with their percent of the damage dealt there: after
    // the cast for an instant hit, as the arrow strikes for a missile
    void CastBonuses(Unit* caster)
    {
        if (_bonusesDone || _firstTarget.IsEmpty())
            return;
        _bonusesDone = true;
        Unit* target = ObjectAccessor::GetUnit(*caster, _firstTarget);
        for (auto const& [trigger, pct] : _bonuses)
        {
            int32 amount = Emberfall::ClassKit::Logic::PctOf(_firstDamage, pct);
            if (amount <= 0 || !target || !target->IsAlive())
                continue;
            caster->CastCustomSpell(target, trigger, &amount, nullptr, nullptr, true);
        }
    }

    void Finish()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;
        SpellInfo const* stackInfo = sSpellMgr->GetSpellInfo(StackSpell());
        uint32 const cap = stackInfo ? uint32(std::max(1, stackInfo->Effects[EFFECT_0].CalcValue(caster))) : 0;
        // effect 3: with the full stacks spent, power back (Killing Draw)
        if (cap && _spent >= cap)
        {
            int32 const back = GetSpellInfo()->Effects[EFFECT_2].CalcValue(caster);
            if (back > 0 && GetSpellInfo()->Effects[EFFECT_2].Effect == SPELL_EFFECT_DUMMY)
                caster->EnergizeBySpell(caster, GetSpellInfo()->Id, uint32(back), caster->getPowerType());
        }
        if (!_spent)
            return;
        // spend bonuses: passive dummies naming this spell, misc B = the stacks needed
        int32 keep = 0;
        for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
        {
            if (uint32(eff->GetMiscValue()) != GetSpellInfo()->Id || !eff->GetBase()->IsPassive())
                continue;
            int32 const need = eff->GetSpellInfo()->Effects[eff->GetEffIndex()].MiscValueB;
            if (need <= 0 || _spent < uint32(need))
                continue;
            uint32 const trigger = eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell;
            if (trigger)
                _bonuses.emplace_back(trigger, eff->GetAmount());
            else
                keep = std::max(keep, eff->GetAmount());
        }
        if (keep > 0)
            Emberfall::ClassKit::AddSelfStacks(caster, StackSpell(), keep);
        _cast = true;
        CastBonuses(caster);
    }

    void Register() override
    {
        OnCast += SpellCastFn(spell_ef_kit_spend_self_stacks::Spend);
        OnHit += SpellHitFn(spell_ef_kit_spend_self_stacks::HandleHit);
        AfterHit += SpellHitFn(spell_ef_kit_spend_self_stacks::AfterHitTarget);
        AfterCast += SpellCastFn(spell_ef_kit_spend_self_stacks::Finish);
    }
};

class spell_ef_kit_pet_strike : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_pet_strike);

    SpellCastResult Check()
    {
        Unit* caster = GetCaster();
        Pet* pet = LivingPet(caster);
        if (!pet)
            return SPELL_FAILED_NO_PET;
        Unit* target = GetExplTargetUnit();
        float const reach = float(std::max(5, GetSpellInfo()->Effects[EFFECT_0].CalcValue(caster)));
        if (target && !pet->IsWithinDistInMap(target, reach))
            return SPELL_FAILED_OUT_OF_RANGE;
        if (pet->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING))
            return SPELL_FAILED_CASTER_AURASTATE;
        return SPELL_CAST_OK;
    }

    void HandleDummy(SpellEffIndex effIndex)
    {
        Unit* target = GetHitUnit();
        Pet* pet = LivingPet(GetCaster());
        uint32 const trigger = GetSpellInfo()->Effects[effIndex].TriggerSpell;
        if (!target || !pet || !trigger)
            return;
        SendPet(pet, target);
        pet->CastSpell(target, trigger, true);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_ef_kit_pet_strike::Check);
        OnEffectHitTarget += SpellEffectFn(spell_ef_kit_pet_strike::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

class spell_ef_kit_dash : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_dash);

    SpellCastResult Check()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return SPELL_FAILED_BAD_TARGETS;
        if (caster->HasUnitState(UNIT_STATE_ROOT))
            return SPELL_FAILED_ROOTED;
        if (caster->IsFalling() || caster->IsFlying() || caster->IsInWater()
            || caster->HasUnitMovementFlag(MOVEMENTFLAG_SWIMMING))
            return SPELL_FAILED_CANT_DO_THAT_RIGHT_NOW;
        return SPELL_CAST_OK;
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_ef_kit_dash::Check);
    }
};

class spell_ef_kit_gentling : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_gentling);

    SpellCastResult Check()
    {
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!player)
            return SPELL_FAILED_DONT_REPORT;
        Creature* target = GetExplTargetUnit() ? GetExplTargetUnit()->ToCreature() : nullptr;
        if (!target || target->IsPet())
        {
            player->SendTameFailure(PET_TAME_INVALID_CREATURE);
            return SPELL_FAILED_DONT_REPORT;
        }
        if (target->GetLevel() > player->GetLevel())
        {
            player->SendTameFailure(PET_TAME_TOO_HIGHLEVEL);
            return SPELL_FAILED_DONT_REPORT;
        }
        if (!target->GetCreatureTemplate()->IsTameable(player->CanTameExoticPets()))
        {
            player->SendTameFailure(PET_TAME_NOT_TAMEABLE);
            return SPELL_FAILED_DONT_REPORT;
        }
        if (PetStable const* stable = player->GetPetStable())
        {
            if (stable->CurrentPet)
                return SPELL_FAILED_ALREADY_HAVE_SUMMON;
            if (stable->GetUnslottedHunterPet())
            {
                player->SendTameFailure(PET_TAME_TOO_MANY);
                return SPELL_FAILED_DONT_REPORT;
            }
        }
        if (player->GetPetGUID() || player->GetCharmGUID())
        {
            player->SendTameFailure(PET_TAME_ANOTHER_SUMMON_ACTIVE);
            return SPELL_FAILED_DONT_REPORT;
        }
        if (target->GetOwnerGUID())
        {
            player->SendTameFailure(PET_TAME_CREATURE_ALREADY_OWNED);
            return SPELL_FAILED_DONT_REPORT;
        }
        return SPELL_CAST_OK;
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_ef_kit_gentling::Check);
    }
};

class spell_ef_kit_gentling_aura : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_gentling_aura);

    void HandleRemove(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
    {
        // only a channel held to its end: moving or an interrupt cancels it
        if (GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_EXPIRE)
            return;
        Unit* caster = GetCaster();
        Unit* target = GetTarget();
        uint32 const trigger = GetSpellInfo()->Effects[aurEff->GetEffIndex()].TriggerSpell;
        if (!caster || !target || !target->IsAlive() || !trigger)
            return;
        caster->CastSpell(target, trigger, true);
        // a new companion starts Defensive (it joins what its hunter attacks; the core leaves a tamed pet passive), the
        // pet bar told
        if (Player* player = caster->ToPlayer())
            if (Pet* pet = player->GetPet(); pet && pet->getPetType() == HUNTER_PET)
            {
                pet->SetReactState(REACT_DEFENSIVE);
                pet->SavePetToDB(PET_SAVE_AS_CURRENT);
                player->PetSpellInitialize();
            }
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(spell_ef_kit_gentling_aura::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY,
                                                AURA_EFFECT_HANDLE_REAL);
    }
};

class spell_ef_pet_owner_scaling : public AuraScript
{
    PrepareAuraScript(spell_ef_pet_owner_scaling);

    Unit* Hunter() { return GetUnitOwner() ? GetUnitOwner()->GetOwner() : nullptr; }

    int32 Share(AuraEffect const* aurEff, float of)
    {
        int32 const pct = GetSpellInfo()->Effects[aurEff->GetEffIndex()].CalcValue();
        return int32(std::max(0.0f, of) * float(pct) / 100.0f);
    }

    void CalcAttackPower(AuraEffect const* aurEff, int32& amount, bool& canBeRecalculated)
    {
        canBeRecalculated = true;
        if (Unit* owner = Hunter())
            amount = Share(aurEff, owner->GetTotalAttackPowerValue(RANGED_ATTACK));
    }

    void CalcStat(AuraEffect const* aurEff, int32& amount, bool& canBeRecalculated)
    {
        canBeRecalculated = true;
        if (Unit* owner = Hunter())
            amount = Share(aurEff, owner->GetStat(Stats(aurEff->GetMiscValue())));
    }

    void CalcArmor(AuraEffect const* aurEff, int32& amount, bool& canBeRecalculated)
    {
        canBeRecalculated = true;
        if (Unit* owner = Hunter())
            amount = Share(aurEff, float(owner->GetArmor()));
    }

    void CalcPeriodic(AuraEffect const* /*aurEff*/, bool& isPeriodic, int32& amplitude)
    {
        isPeriodic = true;
        amplitude = 2 * IN_MILLISECONDS;
    }

    void HandlePeriodic(AuraEffect const* aurEff)
    {
        PreventDefaultAction();
        Unit* pet = GetUnitOwner();
        AuraEffect* eff = GetEffect(aurEff->GetEffIndex());
        if (!pet || !eff)
            return;
        // keep the health where it is when the stamina share moves (no free heal, no loss)
        uint32 const health = pet->GetHealth();
        eff->RecalculateAmount();
        if (aurEff->GetAuraType() == SPELL_AURA_MOD_STAT)
            pet->SetHealth(std::min<uint32>(pet->GetMaxHealth(), health));
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_ef_pet_owner_scaling::CalcAttackPower, EFFECT_ALL, SPELL_AURA_MOD_ATTACK_POWER);
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_ef_pet_owner_scaling::CalcStat, EFFECT_ALL, SPELL_AURA_MOD_STAT);
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_ef_pet_owner_scaling::CalcArmor, EFFECT_ALL, SPELL_AURA_MOD_RESISTANCE);
        DoEffectCalcPeriodic += AuraEffectCalcPeriodicFn(spell_ef_pet_owner_scaling::CalcPeriodic, EFFECT_ALL, SPELL_AURA_ANY);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_ef_pet_owner_scaling::HandlePeriodic, EFFECT_ALL, SPELL_AURA_ANY);
    }
};

// A companion always fights beside its hunter at the hunter's level (the core's level hunk, PATCHES.md #17, keeps it
// there afterwards). Its family abilities autocast from the start: the core learns them with autocast on (PATCHES.md
// #27); a loaded companion keeps its saved toggles. (This hook once turned them on here, but it runs before a loaded
// companion's spells arrive, so it never did.)
class emberfall_classkit_hunt_pet : public PetScript
{
public:
    emberfall_classkit_hunt_pet() : PetScript("emberfall_classkit_hunt_pet", { PETHOOK_ON_PET_ADD_TO_WORLD }) { }

    void OnPetAddToWorld(Pet* pet) override
    {
        if (!pet || pet->getPetType() != HUNTER_PET)
            return;
        Player* owner = pet->GetOwner();
        if (!owner || !Emberfall::ClassKit::KitOf(owner))
            return;
        if (pet->GetLevel() != owner->GetLevel())
            pet->GivePetLevel(owner->GetLevel());
    }
};

void AddEmberfallClassKitHuntScripts()
{
    RegisterSpellScript(spell_ef_kit_spend_self_stacks);
    RegisterSpellScript(spell_ef_kit_pet_strike);
    RegisterSpellScript(spell_ef_kit_dash);
    RegisterSpellAndAuraScriptPair(spell_ef_kit_gentling, spell_ef_kit_gentling_aura);
    RegisterSpellScript(spell_ef_pet_owner_scaling);
    new emberfall_classkit_hunt_pet();
}
