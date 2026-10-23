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

// Emberfall class kits, the Druid's (Emberfall docs/design/classes/druid-stormbringer.md): a spec's own power (Static)
// that builders build and spenders spend, and the class's crowd control and cooldown rules. Generic like the scripts of
// src/emberfall_classkit.cpp: each reads its numbers from the spell's own effects (Emberfall's classkit/model.py SCRIPTS
// checks the contracts):
//
//   spell_ef_kit_build_power      the first dummy effect on the caster builds its value in points of the caster's spec
//                                 power once the cast is done (misc B > 0: per enemy struck, at most misc B enemies;
//                                 nothing when the enemy effects struck nobody); with the talent of its misc known and its
//                                 trigger (the fight's marker) not on the caster, double, and the marker is cast (First
//                                 Flash). The caster's hit bonuses for this spell run on every enemy struck (Crackling Air,
//                                 Rising Air; self triggers once).
//   spell_ef_kit_power_spender    a spender of the spec power (its Spell.dbc cost; damage, or since the Warlock healing:
//                                 Bequest and Dole, whose overspend and echo heal and which may target the caster): a dummy on the caster with misc = a
//                                 talent the caster knows (or = the spell itself: always, the Warlock's Spoil) spends up to
//                                 its value in more points, misc B tenths of a percent more damage each (Heavy Rend); a
//                                 dummy on the caster without misc gives its value (spell
//                                 mods in: Lightning Rod) back on a critical strike, once per cast. Spend bonuses: the
//                                 caster's passive dummy auras with misc = this spell and a trigger, when the power before
//                                 the cast was at least their misc B: a trigger whose effect 1 is school damage is an echo
//                                 (their value in percent of the damage to the first enemy, at that enemy, or at its spot
//                                 for an area trigger, after the trigger's effect 2 value in ms: Overcast, Thunder Roll);
//                                 any other is a hit bonus (their value a chance; self triggers once: Unbroken Storm,
//                                 Rain-Lashed).
//   spell_ef_kit_gain_power       effect 1 (dummy on the caster): the value it is cast with in points of the spec power
//                                 (Static Skin, Stored Charge)
//   spell_ef_kit_charges          a dummy effect with misc = the talent, value = the charges, trigger = the charge aura (a
//                                 stacking aura):
//                                 with the talent known the spell holds that many charges (EfClassKit.h SpendCharge;
//                                 Downburst + Second Gust)
//   spell_ef_kit_charge_aura      the charge aura: when it runs out a charge is back (EfClassKit.h ChargeBack)
//   spell_ef_kit_proc_while       a proc_trigger_spell aura that procs only while its owner has the aura of its misc; the
//                                 trigger is cast with the aura's value (Static Skin while Galeward holds)
//   spell_ef_kit_end_bonuses      when the aura ends, the caster's talents that name it (EfClassKit.h EndBonuses: Stored
//                                 Charge, Slow Waking, Echoing Peal)
//   spell_ef_kit_limit_targets    effect 2 (dummy): at most its value enemies hold the aura at a time, misc B with the
//                                 talent of its misc known (Becalm: one, two with Twin Calm); a new one frees the oldest
//   spell_ef_kit_hold_not_bosses  the spell's aura (its hold: a root, a stun) never stays on a boss; its damage still lands
//                                 and a boss may be its target (Glassroot)

#include "EfClassKit.h"
#include "EfClassKitLogic.h"

#include "DataMap.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"

#include <algorithm>
#include <deque>
#include <utility>
#include <map>

namespace
{
    class emberfall_classkit_power_player : public PlayerScript
    {
    public:
        emberfall_classkit_power_player()
            : PlayerScript("emberfall_classkit_power_player", { PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_AFTER_UPDATE_MAX_POWER }) { }

        void OnPlayerUpdate(Player* player, uint32 diff) override { Emberfall::ClassKit::UpdateSpecPower(player, diff); }

        // A class power's maximum (the Hunter's Focus, EfClassKit.h ClassPowerRule): the core's create power is 0 for a
        // player, so the rule's maximum comes in here, under the slot's max-power auras (Wide Lungs) the core summed.
        void OnPlayerAfterUpdateMaxPower(Player* player, Powers& power, float& value) override
        {
            if (Emberfall::ClassKit::KitOf(player))
                value += float(Emberfall::ClassKit::ClassPowerBase(player, uint8(power)));
        }
    };

    // The first dummy effect on the caster without a trigger-and-misc pair of a charge (the build effect), or -1.
    int8 BuildEffect(SpellInfo const* info)
    {
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            SpellEffectInfo const& eff = info->Effects[i];
            if (eff.Effect == SPELL_EFFECT_DUMMY && eff.TargetA.GetTarget() == TARGET_UNIT_CASTER)
                return int8(i);
        }
        return -1;
    }

    bool NeedsHit(SpellInfo const* info)
    {
        for (SpellEffectInfo const& eff : info->Effects)
            if (eff.Effect && eff.TargetA.GetTarget() != TARGET_UNIT_CASTER)
                return true;
        return false;
    }
}

class spell_ef_kit_build_power : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_build_power);

    uint32 _hits = 0;
    bool _first = true;

    void HandleHit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (caster && target && target != caster)
            ++_hits;
    }

    void AfterHitTarget()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || target == caster)
            return;
        Emberfall::ClassKit::HitBonuses(caster, target, GetSpellInfo()->Id, 0, _first);
        _first = false;
    }

    void Finish()
    {
        Unit* caster = GetCaster();
        int8 const index = BuildEffect(GetSpellInfo());
        if (!caster || index < 0)
            return;
        SpellEffectInfo const& eff = GetSpellInfo()->Effects[index];
        bool const first = eff.MiscValue && eff.TriggerSpell && caster->HasAura(uint32(eff.MiscValue))
                           && !caster->HasAura(eff.TriggerSpell);
        int32 const gain = Emberfall::ClassKit::Logic::PowerGain(eff.CalcValue(caster), eff.MiscValueB, _hits,
                                                                 NeedsHit(GetSpellInfo()), first);
        if (gain <= 0)
            return;
        if (Emberfall::ClassKit::GainSpecPower(caster, gain, GetSpellInfo()->Id) > 0 && first)
            caster->CastSpell(caster, eff.TriggerSpell, true);
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_ef_kit_build_power::HandleHit);
        AfterHit += SpellHitFn(spell_ef_kit_build_power::AfterHitTarget);
        AfterCast += SpellCastFn(spell_ef_kit_build_power::Finish);
    }
};

class spell_ef_kit_power_spender : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_power_spender);

    int32 _before = 0;        // the power before the cost (stored units)
    int32 _extra = 0;         // overspent points
    int32 _perPoint = 0;      // tenths of a percent each
    bool _first = true;
    bool _refunded = false;

    Powers Slot() { return Powers(GetSpellInfo()->PowerType); }

    // A healing spender (the Warlock's Bequest and Dole): the overspend raises the healing, the echo is a heal, and the
    // caster may be its own target.
    bool Heals() { return GetSpellInfo()->Effects[EFFECT_0].Effect == SPELL_EFFECT_HEAL; }

    int32 HitAmount() { return Heals() ? GetHitHeal() : GetHitDamage(); }

    void Spend()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;
        Powers const slot = Slot();
        _before = int32(caster->GetPower(slot));
        int32 const scale = Emberfall::ClassKit::PowerScale(slot);
        for (SpellEffectInfo const& eff : GetSpellInfo()->Effects)
        {
            // misc: the talent that enables it, or the spell itself for an overspend that is always on (Spoil)
            if (eff.Effect != SPELL_EFFECT_DUMMY || eff.TargetA.GetTarget() != TARGET_UNIT_CASTER || !eff.MiscValue
                || (uint32(eff.MiscValue) != GetSpellInfo()->Id && !caster->HasAura(uint32(eff.MiscValue))))
                continue;
            _extra = Emberfall::ClassKit::Logic::Overspend(eff.CalcValue(caster), _before, GetSpell()->GetPowerCost(), scale);
            _perPoint = eff.MiscValueB;
            if (_extra > 0)
                caster->ModifyPower(slot, -_extra * scale);
            break;
        }
    }

    void HandleHit()
    {
        Unit* target = GetHitUnit();
        if (!target || _extra <= 0)
            return;
        if (Heals())
        {
            SetHitHeal(Emberfall::ClassKit::Logic::OverspendDamage(GetHitHeal(), _extra, _perPoint));
            return;
        }
        if (target == GetCaster())
            return;
        SetHitDamage(Emberfall::ClassKit::Logic::OverspendDamage(GetHitDamage(), _extra, _perPoint));
    }

    bool Crit(Unit* target)
    {
        for (TargetInfo const& t : *GetSpell()->GetUniqueTargetInfo())
            if (t.targetGUID == target->GetGUID())
                return t.crit;
        return false;
    }

    void Echo(Unit* caster, Unit* target, uint32 trigger, int32 pct)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(trigger);
        int32 const damage = Emberfall::ClassKit::Logic::EchoDamage(HitAmount(), pct);
        if (!info || damage <= 0)
            return;
        int32 const delay = info->Effects[EFFECT_1].Effect == SPELL_EFFECT_DUMMY ? std::max(0, info->Effects[EFFECT_1].BasePoints + 1) : 0;
        Targets const at = info->Effects[EFFECT_0].TargetA.GetTarget();
        bool const atUnit = at == TARGET_UNIT_TARGET_ENEMY || at == TARGET_UNIT_TARGET_ALLY;
        ObjectGuid const victim = target->GetGUID();
        float const x = target->GetPositionX(), y = target->GetPositionY(), z = target->GetPositionZ();
        auto cast = [caster, victim, info, damage, atUnit, x, y, z]()
        {
            if (!caster->IsInWorld())
                return;
            CustomSpellValues values;
            values.AddSpellMod(SPELLVALUE_BASE_POINT0, damage);
            if (atUnit)
            {
                Unit* unit = ObjectAccessor::GetUnit(*caster, victim);
                if (unit && unit->IsAlive())
                    caster->CastCustomSpell(info, values, unit, TRIGGERED_FULL_MASK);
                return;
            }
            SpellCastTargets targets;
            targets.SetDst(x, y, z, 0.0f);
            caster->CastSpell(targets, info, &values, TRIGGERED_FULL_MASK);
        };
        if (delay > 0)
            caster->m_Events.AddEventAtOffset(std::move(cast), Milliseconds(delay));
        else
            cast();
    }

    void AfterHitTarget()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || (target == caster && !Heals()))
            return;
        bool const first = _first;
        _first = false;
        // Lightning Rod: points back on a critical strike, once per cast
        if (!_refunded && Crit(target))
            for (SpellEffectInfo const& eff : GetSpellInfo()->Effects)
                if (eff.Effect == SPELL_EFFECT_DUMMY && eff.TargetA.GetTarget() == TARGET_UNIT_CASTER && !eff.MiscValue)
                {
                    _refunded = true;
                    Emberfall::ClassKit::GainSpecPower(caster, eff.CalcValue(caster), GetSpellInfo()->Id);
                    break;
                }
        // spend bonuses
        int32 const shown = _before / Emberfall::ClassKit::PowerScale(Slot());
        struct Bonus
        {
            uint32 trigger;
            int32 amount;
        };
        std::vector<Bonus> bonuses;
        for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
        {
            uint32 const trigger = eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell;
            if (uint32(eff->GetMiscValue()) == GetSpellInfo()->Id && trigger && eff->GetBase()->IsPassive()
                && shown >= eff->GetMiscValueB())
                bonuses.push_back({ trigger, eff->GetAmount() });
        }
        for (Bonus const& b : bonuses)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(b.trigger);
            if (!info)
                continue;
            if (info->Effects[EFFECT_0].Effect == SPELL_EFFECT_SCHOOL_DAMAGE || info->Effects[EFFECT_0].Effect == SPELL_EFFECT_HEAL)
            {
                if (first)
                    Echo(caster, target, b.trigger, b.amount);
                continue;
            }
            bool const self = Emberfall::ClassKit::TargetsCaster(b.trigger);
            if (self && !first)
                continue;
            if (b.amount > 0 && b.amount < 100 && !roll_chance_i(b.amount))
                continue;
            if (self)
                caster->CastSpell(caster, b.trigger, true);
            else if (target->IsAlive())
                caster->CastSpell(target, b.trigger, true);
        }
    }

    void Register() override
    {
        OnCast += SpellCastFn(spell_ef_kit_power_spender::Spend);
        OnHit += SpellHitFn(spell_ef_kit_power_spender::HandleHit);
        AfterHit += SpellHitFn(spell_ef_kit_power_spender::AfterHitTarget);
    }
};

class spell_ef_kit_gain_power : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_gain_power);

    void HandleDummy(SpellEffIndex /*effIndex*/)
    {
        Emberfall::ClassKit::GainSpecPower(GetCaster(), GetEffectValue(), GetSpellInfo()->Id);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_ef_kit_gain_power::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

class spell_ef_kit_charges : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_charges);

    void Finish()
    {
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!player)
            return;
        // the charge effect: a dummy whose trigger is a stacking aura (the charge aura; a builder's First Flash marker is
        // a dummy with trigger and misc too, but its marker does not stack)
        for (SpellEffectInfo const& eff : GetSpellInfo()->Effects)
        {
            SpellInfo const* aura = eff.TriggerSpell ? sSpellMgr->GetSpellInfo(eff.TriggerSpell) : nullptr;
            if (eff.Effect == SPELL_EFFECT_DUMMY && aura && aura->StackAmount > 1 && eff.MiscValue)
            {
                if (player->HasAura(uint32(eff.MiscValue)))
                    Emberfall::ClassKit::SpendCharge(player, GetSpellInfo()->Id, eff.TriggerSpell, uint32(std::max(0, eff.CalcValue(player))));
                return;
            }
        }
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_ef_kit_charges::Finish);
    }
};

class spell_ef_kit_charge_aura : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_charge_aura);

    void Remove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_EXPIRE)
            return;
        Emberfall::ClassKit::ChargeBack(GetUnitOwner(), GetId(), GetAura()->GetStackAmount());
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(spell_ef_kit_charge_aura::Remove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

class spell_ef_kit_proc_while : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_proc_while);

    bool CheckProc(ProcEventInfo& /*info*/)
    {
        Unit* owner = GetUnitOwner();
        int32 const need = GetSpellInfo()->Effects[EFFECT_0].MiscValue;
        return owner && (!need || owner->HasAura(uint32(need)));
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& /*info*/)
    {
        PreventDefaultAction();
        Unit* owner = GetUnitOwner();
        uint32 const trigger = GetSpellInfo()->Effects[EFFECT_0].TriggerSpell;
        if (!owner || !trigger)
            return;
        int32 const amount = aurEff->GetAmount();
        owner->CastCustomSpell(owner, trigger, &amount, nullptr, nullptr, true);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_ef_kit_proc_while::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_ef_kit_proc_while::HandleProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

class spell_ef_kit_end_bonuses : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_end_bonuses);

    void Remove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Emberfall::ClassKit::EndBonuses(GetCaster(), GetUnitOwner(), GetId());
    }

    void Register() override
    {
        // once per aura: on its first aura effect
        uint8 first = EFFECT_0;
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(m_scriptSpellId))
            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                if (info->Effects[i].IsAura())
                {
                    first = i;
                    break;
                }
        AfterEffectRemove += AuraEffectRemoveFn(spell_ef_kit_end_bonuses::Remove, first, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

namespace
{
    // Per caster: the enemies holding each limited aura, oldest first.
    struct LimitedTargets : public DataMap::Base
    {
        std::map<uint32, std::deque<ObjectGuid>> bySpell;
    };
}

class spell_ef_kit_limit_targets : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_limit_targets);

    void AfterHitTarget()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        uint32 const spell = GetSpellInfo()->Id;
        if (!caster || !target || target == caster || !target->HasAura(spell, caster->GetGUID()))
            return;
        SpellEffectInfo const& rule = GetSpellInfo()->Effects[EFFECT_1];
        int32 limit = std::max(1, rule.CalcValue(caster));
        if (rule.MiscValue && rule.MiscValueB > 0 && caster->HasAura(uint32(rule.MiscValue)))
            limit = rule.MiscValueB;
        std::deque<ObjectGuid>& held = caster->CustomData.GetDefault<LimitedTargets>("ef_limited_targets")->bySpell[spell];
        held.erase(std::remove(held.begin(), held.end(), target->GetGUID()), held.end());
        // forget the ones the aura already left
        held.erase(std::remove_if(held.begin(), held.end(), [caster, spell](ObjectGuid const& guid)
        {
            Unit* unit = ObjectAccessor::GetUnit(*caster, guid);
            return !unit || !unit->HasAura(spell, caster->GetGUID());
        }), held.end());
        held.push_back(target->GetGUID());
        while (int32(held.size()) > limit)
        {
            if (Unit* oldest = ObjectAccessor::GetUnit(*caster, held.front()))
                oldest->RemoveAurasDueToSpell(spell, caster->GetGUID());
            held.pop_front();
        }
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_ef_kit_limit_targets::AfterHitTarget);
    }
};

// The hold never on bosses (Glassroot): unlike spell_ef_kit_not_bosses a boss may be the target and takes the damage;
// only the spell's aura (the root) comes off it at once.
class spell_ef_kit_hold_not_bosses : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_hold_not_bosses);

    void AfterHitTarget()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (caster && target && Emberfall::ClassKit::IsBoss(target))
            target->RemoveAurasDueToSpell(GetSpellInfo()->Id, caster->GetGUID());
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_ef_kit_hold_not_bosses::AfterHitTarget);
    }
};

void AddEmberfallClassKitPowerScripts()
{
    new emberfall_classkit_power_player();
    RegisterSpellScript(spell_ef_kit_build_power);
    RegisterSpellScript(spell_ef_kit_power_spender);
    RegisterSpellScript(spell_ef_kit_gain_power);
    RegisterSpellScript(spell_ef_kit_charges);
    RegisterSpellScript(spell_ef_kit_charge_aura);
    RegisterSpellScript(spell_ef_kit_proc_while);
    RegisterSpellScript(spell_ef_kit_hold_not_bosses);
    RegisterSpellScript(spell_ef_kit_end_bonuses);
    RegisterSpellScript(spell_ef_kit_limit_targets);
}
