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

// Emberfall class kits: the Warrior's Corsair (Emberfall docs/design/classes/warrior-corsair.md: Flow, the combination of
// two different builders that prepares a finisher; the finisher button's faces; the damage over time split off a strike;
// the off-hand hit; the hook). Generic scripts, their numbers in the spells' own effects (Emberfall's classkit/model.py
// SCRIPTS checks the contracts); the arithmetic is classkit/EfFlowLogic.h (tests/classkit_tests.cpp).
//
//   spell_ef_kit_flow           a builder: a dummy on the caster with trigger = the builder's mark (a self aura). The mark's
//                               effect 1 misc is the gate (an aura of the caster's: a finisher is prepared), its effects 2-3
//                               name a partner's mark (misc) and the ready aura the pair prepares (trigger). After a cast
//                               that struck an enemy (the miss rules, alpha1 feedback #146: a miss, dodge, parry, nobody
//                               in reach or empty air changes nothing): with the gate up nothing changes; while an aura of the caster's has a dummy
//                               effect naming this mark (misc) with a trigger (No Quarter), that ready aura and the gate are
//                               cast; holding a partner's mark, every mark goes and the pair's ready aura and the gate are
//                               cast; else the mark is laid (refreshed).
//   spell_ef_kit_flow_clear     No Quarter: its dummy auras name the marks (misc) and the ready aura each builder prepares
//                               by itself (trigger, read by spell_ef_kit_flow); the cast takes the marks off.
//   spell_ef_kit_faces          a button with faces (Flow Finisher): dummy effects on the caster, misc = an aura of the
//                               caster's, trigger = the spell cast instead while it holds (the first one up). A face that
//                               needs an enemy takes the player's selected target (in reach, in front); the face's aura and
//                               the button's caster_aura_spell (the gate) are spent.
//   spell_ef_kit_offhand_strike a dummy at the enemy with trigger = the off-hand strike: cast at the target after the cast
//                               when the caster holds a usable off-hand weapon (Quickcut's second hit, its own crit).
//   spell_ef_kit_split_dot      effect 1 deals the strike; a dummy with trigger = the damage over time, value = the percent
//                               of the strike's base (effect 1's value with attack power, before the crit) it deals over
//                               the trigger's base duration. A periodic aura (Deep Wounds) goes to each enemy struck: misc =
//                               1: a crit doubles it; misc B = instances on one enemy (consecutive spell IDs from the trigger
//                               on; v2.0's Deep Wounds 1: a new one replaces the old and its time; all running, the one with
//                               the least time left is replaced), or, while the caster holds a passive dummy naming the
//                               trigger (misc) with misc B 0 and no trigger, that passive's value (Open Wounds: 6, every
//                               application its own). A persistent area aura (Razorstorm) once at the caster's feet, enemy
//                               or not; its misc (when set) a spell the caster must hold (v2.0: the Razorstorm talent).
//                               On the enemy the caster's passive dummies naming this spell (misc, no trigger) with misc B
//                               instances of the trigger's pool running add their value in percent to the strike (Crimson
//                               Edge, 1 running), those naming the trigger add it to its crit chance. The caster's
//                               `spell_ef_kit_rule_stack_bonus` rules naming this spell (value % a stack of their trigger, a
//                               self stacking aura: Crimson Undertow) raise the strike and its new damage over time; the
//                               cast spends those stacks, struck or not (the miss rules, alpha1 feedback #143).
//   spell_ef_kit_hook           effect 1, a dummy at the enemy: value = yards short of the caster it lands, misc B = the
//                               pull's speed (yd/s), trigger = the spell cast at it as the pull starts (Boarding Hook's
//                               cut: interrupt_cast with no lockout), its mechanic the immunity that refuses it (grip).
//                               Only on a regular creature (no player, elite, boss); v2.0: also while the caster moves. The
//                               caster's rules naming it: `spell_ef_kit_rule_hook_arrival` casts its trigger at the enemy as
//                               it arrives (Dragging Anchor's stun); `spell_ef_kit_rule_hook_rush` sends the caster to the
//                               enemy instead, stopping as far short, the enemy left where it stands and uncut, any enemy
//                               (Boarding Rush).
//   spell_ef_kit_line_behind    a dummy at the enemy with trigger = the spell cast at every other enemy on the line behind
//                               it (from the caster through the target), value = the line's length in yards, misc B = its
//                               half width (0: 2.5 yd), misc = a spell the caster must hold (0: always): once a cast, after
//                               the strike lands on its target (Full Piercing)
//   spell_ef_kit_barrage        effect 1, a dummy at the enemy: value = the strikes, misc B = ms between them, trigger = the
//                               strike (its own hit and crit roll), the first at once, the others while the enemy lives,
//                               in reach and in sight (Cutlass Barrage)
//   spell_ef_kit_channel_finale a channel's aura on its caster (an effect on the caster with a trigger: Whirlwind Assault's
//                               damage-taken aura): when it runs its full time (not interrupted) the trigger is cast (the
//                               final sweeping slash round the caster) and the caster's `spell_ef_kit_rule_cooldown_cut`
//                               rules naming the channel take their value off their trigger's cooldown (Relentless
//                               Onslaught: Cutlass Barrage ready again)

#include "EfClassKit.h"
#include "EfClassKitLogic.h"
#include "EfFlowLogic.h"

#include "CellImpl.h"
#include "Creature.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Item.h"
#include "MotionMaster.h"
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
#include <list>
#include <vector>

namespace
{
    namespace Flow = Emberfall::ClassKit::Flow;

    // The first effect of `info` with `effect` and a trigger whose implicit target is (or is not) the caster.
    SpellEffectInfo const* FindTriggerDummy(SpellInfo const* info, bool onCaster)
    {
        for (SpellEffectInfo const& eff : info->Effects)
            if (eff.Effect == SPELL_EFFECT_DUMMY && eff.TriggerSpell
                && (eff.TargetA.GetTarget() == TARGET_UNIT_CASTER) == onCaster)
                return &eff;
        return nullptr;
    }

    // The strike's base: effect `index`'s value (level growth in, random in its range) and the attack power share of the
    // spell's spell_bonus_data, before any crit or modifier.
    int32 StrikeBase(Unit* caster, SpellInfo const* info, uint8 index)
    {
        int32 base = info->Effects[index].CalcValue(caster);
        if (SpellBonusEntry const* bonus = sSpellMgr->GetSpellBonusData(info->Id); bonus && bonus->ap_bonus > 0.0f)
            base += int32(bonus->ap_bonus * caster->GetTotalAttackPowerValue(BASE_ATTACK));
        return base;
    }

    // The instances of a pool (`first` .. `first + count - 1`) the caster has on `target`, and each one's time left
    // (-1: free).
    std::vector<int32> PoolLeft(Unit* caster, Unit* target, uint32 first, int32 count)
    {
        std::vector<int32> left;
        for (int32 i = 0; i < count; ++i)
        {
            Aura const* aura = target->GetAura(first + uint32(i), caster->GetGUID());
            left.push_back(aura ? std::max(0, aura->GetDuration()) : -1);
        }
        return left;
    }

    int32 PoolRunning(std::vector<int32> const& left)
    {
        int32 n = 0;
        for (int32 l : left)
            n += l >= 0 ? 1 : 0;
        return n;
    }

    // The caster's passive dummy auras naming `spell` (misc, no trigger) whose misc B is at most `running`: their values summed.
    int32 PoolBonus(Unit* caster, uint32 spell, int32 running)
    {
        int32 sum = 0;
        for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
            if (uint32(eff->GetMiscValue()) == spell && eff->GetBase()->IsPassive() && eff->GetMiscValueB() > 0
                && eff->GetMiscValueB() <= running && !eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell)
                sum += eff->GetAmount();
        return sum;
    }
}

class spell_ef_kit_flow : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_flow);

    void After()
    {
        Unit* caster = GetCaster();
        SpellEffectInfo const* own = FindTriggerDummy(GetSpellInfo(), true);
        SpellInfo const* mark = own ? sSpellMgr->GetSpellInfo(own->TriggerSpell) : nullptr;
        if (!caster || !mark)
            return;
        // the miss rules (#146): a builder builds Flow only on an enemy it struck
        if (!Emberfall::ClassKit::Logic::Builds(Emberfall::ClassKit::StrikesOthers(GetSpellInfo()),
                                                Emberfall::ClassKit::Struck(GetSpell(), caster)))
            return;
        uint32 const gate = uint32(mark->Effects[EFFECT_0].MiscValue);
        // No Quarter: a non-passive aura of the caster's naming this mark with a ready aura (not a partner's mark, whose
        // effect 1 names the gate)
        uint32 overrideReady = 0;
        for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
        {
            uint32 const trigger = eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell;
            if (uint32(eff->GetMiscValue()) == mark->Id && trigger && !eff->GetBase()->IsPassive()
                && uint32(eff->GetSpellInfo()->Effects[EFFECT_0].MiscValue) != gate)
            {
                overrideReady = trigger;
                break;
            }
        }
        uint32 partnerReady = 0;
        for (uint8 i = EFFECT_1; i < MAX_SPELL_EFFECTS; ++i)
        {
            SpellEffectInfo const& eff = mark->Effects[i];
            if (eff.MiscValue && eff.TriggerSpell && caster->HasAura(uint32(eff.MiscValue)))
            {
                partnerReady = eff.TriggerSpell;
                break;
            }
        }
        // a gate without any prepared finisher is stale (a balance run once held one: every Flow Finisher refused and
        // every builder changed nothing): it goes, and the builder counts
        bool gateUp = gate && caster->HasAura(gate);
        if (gateUp && !AnyReady(caster, mark))
        {
            caster->RemoveAurasDueToSpell(gate);
            gateUp = false;
        }
        switch (Flow::NextStep(gateUp, overrideReady != 0, partnerReady != 0))
        {
            case Flow::Step::Nothing:
                return;
            case Flow::Step::Override:
                Prepare(caster, overrideReady, gate);
                return;
            case Flow::Step::Combine:
                caster->RemoveAurasDueToSpell(mark->Id);
                for (uint8 i = EFFECT_1; i < MAX_SPELL_EFFECTS; ++i)
                    if (mark->Effects[i].MiscValue)
                        caster->RemoveAurasDueToSpell(uint32(mark->Effects[i].MiscValue));
                Prepare(caster, partnerReady, gate);
                return;
            case Flow::Step::Mark:
                caster->CastSpell(caster, mark->Id, true);
                return;
        }
    }

    // Whether the caster holds a ready aura of the flow: one the mark's partners name, or one their marks name (the
    // three finishers).
    static bool AnyReady(Unit* caster, SpellInfo const* mark)
    {
        for (uint8 i = EFFECT_1; i < MAX_SPELL_EFFECTS; ++i)
        {
            SpellEffectInfo const& eff = mark->Effects[i];
            if (eff.TriggerSpell && caster->HasAura(eff.TriggerSpell))
                return true;
            if (SpellInfo const* partner = eff.MiscValue ? sSpellMgr->GetSpellInfo(uint32(eff.MiscValue)) : nullptr)
                for (uint8 j = EFFECT_1; j < MAX_SPELL_EFFECTS; ++j)
                    if (partner->Effects[j].TriggerSpell && caster->HasAura(partner->Effects[j].TriggerSpell))
                        return true;
        }
        return false;
    }

    static void Prepare(Unit* caster, uint32 ready, uint32 gate)
    {
        caster->CastSpell(caster, ready, true);
        if (gate)
            caster->CastSpell(caster, gate, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_ef_kit_flow::After);
    }
};

class spell_ef_kit_flow_clear : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_flow_clear);

    void After()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;
        for (SpellEffectInfo const& eff : GetSpellInfo()->Effects)
            if (eff.ApplyAuraName == SPELL_AURA_DUMMY && eff.MiscValue)
                caster->RemoveAurasDueToSpell(uint32(eff.MiscValue));
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_ef_kit_flow_clear::After);
    }
};

class spell_ef_kit_faces : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_faces);

    uint32 _face = 0;
    uint32 _faceAura = 0;
    ObjectGuid _target;

    // The face that holds: the first effect whose aura the caster carries.
    bool FindFace(Unit* caster)
    {
        for (SpellEffectInfo const& eff : GetSpellInfo()->Effects)
            if (eff.Effect == SPELL_EFFECT_DUMMY && eff.MiscValue && eff.TriggerSpell && caster->HasAura(uint32(eff.MiscValue)))
            {
                _face = eff.TriggerSpell;
                _faceAura = uint32(eff.MiscValue);
                return true;
            }
        return false;
    }

    SpellCastResult Check()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return SPELL_FAILED_CASTER_AURASTATE;
        if (!FindFace(caster))
        {
            // a stale gate (no face holds): it goes, so the button greys and the builders count again
            if (uint32 const gate = GetSpellInfo()->CasterAuraSpell)
                caster->RemoveAurasDueToSpell(gate);
            return SPELL_FAILED_CASTER_AURASTATE;
        }
        SpellInfo const* face = sSpellMgr->GetSpellInfo(_face);
        if (!face)
            return SPELL_FAILED_ERROR;
        if (face->Effects[EFFECT_0].TargetA.GetTarget() != TARGET_UNIT_TARGET_ENEMY)
            return SPELL_CAST_OK;
        Player* player = caster->ToPlayer();
        Unit* target = player ? player->GetSelectedUnit() : caster->GetVictim();
        if (!target || target == caster || !target->IsAlive() || !caster->IsValidAttackTarget(target))
            return SPELL_FAILED_BAD_TARGETS;
        bool const melee = face->RangeEntry && (face->RangeEntry->Flags & SPELL_RANGE_MELEE);
        if (melee ? !caster->IsWithinMeleeRange(target) : !caster->IsWithinDistInMap(target, face->GetMaxRange(false, caster)))
            return SPELL_FAILED_OUT_OF_RANGE;
        if (!caster->HasInArc(float(M_PI), target))
            return SPELL_FAILED_UNIT_NOT_INFRONT;
        if (!caster->IsWithinLOSInMap(target))
            return SPELL_FAILED_LINE_OF_SIGHT;
        _target = target->GetGUID();
        return SPELL_CAST_OK;
    }

    void After()
    {
        Unit* caster = GetCaster();
        if (!caster || !_face)
            return;
        Unit* target = _target ? ObjectAccessor::GetUnit(*caster, _target) : caster;
        if (!target)
            return;
        caster->RemoveAurasDueToSpell(_faceAura);
        if (uint32 const gate = GetSpellInfo()->CasterAuraSpell)
            caster->RemoveAurasDueToSpell(gate);
        caster->CastSpell(target, _face, true);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_ef_kit_faces::Check);
        AfterCast += SpellCastFn(spell_ef_kit_faces::After);
    }
};

class spell_ef_kit_offhand_strike : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_offhand_strike);

    void After()
    {
        Unit* caster = GetCaster();
        Unit* target = GetExplTargetUnit();
        SpellEffectInfo const* eff = FindTriggerDummy(GetSpellInfo(), false);
        Player* player = caster ? caster->ToPlayer() : nullptr;
        if (!player || !target || !eff || !target->IsAlive() || !player->GetWeaponForAttack(OFF_ATTACK, true))
            return;
        player->CastSpell(target, eff->TriggerSpell, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_ef_kit_offhand_strike::After);
    }
};

class spell_ef_kit_split_dot : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_split_dot);

    int32 _stackPct = 0;           // Crimson Undertow: percent more on this cast's strike and its new damage over time
    std::vector<uint32> _stackAuras; // the stack auras that bonus came from (spent by the cast, struck or not)

    SpellEffectInfo const* Split()
    {
        for (SpellEffectInfo const& eff : GetSpellInfo()->Effects)
            if (eff.Effect == SPELL_EFFECT_DUMMY && eff.TriggerSpell && eff.BasePoints > 0)
                return &eff;
        return nullptr;
    }

    // the instances of the trigger's pool: the dummy's misc B, or a passive's value while the caster holds it (Open Wounds)
    static int32 PoolSize(Unit* caster, SpellEffectInfo const& eff)
    {
        int32 extra = 0;
        if (caster)
            for (AuraEffect const* aura : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
                if (uint32(aura->GetMiscValue()) == eff.TriggerSpell && aura->GetMiscValueB() == 0 && aura->GetBase()->IsPassive()
                    && !aura->GetSpellInfo()->Effects[aura->GetEffIndex()].TriggerSpell)
                    extra = std::max(extra, aura->GetAmount());
        return Flow::PoolInstances(eff.MiscValueB, extra);
    }

    // the damage over time's tick for one strike
    int32 Tick(Unit* caster, SpellEffectInfo const& eff, SpellInfo const* dot, bool crit)
    {
        SpellEffectInfo const& periodic = dot->Effects[EFFECT_0];
        int32 const ticks = Flow::Ticks(dot->GetMaxDuration(), periodic.Amplitude);
        return Flow::SplitTick(StrikeBase(caster, GetSpellInfo(), EFFECT_0), eff.CalcValue(caster), ticks,
                               crit && eff.MiscValue == 1);
    }

    bool IsArea(SpellInfo const* dot) { return dot->Effects[EFFECT_0].Effect == SPELL_EFFECT_PERSISTENT_AREA_AURA; }

    // Crimson Undertow: the caster's stack-bonus rules naming this spell, read once before the first hit
    void ReadStacks()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;
        for (AuraEffect const* eff : Emberfall::ClassKit::Rules(caster, "spell_ef_kit_rule_stack_bonus", GetSpellInfo()->Id))
        {
            uint32 const stackSpell = eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell;
            Aura const* stacks = stackSpell ? caster->GetAura(stackSpell, caster->GetGUID()) : nullptr;
            if (!stacks)
                continue;
            _stackPct += Flow::StackBonusPct(stacks->GetStackAmount(), eff->GetAmount());
            _stackAuras.push_back(stackSpell);
        }
    }

    // more crit chance on an enemy with enough instances running (the launch rolled the crit already; the damage takes it
    // from the target info)
    void BeforeHitTarget(SpellMissInfo missInfo)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        SpellEffectInfo const* eff = Split();
        if (!caster || !target || target == caster || !eff || missInfo != SPELL_MISS_NONE)
            return;
        int32 const running = PoolRunning(PoolLeft(caster, target, eff->TriggerSpell, PoolSize(caster, *eff)));
        int32 const extra = PoolBonus(caster, eff->TriggerSpell, running);
        if (extra <= 0)
            return;
        for (TargetInfo& info : *GetSpell()->GetUniqueTargetInfo())
            if (info.targetGUID == target->GetGUID() && !info.crit)
            {
                float const base = caster->SpellDoneCritChance(target, GetSpellInfo(), GetSpellInfo()->GetSchoolMask(), BASE_ATTACK, false);
                info.crit = roll_chance_f(Flow::ExtraCritChance(base, float(extra)));
            }
    }

    // more damage on an enemy with enough instances running (Crimson Edge) and by the stacks spent (Crimson Undertow)
    void HitTarget()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        SpellEffectInfo const* eff = Split();
        if (!caster || !target || target == caster || !eff || GetHitDamage() <= 0)
            return;
        int32 const running = PoolRunning(PoolLeft(caster, target, eff->TriggerSpell, PoolSize(caster, *eff)));
        int32 const pct = PoolBonus(caster, GetSpellInfo()->Id, running) + _stackPct;
        if (pct > 0)
            SetHitDamage(Flow::WithPct(GetHitDamage(), pct));
    }

    // a periodic aura at each enemy struck: a new one, or its own instance
    void AfterHitTarget()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        SpellEffectInfo const* eff = Split();
        SpellInfo const* dot = eff ? sSpellMgr->GetSpellInfo(eff->TriggerSpell) : nullptr;
        if (!caster || !target || target == caster || !dot || IsArea(dot) || !target->IsAlive())
            return;
        bool crit = false, landed = false;
        for (TargetInfo const& info : *GetSpell()->GetUniqueTargetInfo())
            if (info.targetGUID == target->GetGUID())
            {
                crit = info.crit;
                landed = info.missCondition == SPELL_MISS_NONE;
            }
        if (!landed)
            return;
        int32 const tick = Flow::WithPct(Tick(caster, *eff, dot, crit), _stackPct);
        if (tick <= 0)
            return;
        std::vector<int32> const left = PoolLeft(caster, target, eff->TriggerSpell, PoolSize(caster, *eff));
        uint32 const spell = eff->TriggerSpell + uint32(Flow::PickInstance(left));
        target->RemoveAurasDueToSpell(spell, caster->GetGUID());
        caster->CastCustomSpell(target, spell, &tick, nullptr, nullptr, true);
    }

    // a persistent area aura once at the caster's feet, whatever was struck (with its spell held, when it names one); the
    // stacks read before the cast are spent, struck or not (the miss rules, #143; the strike keeps their percent)
    void AfterCastArea()
    {
        Unit* caster = GetCaster();
        if (caster)
            for (uint32 stackSpell : _stackAuras)
                caster->RemoveAurasDueToSpell(stackSpell);
        _stackAuras.clear();
        SpellEffectInfo const* eff = Split();
        SpellInfo const* dot = eff ? sSpellMgr->GetSpellInfo(eff->TriggerSpell) : nullptr;
        if (!caster || !dot || !IsArea(dot))
            return;
        if (eff->MiscValue > 1 && !caster->HasAura(uint32(eff->MiscValue)))
            return;
        int32 const tick = Tick(caster, *eff, dot, false);
        if (tick > 0)
            caster->CastCustomSpell(caster, dot->Id, &tick, nullptr, nullptr, true);
    }

    void Register() override
    {
        BeforeCast += SpellCastFn(spell_ef_kit_split_dot::ReadStacks);
        BeforeHit += BeforeSpellHitFn(spell_ef_kit_split_dot::BeforeHitTarget);
        OnHit += SpellHitFn(spell_ef_kit_split_dot::HitTarget);
        AfterHit += SpellHitFn(spell_ef_kit_split_dot::AfterHitTarget);
        AfterCast += SpellCastFn(spell_ef_kit_split_dot::AfterCastArea);
    }
};

class spell_ef_kit_hook : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_hook);

    // Boarding Rush: the caster travels instead (any enemy: it is not moved)
    bool Rush(Unit* caster)
    {
        return !Emberfall::ClassKit::Rules(caster, "spell_ef_kit_rule_hook_rush", GetSpellInfo()->Id).empty();
    }

    SpellCastResult Check()
    {
        Unit* caster = GetCaster();
        Unit* target = GetExplTargetUnit();
        if (!caster || !target)
            return SPELL_FAILED_BAD_TARGETS;
        // v2.0: usable while moving (the draft's "stand still" went)
        if (Rush(caster))
            return SPELL_CAST_OK;
        Creature* creature = target->ToCreature();
        if (!creature || creature->isElite() || creature->isWorldBoss() || Emberfall::ClassKit::IsBoss(creature))
            return SPELL_FAILED_BAD_TARGETS;
        if (creature->IsImmunedToSpellEffect(GetSpellInfo(), EFFECT_0, caster) || creature->HasUnitState(UNIT_STATE_ROOT))
            return SPELL_FAILED_IMMUNE;
        return SPELL_CAST_OK;
    }

    void HandlePull(SpellEffIndex effIndex)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || target == caster)
            return;
        SpellEffectInfo const& eff = GetSpellInfo()->Effects[effIndex];
        float const stopShort = float(std::max(1, eff.CalcValue(caster)));
        float const travel = Flow::HookTravel(caster->GetExactDist2d(target), stopShort);
        float const speed = float(std::max(1, eff.MiscValueB));
        if (Rush(caster))
        {
            // Boarding Rush: the Corsair along the line to the enemy, stopping as far short; the enemy stays, uncut
            if (travel <= 0.5f)
                return;
            Position const dest = caster->GetFirstCollisionPosition(travel, caster->GetRelativeAngle(target));
            caster->GetMotionMaster()->MoveCharge(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), speed);
            return;
        }
        if (eff.TriggerSpell)
            caster->CastSpell(target, eff.TriggerSpell, true);       // the cut, as the pull starts
        if (travel > 0.5f)
        {
            Position const dest = caster->GetFirstCollisionPosition(stopShort, caster->GetRelativeAngle(target));
            target->GetMotionMaster()->MoveCharge(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), speed);
        }
        // Dragging Anchor: as it arrives
        uint32 const arrivalMs = uint32(travel / speed * 1000.0f) + 100;
        ObjectGuid const guid = target->GetGUID();
        for (AuraEffect const* rule : Emberfall::ClassKit::Rules(caster, "spell_ef_kit_rule_hook_arrival", GetSpellInfo()->Id))
            if (uint32 const trigger = rule->GetSpellInfo()->Effects[rule->GetEffIndex()].TriggerSpell)
                caster->m_Events.AddEventAtOffset([caster, guid, trigger]()
                {
                    if (!caster->IsInWorld())
                        return;
                    if (Unit* hooked = ObjectAccessor::GetUnit(*caster, guid); hooked && hooked->IsAlive())
                        caster->CastSpell(hooked, trigger, true);
                }, Milliseconds(arrivalMs));
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_ef_kit_hook::Check);
        OnEffectHitTarget += SpellEffectFn(spell_ef_kit_hook::HandlePull, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

class spell_ef_kit_line_behind : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_line_behind);

    bool _done = false;

    void AfterHitTarget()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (_done || !caster || !target || target == caster || target != GetExplTargetUnit())
            return;
        _done = true;
        for (SpellEffectInfo const& eff : GetSpellInfo()->Effects)
        {
            if (eff.Effect != SPELL_EFFECT_DUMMY || !eff.TriggerSpell || eff.TargetA.GetTarget() == TARGET_UNIT_CASTER)
                continue;
            if (eff.MiscValue > 0 && !caster->HasAura(uint32(eff.MiscValue)))
                continue;
            float const length = float(std::max(1, eff.CalcValue(caster)));
            float const halfWidth = eff.MiscValueB > 0 ? float(eff.MiscValueB) : 2.5f;
            std::list<Unit*> near;
            Acore::AnyUnfriendlyUnitInObjectRangeCheck check(target, caster, length + halfWidth);
            Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(target, near, check);
            Cell::VisitObjects(target, searcher, length + halfWidth);
            for (Unit* unit : near)
            {
                if (unit == target || !unit->IsAlive() || !caster->IsValidAttackTarget(unit) || !target->IsWithinLOSInMap(unit)
                    || !Emberfall::ClassKit::Logic::BehindOnLine(caster->GetPositionX(), caster->GetPositionY(),
                                                                 target->GetPositionX(), target->GetPositionY(),
                                                                 unit->GetPositionX(), unit->GetPositionY(), length,
                                                                 halfWidth, unit->GetCombatReach()))
                    continue;
                caster->CastSpell(unit, eff.TriggerSpell, true);
            }
        }
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_ef_kit_line_behind::AfterHitTarget);
    }
};

class spell_ef_kit_barrage : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_barrage);

    void HandleStrikes(SpellEffIndex effIndex)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        SpellEffectInfo const& eff = GetSpellInfo()->Effects[effIndex];
        if (!caster || !target || target == caster || !eff.TriggerSpell)
            return;
        int32 const strikes = std::clamp(eff.CalcValue(caster), 1, 20);
        uint32 const strike = eff.TriggerSpell;
        caster->CastSpell(target, strike, true);
        float const reach = GetSpellInfo()->GetMaxRange(false, caster) + 2.0f;
        ObjectGuid const guid = target->GetGUID();
        for (int32 i = 1; i < strikes; ++i)
            caster->m_Events.AddEventAtOffset([caster, guid, strike, reach]()
            {
                if (!caster->IsInWorld() || !caster->IsAlive())
                    return;
                Unit* enemy = ObjectAccessor::GetUnit(*caster, guid);
                if (!enemy || !enemy->IsAlive() || !caster->IsWithinDistInMap(enemy, reach) || !caster->IsWithinLOSInMap(enemy)
                    || !caster->IsValidAttackTarget(enemy))
                    return;
                caster->CastSpell(enemy, strike, true);
            }, Milliseconds(Flow::BarrageDelay(i, eff.MiscValueB)));
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_ef_kit_barrage::HandleStrikes, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

class spell_ef_kit_channel_finale : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_channel_finale);

    // the channel's effect on its caster that carries the finale (a trigger)
    static uint8 FinaleIndex(uint32 spellId)
    {
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId))
            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                if (info->Effects[i].IsAura() && info->Effects[i].TargetA.GetTarget() == TARGET_UNIT_CASTER
                    && info->Effects[i].TriggerSpell)
                    return i;
        return EFFECT_0;
    }

    void HandleRemove(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
    {
        Unit* caster = GetCaster();
        if (!caster || GetUnitOwner() != caster || GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_EXPIRE)
            return;
        uint32 const id = GetSpellInfo()->Id;
        if (uint32 const finale = GetSpellInfo()->Effects[aurEff->GetEffIndex()].TriggerSpell)
            caster->CastSpell(caster, finale, true);
        for (AuraEffect const* rule : Emberfall::ClassKit::Rules(caster, "spell_ef_kit_rule_cooldown_cut", id))
            if (uint32 const trigger = rule->GetSpellInfo()->Effects[rule->GetEffIndex()].TriggerSpell; trigger && rule->GetAmount() < 0)
                Emberfall::ClassKit::ShortenCooldown(caster, trigger, -rule->GetAmount());
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(spell_ef_kit_channel_finale::HandleRemove, FinaleIndex(m_scriptSpellId),
                                                SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

void AddEmberfallClassKitFlowScripts()
{
    RegisterSpellScript(spell_ef_kit_flow);
    RegisterSpellScript(spell_ef_kit_flow_clear);
    RegisterSpellScript(spell_ef_kit_faces);
    RegisterSpellScript(spell_ef_kit_offhand_strike);
    RegisterSpellScript(spell_ef_kit_split_dot);
    RegisterSpellScript(spell_ef_kit_hook);
    RegisterSpellScript(spell_ef_kit_line_behind);
    RegisterSpellScript(spell_ef_kit_barrage);
    RegisterSpellScript(spell_ef_kit_channel_finale);
}
