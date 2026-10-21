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

// Emberfall class kits, the Paladin's Wrathbringer as Nathanael's ability cards draw it (Emberfall
// docs/design/classes/paladin-wrathbringer.md §7, 2026-10-21): Condemned, the holy swords, Crown of Blades' launches,
// Solenne's Blessing and Solenne's Wrath, Last Sentence, Oathbound Rush. Generic like the scripts of
// src/emberfall_classkit.cpp: each reads its numbers from the spell's own effects (Emberfall's classkit/model.py SCRIPTS
// checks the contracts). Condemned's bonuses are data (mod_damage_from_caster with a class mask); Oathblade's sword and
// Hallowed Ground's burst are hit bonuses (EfClassKit.h HitBonuses: a class mask, the trigger's TargetAuraSpell).
//
//   spell_ef_kit_splash          effect 2 (school damage on the enemies round the target) leaves the target out and deals
//                                its misc value in percent (Sacred Brand: 50 % to the enemies near its target)
//   spell_ef_kit_launch_stack    the caster's launch auras whose class mask covers this spell launch their stacks
//                                (EfClassKit.h LaunchStacks: Crown of Blades' swords from Sacred Brand, Edict of Solenne,
//                                Oathblade one at a time, from Solenne's Wrath all at once)
//   spell_ef_kit_main_stat       mod_total_stat_percentage effects (one per stat): only the owner's main stat counts
//                                (EfClassKitLogic.h MainStat), stronger by the caster's empowerment (EfClassKit.h
//                                EmpowerPct: Solenne's Wrath doubles it); players only (Solenne's Blessing)
//   spell_ef_kit_empower         an aura whose dummy effects with misc = an aura spell of the caster's (no trigger) make
//                                that aura stronger by their value in percent while it holds; it recomputes it on the
//                                caster's group when it comes and goes (Solenne's Wrath on Solenne's Blessing)
//   spell_ef_kit_execute         effect 1's damage up to effect 2's value in percent more by the target's health missing,
//                                the full bonus at effect 2's misc B percent of health or below; effect 3: its value in
//                                percent of the spell's cost back when the strike kills (Last Sentence)
//   spell_ef_kit_cooldown_cut    every dummy effect with a trigger shortens the caster's cooldown of the trigger by -value ms
//                                when it hits; its misc (optional): an aura the caster casts on itself at the cast (Oathbound
//                                Rush: no slows during the charge, Oathblade 3 s sooner; Endless Crown's cut). A dummy with a
//                                trigger and value 0 casts the trigger at the unit it hits instead: Oathbound Rush's blow on
//                                arrival (a melee helper with the normal hit rules; the charge itself has damage class none
//                                and always lands, as the stock Warrior Charge does)

#include "EfClassKit.h"
#include "EfClassKitLogic.h"

#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"

// Sacred Brand: effect 2 strikes the enemies round the target, not the target itself, for its misc value in percent.
class spell_ef_kit_splash : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_splash);

    void FilterTargets(std::list<WorldObject*>& targets)
    {
        Unit* main = GetExplTargetUnit();
        targets.remove_if([main](WorldObject* object) { return object == main; });
    }

    void HandleSplash()
    {
        Unit* target = GetHitUnit();
        if (!target || target == GetExplTargetUnit() || target == GetCaster())
            return;
        int32 const pct = GetSpellInfo()->Effects[EFFECT_1].MiscValue;
        if (pct > 0 && GetHitDamage() > 0)
            SetHitDamage(Emberfall::ClassKit::Logic::PctOf(GetHitDamage(), pct));
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_ef_kit_splash::FilterTargets, EFFECT_1, TARGET_UNIT_DEST_AREA_ENEMY);
        OnHit += SpellHitFn(spell_ef_kit_splash::HandleSplash);
    }
};

// Crown of Blades' swords: launched by the spells their launch aura's class mask covers, once the cast is done.
class spell_ef_kit_launch_stack : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_launch_stack);

    void Launch()
    {
        Unit* caster = GetCaster();
        if (caster)
            Emberfall::ClassKit::LaunchStacks(caster, GetExplTargetUnit(), GetSpellInfo());
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_ef_kit_launch_stack::Launch);
    }
};

// Solenne's Blessing: of its stat effects, only the owner's main stat counts, stronger while the caster is empowered.
class spell_ef_kit_main_stat : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_main_stat);

    void CalcAmount(AuraEffect const* aurEff, int32& amount, bool& canBeRecalculated)
    {
        canBeRecalculated = true;
        Player* owner = GetUnitOwner() ? GetUnitOwner()->ToPlayer() : nullptr;
        if (!owner || aurEff->GetMiscValue() != Emberfall::ClassKit::Logic::MainStat(owner->getClass()))
        {
            amount = 0;
            return;
        }
        amount = Emberfall::ClassKit::Logic::Empowered(amount, Emberfall::ClassKit::EmpowerPct(GetCaster(), GetId()));
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_ef_kit_main_stat::CalcAmount, EFFECT_ALL, SPELL_AURA_MOD_TOTAL_STAT_PERCENTAGE);
    }
};

// Solenne's Wrath: while it holds, the auras its dummy effects name are recomputed stronger (EfClassKit.h EmpowerPct).
class spell_ef_kit_empower : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_empower);

    void Refresh(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
    {
        SpellEffectInfo const& eff = GetSpellInfo()->Effects[aurEff->GetEffIndex()];
        if (eff.TriggerSpell || !eff.MiscValue)
            return;
        if (Unit* caster = GetCaster())
            Emberfall::ClassKit::RefreshEmpowered(caster, uint32(eff.MiscValue));
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_ef_kit_empower::Refresh, EFFECT_ALL, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_ef_kit_empower::Refresh, EFFECT_ALL, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Last Sentence: more damage by the health the target is missing; part of its cost back when it kills.
class spell_ef_kit_execute : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_execute);

    void HandleHit()
    {
        Unit* target = GetHitUnit();
        Unit* caster = GetCaster();
        if (!target || !caster || target == caster || GetHitDamage() <= 0)
            return;
        SpellEffectInfo const& bonus = GetSpellInfo()->Effects[EFFECT_1];
        int32 const pct = Emberfall::ClassKit::Logic::ExecuteBonusPct(target->GetHealthPct(), bonus.CalcValue(caster), bonus.MiscValueB);
        if (pct > 0)
            SetHitDamage(Emberfall::ClassKit::Logic::Empowered(GetHitDamage(), pct));
    }

    void AfterHitTarget()
    {
        Unit* target = GetHitUnit();
        Unit* caster = GetCaster();
        if (!target || !caster || target == caster || target->IsAlive())
            return;
        int32 const back = Emberfall::ClassKit::Logic::PctOf(GetSpell()->GetPowerCost(), GetSpellInfo()->Effects[EFFECT_2].CalcValue(caster));
        if (back > 0)
            caster->EnergizeBySpell(caster, GetSpellInfo()->Id, uint32(back), POWER_MANA);
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_ef_kit_execute::HandleHit);
        AfterHit += SpellHitFn(spell_ef_kit_execute::AfterHitTarget);
    }
};

// Oathbound Rush (and Endless Crown's cut): a dummy with a trigger takes -value ms off the trigger's cooldown when it hits;
// its misc is an aura the caster takes at the cast (the charge's slow immunity). A dummy with a trigger and value 0 casts
// the trigger at the unit it hits: the charge's blow, when the charge (speed > 0: its hit waits for the arrival) lands.
// SPELL_EFFECT_TRIGGER_SPELL would not do: it is handled at the launch, before the charge has run.
class spell_ef_kit_cooldown_cut : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_cooldown_cut);

    void AtCast()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;
        for (SpellEffectInfo const& eff : GetSpellInfo()->Effects)
            if (eff.Effect == SPELL_EFFECT_DUMMY && eff.TriggerSpell && eff.MiscValue > 0)
                caster->CastSpell(caster, uint32(eff.MiscValue), true);
    }

    void HandleDummy(SpellEffIndex effIndex)
    {
        SpellEffectInfo const& eff = GetSpellInfo()->Effects[effIndex];
        if (!eff.TriggerSpell)
            return;
        if (GetEffectValue() < 0)
            Emberfall::ClassKit::ShortenCooldown(GetCaster(), eff.TriggerSpell, -GetEffectValue());
        else if (GetEffectValue() == 0)
        {
            Unit* caster = GetCaster();
            Unit* target = GetHitUnit();
            if (caster && target && target != caster && target->IsAlive())
                caster->CastSpell(target, eff.TriggerSpell, true);
        }
    }

    void Register() override
    {
        OnCast += SpellCastFn(spell_ef_kit_cooldown_cut::AtCast);
        OnEffectHitTarget += SpellEffectFn(spell_ef_kit_cooldown_cut::HandleDummy, EFFECT_ALL, SPELL_EFFECT_DUMMY);
    }
};

void AddEmberfallClassKitWrathScripts()
{
    RegisterSpellScript(spell_ef_kit_splash);
    RegisterSpellScript(spell_ef_kit_launch_stack);
    RegisterSpellScript(spell_ef_kit_main_stat);
    RegisterSpellScript(spell_ef_kit_empower);
    RegisterSpellScript(spell_ef_kit_execute);
    RegisterSpellScript(spell_ef_kit_cooldown_cut);
}
