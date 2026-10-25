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


// Emberfall class kits: the Warrior's Executioner v2.0 (Emberfall docs/design/classes/warrior-executioner.md, the
// designers' handoff of 2026-10-24: personal Openings, the spec power, max 5). Generic scripts, their numbers in the
// spells' own effects (Emberfall's classkit/model.py SCRIPTS checks the contracts); the arithmetic is EfClassKitLogic.h
// (tests/classkit_tests.cpp).
//
//   spell_ef_kit_finisher   a spender of the caster's spec power (its Spell.dbc cost: the core asks for the cost and
//                           takes exactly it, 3 Openings) whose every hit on an enemy is a sure critical strike (so it opens
//                           Deathblow). A dummy effect on the caster with misc = an aura of the caster's makes the cast
//                           free (Perfect Execution: read before the cast; the cost is still asked for, then given back,
//                           and the aura goes). The caster's passive dummy auras with misc = this spell and no trigger give
//                           their value in points back once the finisher struck an enemy, only for a finisher that paid
//                           (not a free one) and, with misc B, only on an enemy below misc B % health (Headsman's Due: 1
//                           Opening below 35 %; playtest #69: a Final Verdict that refunds on every cast and a Deathblow
//                           +2 kept the warrior on finishers alone) (Final Verdict, Whirling Steel)
//   spell_ef_kit_sweep      a dummy on the caster with misc = an aura of the caster's that makes the strike sweep, trigger
//                           = the spell cast at the other enemies, value = how many (spell mods in: Sweeping Reach), misc B =
//                           yards round the target: after the hit on the target, while the caster holds the aura (it is
//                           not spent: Sweeping Deathblow's 6 s), the trigger strikes that many other enemies nearest the
//                           target, once a cast (Deathblow)
//   spell_ef_kit_sweep_hit  effect 1 deals the damage, its misc = the percent of it that lands (Sweeping Deathblow's other
//                           targets: 50 % of Deathblow's own numbers; Red Harvest's damage mod on top: 75 %)

#include "EfClassKit.h"
#include "EfClassKitLogic.h"

#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "ObjectAccessor.h"
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

    uint32 _free = 0;          // the aura that made this cast free (Perfect Execution), 0: none

    // the dummy on the caster whose misc names the aura that makes the cast free
    uint32 FreeAura()
    {
        for (SpellEffectInfo const& eff : GetSpellInfo()->Effects)
            if (eff.Effect == SPELL_EFFECT_DUMMY && eff.TargetA.GetTarget() == TARGET_UNIT_CASTER && eff.MiscValue > 0)
                return uint32(eff.MiscValue);
        return 0;
    }

    void Before()
    {
        Unit* caster = GetCaster();
        uint32 const free = FreeAura();
        _free = caster && free && caster->HasAura(free) ? free : 0;
    }

    // the sure crit on every enemy struck: the launch rolled the crit already, the damage takes it from the target info
    void BeforeHitTarget(SpellMissInfo missInfo)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || target == caster || missInfo != SPELL_MISS_NONE)
            return;
        for (TargetInfo& info : *GetSpell()->GetUniqueTargetInfo())
            if (info.targetGUID == target->GetGUID())
                info.crit = true;
    }

    void Finish()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;
        Powers const slot = Powers(GetSpellInfo()->PowerType);
        int32 const scale = Emberfall::ClassKit::PowerScale(uint8(slot));
        // Perfect Execution: the 3 Openings the core took come back, and the buff is spent by this finisher
        if (_free)
        {
            if (int32 const cost = GetSpell()->GetPowerCost(); cost > 0)
                caster->ModifyPower(slot, cost);
            caster->RemoveAurasDueToSpell(_free);
        }
        // Headsman's Due: points back once a finisher that paid struck an enemy (below misc B % health)
        if (_free)
            return;
        float lowest = 101.0f;   // the lowest health % of the enemies struck
        for (TargetInfo const& info : *GetSpell()->GetUniqueTargetInfo())
            if (info.targetGUID != caster->GetGUID() && info.missCondition == SPELL_MISS_NONE)
                if (Unit* struck = ObjectAccessor::GetUnit(*caster, info.targetGUID))
                    lowest = std::min(lowest, struck->GetHealthPct());
        if (lowest > 100.0f)
            return;
        int32 back = 0;
        for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
            if (uint32(eff->GetMiscValue()) == GetSpellInfo()->Id && !eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell
                && eff->GetBase()->IsPassive() && Emberfall::ClassKit::Logic::BelowHealth(lowest, eff->GetMiscValueB()))
                back = std::max(back, eff->GetAmount());
        if (back > 0)
            caster->ModifyPower(slot, back * scale);   // SetPower's maximum keeps it at 5
    }

    void Register() override
    {
        BeforeCast += SpellCastFn(spell_ef_kit_finisher::Before);
        BeforeHit += BeforeSpellHitFn(spell_ef_kit_finisher::BeforeHitTarget);
        AfterCast += SpellCastFn(spell_ef_kit_finisher::Finish);
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
            if (!caster->HasAura(uint32(eff.MiscValue)))
                continue;
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
            others.resize(size_t(Emberfall::ClassKit::Logic::SweepCount(int32(others.size()), eff.CalcValue(caster))));
            for (Unit* unit : others)
                caster->CastSpell(unit, eff.TriggerSpell, true);
        }
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_ef_kit_sweep::AfterHitTarget);
    }
};

class spell_ef_kit_sweep_hit : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_sweep_hit);

    void HandleHit()
    {
        Unit* target = GetHitUnit();
        if (!target || target == GetCaster() || GetHitDamage() <= 0)
            return;
        SetHitDamage(Emberfall::ClassKit::Logic::SweepDamage(GetHitDamage(), GetSpellInfo()->Effects[EFFECT_0].MiscValue));
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_ef_kit_sweep_hit::HandleHit);
    }
};

void AddEmberfallClassKitExecScripts()
{
    RegisterSpellScript(spell_ef_kit_finisher);
    RegisterSpellScript(spell_ef_kit_sweep);
    RegisterSpellScript(spell_ef_kit_sweep_hit);
}
