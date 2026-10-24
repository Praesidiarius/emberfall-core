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

// Emberfall class kits: the Warrior's Executioner (Emberfall docs/design/classes/warrior-executioner.md: Openings on the
// enemy, the finishers that consume them, Deathblow's sweep). Generic scripts, their numbers in the spells' own effects
// (Emberfall's classkit/model.py SCRIPTS checks the contracts); the arithmetic is EfClassKitLogic.h
// (tests/classkit_tests.cpp).
//
//   spell_ef_kit_finisher   effect 1 deals the damage; effect 2 (dummy on the caster): misc = the stacking debuff it
//                           consumes (Openings), value = percent more damage per stack the enemy carries (spell mods in),
//                           misc B = the stacks at which the hit is a sure critical strike (3); effect 3 (dummy on the
//                           caster, optional): misc = an aura of the caster's that keeps the stacks (Perfect Execution,
//                           whose own spell mod makes the cast free; read before the cast spends its charge). Every
//                           enemy struck: its stacks of the caster's debuff raise the damage, at misc B the hit crits,
//                           then the stacks go, all but those a caster passive leaves: a dummy aura with misc = this spell,
//                           no trigger, value = stacks left when the enemy carried at least misc B (Headsman's Due)
//                           (Final Verdict, Whirling Steel)
//   spell_ef_kit_sweep      a dummy on the caster with misc = an aura of the caster's that readies the sweep, trigger =
//                           the spell cast at the other enemies, value = how many (spell mods in: Sweeping Reach), misc B =
//                           yards round the target: after the hit on the target, while the caster holds the aura, the
//                           trigger strikes that many other enemies nearest the target and the aura is spent (Deathblow
//                           after a Whirling Steel crit: Sweeping Deathblow)

#include "EfClassKit.h"
#include "EfClassKitLogic.h"

#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
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

class spell_ef_kit_finisher : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_finisher);

    bool _keep = false;

    uint32 StackSpell() { return uint32(GetSpellInfo()->Effects[EFFECT_1].MiscValue); }
    int32 SureCritAt() { return GetSpellInfo()->Effects[EFFECT_1].MiscValueB; }

    uint32 StacksOf(Unit* caster, Unit* target)
    {
        Aura const* aura = target->GetAura(StackSpell(), caster->GetGUID());
        return aura ? aura->GetStackAmount() : 0;
    }

    // Perfect Execution: read before the cast, whose cost mod spends its charge
    void Before()
    {
        Unit* caster = GetCaster();
        uint32 const keep = GetSpellInfo()->Effects[EFFECT_2].Effect == SPELL_EFFECT_DUMMY
            ? uint32(GetSpellInfo()->Effects[EFFECT_2].MiscValue) : 0;
        _keep = caster && keep && caster->HasAura(keep);
    }

    // the sure crit at misc B stacks: the launch rolled the crit already, the damage takes it from the target info
    void BeforeHitTarget(SpellMissInfo missInfo)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || target == caster || missInfo != SPELL_MISS_NONE)
            return;
        if (!Emberfall::ClassKit::Logic::FinisherSureCrit(StacksOf(caster, target), SureCritAt()))
            return;
        for (TargetInfo& info : *GetSpell()->GetUniqueTargetInfo())
            if (info.targetGUID == target->GetGUID())
                info.crit = true;
    }

    void HitTarget()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || target == caster)
            return;
        uint32 const stacks = StacksOf(caster, target);
        if (!stacks)
            return;
        int32 const pct = GetSpellInfo()->Effects[EFFECT_1].CalcValue(caster);
        SetHitDamage(Emberfall::ClassKit::Logic::FinisherDamage(GetHitDamage(), pct, stacks));
        // Headsman's Due: a passive dummy naming this spell leaves its amount of stacks at misc B or more
        int32 leave = 0, leaveAt = 0;
        for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
            if (uint32(eff->GetMiscValue()) == GetSpellInfo()->Id && !eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell)
            {
                leave = std::max(leave, eff->GetAmount());
                leaveAt = eff->GetMiscValueB();
            }
        uint32 const left = Emberfall::ClassKit::Logic::FinisherStacksLeft(stacks, _keep, leave, leaveAt);
        if (left < stacks)
            Emberfall::ClassKit::TakeStacks(caster, target, StackSpell(), stacks - left);
    }

    void Register() override
    {
        BeforeCast += SpellCastFn(spell_ef_kit_finisher::Before);
        BeforeHit += BeforeSpellHitFn(spell_ef_kit_finisher::BeforeHitTarget);
        OnHit += SpellHitFn(spell_ef_kit_finisher::HitTarget);
    }
};

class spell_ef_kit_sweep : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_sweep);

    bool _done = false;

    void AfterHitTarget()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (_done || !caster || !target || target == caster)
            return;
        _done = true;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            SpellEffectInfo const& eff = GetSpellInfo()->Effects[i];
            if (eff.Effect != SPELL_EFFECT_DUMMY || !eff.MiscValue || !eff.TriggerSpell || eff.MiscValueB <= 0)
                continue;
            uint32 const ready = uint32(eff.MiscValue);
            if (!caster->HasAura(ready))
                continue;
            int32 const count = eff.CalcValue(caster);
            float const radius = float(eff.MiscValueB);
            std::list<Unit*> near;
            Acore::AnyUnfriendlyUnitInObjectRangeCheck check(target, caster, radius);
            Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(target, near, check);
            Cell::VisitObjects(target, searcher, radius);
            std::vector<Unit*> others;
            for (Unit* unit : near)
                if (unit != target && unit->IsAlive() && caster->IsValidAttackTarget(unit) && target->IsWithinLOSInMap(unit))
                    others.push_back(unit);
            std::sort(others.begin(), others.end(), [target](Unit* a, Unit* b)
            {
                return target->GetExactDist2dSq(a) < target->GetExactDist2dSq(b);
            });
            if (int32(others.size()) > count)
                others.resize(std::max(0, count));
            caster->RemoveAurasDueToSpell(ready);
            for (Unit* unit : others)
                caster->CastSpell(unit, eff.TriggerSpell, true);
        }
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_ef_kit_sweep::AfterHitTarget);
    }
};

void AddEmberfallClassKitExecScripts()
{
    RegisterSpellScript(spell_ef_kit_finisher);
    RegisterSpellScript(spell_ef_kit_sweep);
}
