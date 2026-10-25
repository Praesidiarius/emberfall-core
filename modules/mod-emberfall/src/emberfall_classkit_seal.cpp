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

// Emberfall class kits: the Warlock's Damnator (Emberfall docs/design/classes/warlock-damnator.md, ankarna's "Blood &
// Water Ritualist" v1.0): curses whose ticks make Blood Seals under one shared cooldown, a ritual that spends every Seal
// to lengthen and flare every curse, the detonation of what the curses still had to deal, curses copied round the
// target. Generic scripts, their numbers in the spells' own effects (Emberfall's classkit/scripts_seal.py checks the
// contracts); the arithmetic is classkit/EfSealLogic.h (tests/classkit_tests.cpp).
//
//   spell_ef_kit_seal_dot      a curse (a spell and aura script under one name). Effect 1 periodic damage at the enemy.
//                              Effect 2 a dummy on the caster: value = spec power points a generating tick gives (spell
//                              mods in: Drowned Tithe), trigger = the shared cooldown aura (a self helper: while the caster
//                              has it no tick of any curse gives power; cast at each generating tick for its own duration,
//                              or the window's effect 2 in ms while the window holds), misc = the window aura (Crimson
//                              Flood: its effect 1 the percent more tick damage), misc B = enemies at a time (0: any;
//                              Drowning Vow 1: a new target takes it off the old one). Effect 3 a dummy on the caster:
//                              value = the cap of a recast (spell_ef_kit_capped_extend), misc = the caster's partner aura
//                              beside which the ticks deal misc B percent more (Bloodbinding beside Crimson Brine), or a
//                              number 1-100: the health percent below which they do (Drowning Vow below 35 %). The
//                              caster's dummy auras naming the curse (misc): without a trigger they add their value to
//                              that bonus (Salt in the Wound, Last Breath); with a trigger and misc B 0 their value in
//                              percent more while the target lacks the caster's trigger aura (Cursed Shoals); with a
//                              trigger and misc B 1 the trigger is cast on the caster with their value in percent of the
//                              tick (Drink Deep). The multiplier is applied in the periodic damage hook (before
//                              mitigation, after the snapshot of spell power and spell mods), so Torrent's flares and the
//                              window count for curses cast before it. Every curse of a caster on any enemy is kept in a
//                              list on the caster (Torrent, Collapse: "regardless of distance", on the caster's map).
//   spell_ef_kit_torrent       a channel spending all of the spec power: effect 1 a periodic dummy on the caster (the
//                              flares), effect 2 a dummy at the enemy it starts at, value = ms added to every curse of
//                              the caster per point spent (Rising Water), past every cap; effect 3 a dummy on the caster,
//                              value = percent more damage of the flares (Red Wake). At the channel's start the power is
//                              taken and the cursed enemies are fixed; each period every curse of the caster still on them
//                              ticks once more (its own tick: the cooldown, the bonuses). Refused below 1 point.
//   spell_ef_kit_collapse      effect 1 a dummy on the caster: value = percent of what the curses still had to deal
//                              (Hungry Deep), trigger = the hit cast at each cursed enemy with that amount; effect 2: misc
//                              = a talent, trigger = a curse detonated but left (Unbroken Vow: Drowning Vow); effect 3:
//                              misc = a talent, value = seconds: the curses without a target limit come back with that
//                              much left (Second Wave). Remaining = the ticks left (rounded up) x the tick as it would
//                              land now. Refused when no curse of the caster is up.
//   spell_ef_kit_spread_dots   effect 1 a dummy at the enemy: value = how many more enemies (Spreading Stain), its radius
//                              round the target (Far Reach); effects 2 and 3 name the curses it copies (misc); effect 2's
//                              value = power per newly affected enemy, misc B = at most that many pay. The enemies lacking
//                              the most of the target's curses first, then the closest; each copy gets the target's time
//                              left and tick phase, never shortening a longer one. Refused without one of them on the
//                              target.
//   spell_ef_kit_marked_bonus  a dummy at the enemy names an aura of the caster's: on an enemy carrying it, the caster's
//                              dummy auras naming this spell without a trigger add their value in percent to its damage
//                              (Marked for the Deep on Bloodcurrent).

#include "EfClassKit.h"
#include "EfClassKitLogic.h"
#include "EfSealLogic.h"

#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
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
#include <deque>
#include <list>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace
{
    namespace Seal = Emberfall::ClassKit::Seal;

    // The spells that run spell_ef_kit_seal_dot (filled by the aura script's Validate at startup, read-only after).
    std::set<uint32> s_curses;

    // A Torrent flare's percent more while it forces a tick (Red Wake); the map thread that runs it.
    thread_local int32 t_flarePct = 0;

    char const* const kCursedKey = "ef_seal_cursed";
    char const* const kLimitKey = "ef_seal_limited";

    // Every curse of the caster on an enemy: (enemy, curse spell).
    struct Cursed : public DataMap::Base
    {
        std::set<std::pair<ObjectGuid, uint32>> held;
    };

    struct Limited : public DataMap::Base
    {
        std::map<uint32, std::deque<ObjectGuid>> bySpell;
    };

    struct Held
    {
        Unit* target;
        Aura* aura;
    };

    // The caster's curses still up on its map (the stale entries go).
    std::vector<Held> CursesOf(Unit* caster)
    {
        std::vector<Held> out;
        Cursed* cursed = caster ? caster->CustomData.Get<Cursed>(kCursedKey) : nullptr;
        if (!cursed)
            return out;
        for (auto it = cursed->held.begin(); it != cursed->held.end();)
        {
            Unit* target = ObjectAccessor::GetUnit(*caster, it->first);
            Aura* aura = target && target->IsInWorld() && target->IsAlive() ? target->GetAura(it->second, caster->GetGUID())
                                                                             : nullptr;
            if (!aura)
            {
                it = cursed->held.erase(it);
                continue;
            }
            out.push_back({ target, aura });
            ++it;
        }
        return out;
    }

    bool HasTrigger(AuraEffect const* eff)
    {
        return eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell != 0;
    }

    // The percent more a curse's tick deals on `target` now (without a flare's).
    int32 CurseBonus(Unit* caster, Unit* target, SpellInfo const* info, int32 flare)
    {
        if (!caster || !target || !info)
            return 0;
        SpellEffectInfo const& power = info->Effects[EFFECT_1];
        SpellEffectInfo const& rule = info->Effects[EFFECT_2];
        int32 conditional = rule.MiscValueB;
        int32 lacking = 0;
        for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
        {
            if (uint32(eff->GetMiscValue()) != info->Id)
                continue;
            uint32 const trigger = eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell;
            if (!trigger)
                conditional += eff->GetAmount();
            else if (eff->GetMiscValueB() == 0 && !target->HasAura(trigger, caster->GetGUID()))
                lacking += eff->GetAmount();
        }
        bool met = false;
        if (rule.MiscValue > 100)
            met = target->HasAura(uint32(rule.MiscValue), caster->GetGUID());
        else if (rule.MiscValue > 0)
            met = target->HealthBelowPct(rule.MiscValue);
        int32 window = 0;
        if (power.MiscValue > 0)
            if (Aura const* flood = caster->GetAura(uint32(power.MiscValue)))
                if (AuraEffect const* eff = flood->GetEffect(EFFECT_0))
                    window = eff->GetAmount();
        return Seal::TickBonus(met, conditional, lacking, window, flare);
    }

    // The tick a curse aura would deal now, before mitigation (Collapse; Drink Deep's share).
    int64 TickNow(Unit* caster, Unit* target, AuraEffect const* eff, int32 flare)
    {
        return Seal::WithBonus(std::max(0, eff->GetAmount()), CurseBonus(caster, target, eff->GetSpellInfo(), flare));
    }

    void Track(Unit* caster, Unit* target, uint32 spell, bool on)
    {
        if (!caster || !target)
            return;
        if (on)
            caster->CustomData.GetDefault<Cursed>(kCursedKey)->held.insert({ target->GetGUID(), spell });
        else if (Cursed* cursed = caster->CustomData.Get<Cursed>(kCursedKey))
            cursed->held.erase({ target->GetGUID(), spell });
    }
}

// ---- the curses -----------------------------------------------------------------------------------------------------

class spell_ef_kit_seal_dot : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_seal_dot);

    // misc B of effect 2: enemies at a time (Drowning Vow 1; a new target takes it off the oldest)
    void AfterHitTarget()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        uint32 const spell = GetSpellInfo()->Id;
        int32 const limit = GetSpellInfo()->Effects[EFFECT_1].MiscValueB;
        if (!caster || !target || target == caster || limit <= 0 || !target->HasAura(spell, caster->GetGUID()))
            return;
        std::deque<ObjectGuid>& held = caster->CustomData.GetDefault<Limited>(kLimitKey)->bySpell[spell];
        held.erase(std::remove(held.begin(), held.end(), target->GetGUID()), held.end());
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
        AfterHit += SpellHitFn(spell_ef_kit_seal_dot::AfterHitTarget);
    }
};

class aura_ef_kit_seal_dot : public AuraScript
{
    PrepareAuraScript(aura_ef_kit_seal_dot);

    bool Validate(SpellInfo const* spellInfo) override
    {
        s_curses.insert(spellInfo->Id);
        return true;
    }

    void Applied(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Track(GetCaster(), GetTarget(), GetId(), true);
    }

    void Removed(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Track(GetCaster(), GetTarget(), GetId(), false);
    }

    // inside AuraEffect::PeriodicTick, before the damage (a Torrent's flare too): the Seals and Drink Deep
    void Tick(AuraEffect const* aurEff)
    {
        Unit* caster = GetCaster();
        Unit* target = GetTarget();
        if (!caster || !target || !target->IsAlive())
            return;
        SpellEffectInfo const& power = GetSpellInfo()->Effects[EFFECT_1];
        uint32 const cooldown = power.TriggerSpell;
        if (cooldown && !caster->HasAura(cooldown))
        {
            Emberfall::ClassKit::GainSpecPower(caster, power.CalcValue(caster), GetId());
            caster->CastSpell(caster, cooldown, true);
            if (power.MiscValue > 0)
                if (Aura const* flood = caster->GetAura(uint32(power.MiscValue)))
                    if (AuraEffect const* eff = flood->GetEffect(EFFECT_1); eff && eff->GetAmount() > 0)
                        if (Aura* cd = caster->GetAura(cooldown))
                        {
                            cd->SetMaxDuration(eff->GetAmount());
                            cd->SetDuration(eff->GetAmount());
                        }
        }
        // Drink Deep: the caster's dummies naming this curse with a trigger and misc B 1
        std::vector<std::pair<uint32, int32>> heals;
        for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
            if (uint32(eff->GetMiscValue()) == GetId() && eff->GetMiscValueB() == 1 && HasTrigger(eff))
                heals.push_back({ eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell, eff->GetAmount() });
        if (heals.empty())
            return;
        int64 const tick = TickNow(caster, target, aurEff, t_flarePct);
        for (auto const& [trigger, pct] : heals)
        {
            int32 const amount = Seal::ShareOf(tick, pct);
            if (amount > 0)
                caster->CastCustomSpell(caster, trigger, &amount, nullptr, nullptr, true);
        }
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ef_kit_seal_dot::Applied, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ef_kit_seal_dot::Removed, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(aura_ef_kit_seal_dot::Tick, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE);
    }
};

// The curses' bonuses on each tick (before mitigation; the amount has the spell power and spell mods already).
class EmberfallSealUnitScript : public UnitScript
{
public:
    EmberfallSealUnitScript() : UnitScript("EmberfallSealUnitScript", true, { UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK }) { }

    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage, SpellInfo const* spellInfo) override
    {
        if (!target || !attacker || !spellInfo || !damage || !s_curses.count(spellInfo->Id))
            return;
        int32 const bonus = CurseBonus(attacker, target, spellInfo, t_flarePct);
        if (bonus)
            damage = uint32(std::min<int64>(Seal::WithBonus(damage, bonus), 0x7FFFFFFF));
    }
};

// ---- Sanguine Torrent ---------------------------------------------------------------------------------------------

class spell_ef_kit_torrent : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_torrent);

    SpellCastResult CheckCast()
    {
        return Emberfall::ClassKit::SpecPowerPoints(GetCaster()) >= 1 ? SPELL_CAST_OK : SPELL_FAILED_NO_POWER;
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_ef_kit_torrent::CheckCast);
    }
};

class aura_ef_kit_torrent : public AuraScript
{
    PrepareAuraScript(aura_ef_kit_torrent);

    std::vector<std::pair<ObjectGuid, uint32>> _fixed;

    // the channel starts: every point spent, the cursed enemies fixed, each curse on them lengthened past its cap
    void Started(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;
        int32 const seals = Emberfall::ClassKit::SpecPowerPoints(caster);
        Emberfall::ClassKit::SetSpecPowerPoints(caster, 0);
        int32 const ext = Seal::TorrentExtension(seals, GetSpellInfo()->Effects[EFFECT_1].CalcValue(caster));
        _fixed.clear();
        for (Held const& h : CursesOf(caster))
        {
            _fixed.push_back({ h.target->GetGUID(), h.aura->GetId() });
            if (ext <= 0)
                continue;
            int32 const total = Seal::Extended(h.aura->GetDuration(), ext);
            h.aura->SetMaxDuration(std::max(h.aura->GetMaxDuration(), total));
            h.aura->SetDuration(total);
        }
    }

    // a flare: every fixed curse still up ticks once more
    void Flare(AuraEffect const* /*aurEff*/)
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;
        int32 const flare = GetSpellInfo()->Effects[EFFECT_2].CalcValue(caster);
        std::vector<std::pair<ObjectGuid, uint32>> const fixed = _fixed;   // a tick may kill and change nothing here
        for (auto const& [guid, spell] : fixed)
        {
            Unit* target = ObjectAccessor::GetUnit(*caster, guid);
            if (!target || !target->IsAlive())
                continue;
            AuraApplication* app = target->GetAuraApplication(spell, caster->GetGUID());
            AuraEffect* eff = app ? app->GetBase()->GetEffect(EFFECT_0) : nullptr;
            if (!eff || eff->GetAuraType() != SPELL_AURA_PERIODIC_DAMAGE)
                continue;
            t_flarePct = flare;
            eff->PeriodicTick(app, caster);
            t_flarePct = 0;
        }
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ef_kit_torrent::Started, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(aura_ef_kit_torrent::Flare, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// ---- Crimson Collapse -----------------------------------------------------------------------------------------------

class spell_ef_kit_collapse : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_collapse);

    SpellCastResult CheckCast()
    {
        return CursesOf(GetCaster()).empty() ? SPELL_FAILED_NO_VALID_TARGETS : SPELL_CAST_OK;
    }

    void HandleDummy(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        if (!caster || GetHitUnit() != caster)
            return;
        SpellInfo const* info = GetSpellInfo();
        int32 const pct = info->Effects[EFFECT_0].CalcValue(caster);
        uint32 const hit = info->Effects[EFFECT_0].TriggerSpell;
        uint32 const keepTalent = uint32(std::max(0, info->Effects[EFFECT_1].MiscValue));
        uint32 const kept = keepTalent && caster->HasAura(keepTalent) ? info->Effects[EFFECT_1].TriggerSpell : 0;
        uint32 const waveTalent = uint32(std::max(0, info->Effects[EFFECT_2].MiscValue));
        int32 const waveMs = waveTalent && caster->HasAura(waveTalent)
            ? info->Effects[EFFECT_2].CalcValue(caster) * IN_MILLISECONDS : 0;

        struct Burst
        {
            ObjectGuid target;
            int64 remaining = 0;
            std::vector<uint32> removed;
        };
        std::map<ObjectGuid, Burst> bursts;
        for (Held const& h : CursesOf(caster))
        {
            AuraEffect const* eff = h.aura->GetEffect(EFFECT_0);
            if (!eff)
                continue;
            Burst& b = bursts[h.target->GetGUID()];
            b.target = h.target->GetGUID();
            b.remaining += Seal::Remaining(h.aura->GetDuration(), eff->GetAmplitude(), TickNow(caster, h.target, eff, 0));
            if (h.aura->GetId() != kept)
                b.removed.push_back(h.aura->GetId());
        }
        for (auto& [guid, b] : bursts)
        {
            Unit* target = ObjectAccessor::GetUnit(*caster, guid);
            if (!target)
                continue;
            for (uint32 spell : b.removed)
                target->RemoveAurasDueToSpell(spell, caster->GetGUID());
            int32 const amount = Seal::CollapseHit(b.remaining, pct);
            if (hit && amount > 0)
                caster->CastCustomSpell(target, hit, &amount, nullptr, nullptr, true);
            // Second Wave: the curses without a target limit back with its time
            if (waveMs <= 0 || !target->IsAlive())
                continue;
            for (uint32 spell : b.removed)
            {
                SpellInfo const* curse = sSpellMgr->GetSpellInfo(spell);
                if (!curse || curse->Effects[EFFECT_1].MiscValueB > 0)
                    continue;
                if (Aura* aura = caster->AddAura(spell, target))
                    aura->SetDuration(waveMs);
            }
        }
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_ef_kit_collapse::CheckCast);
        OnEffectHitTarget += SpellEffectFn(spell_ef_kit_collapse::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

// ---- Blood Tide -----------------------------------------------------------------------------------------------------

class spell_ef_kit_spread_dots : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_spread_dots);

    std::vector<uint32> Curses()
    {
        std::vector<uint32> out;
        for (uint8 i : { EFFECT_1, EFFECT_2 })
        {
            SpellEffectInfo const& eff = GetSpellInfo()->Effects[i];
            if (eff.Effect == SPELL_EFFECT_DUMMY && eff.MiscValue > 0)
                out.push_back(uint32(eff.MiscValue));
        }
        return out;
    }

    SpellCastResult CheckCast()
    {
        Unit* caster = GetCaster();
        Unit* target = GetExplTargetUnit();
        if (!caster || !target)
            return SPELL_FAILED_BAD_TARGETS;
        for (uint32 spell : Curses())
            if (target->HasAura(spell, caster->GetGUID()))
                return SPELL_CAST_OK;
        return SPELL_FAILED_BAD_TARGETS;
    }

    void HandleDummy(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || target == caster)
            return;
        SpellEffectInfo const& reach = GetSpellInfo()->Effects[EFFECT_0];
        int32 const count = reach.CalcValue(caster);
        float const radius = reach.CalcRadius(caster);
        // the target's curses and their time left
        std::vector<std::pair<uint32, Aura*>> source;
        for (uint32 spell : Curses())
            if (Aura* aura = target->GetAura(spell, caster->GetGUID()))
                source.push_back({ spell, aura });
        if (source.empty() || count <= 0 || radius <= 0.0f)
            return;
        std::list<Unit*> near;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(target, caster, radius);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(target, near, check);
        Cell::VisitObjects(target, searcher, radius);
        std::vector<Unit*> units;
        std::vector<Seal::Candidate> candidates;
        for (Unit* unit : near)
        {
            if (unit == target || unit == caster || !unit->IsAlive() || !caster->IsValidAttackTarget(unit)
                || !target->IsWithinLOSInMap(unit))
                continue;
            int32 lacking = 0;
            for (auto const& [spell, aura] : source)
                lacking += unit->HasAura(spell, caster->GetGUID()) ? 0 : 1;
            candidates.push_back({ units.size(), lacking, target->GetExactDist2dSq(unit) });
            units.push_back(unit);
        }
        int32 newly = 0;
        for (std::size_t i : Seal::PickSpread(candidates, count))
        {
            Unit* unit = units[i];
            bool gained = false;
            for (auto const& [spell, from] : source)
            {
                int32 const left = from->GetDuration();
                Aura* own = unit->GetAura(spell, caster->GetGUID());
                if (!Seal::CopyLengthens(left, own ? own->GetDuration() : -1))
                    continue;
                Aura* aura = own ? own : caster->AddAura(spell, unit);
                if (!aura)
                    continue;
                gained = gained || !own;
                aura->SetMaxDuration(std::max(aura->GetMaxDuration(), left));
                aura->SetDuration(left);
                // the tick phase of the target's curse
                AuraEffect const* phase = from->GetEffect(EFFECT_0);
                if (AuraEffect* eff = aura->GetEffect(EFFECT_0); eff && phase)
                    eff->SetPeriodicTimer(phase->GetPeriodicTimer());
            }
            newly += gained ? 1 : 0;
        }
        SpellEffectInfo const& pay = GetSpellInfo()->Effects[EFFECT_1];
        int32 const seals = Seal::SpreadSeals(newly, pay.CalcValue(caster), pay.MiscValueB);
        if (seals > 0)
            Emberfall::ClassKit::GainSpecPower(caster, seals, GetSpellInfo()->Id);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_ef_kit_spread_dots::CheckCast);
        OnEffectHitTarget += SpellEffectFn(spell_ef_kit_spread_dots::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

// ---- Marked for the Deep --------------------------------------------------------------------------------------------

class spell_ef_kit_marked_bonus : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_marked_bonus);

    void Hit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        int32 const damage = GetHitDamage();
        if (!caster || !target || target == caster || damage <= 0)
            return;
        uint32 mark = 0;
        for (SpellEffectInfo const& eff : GetSpellInfo()->Effects)
            if (eff.Effect == SPELL_EFFECT_DUMMY && eff.MiscValue > 0 && eff.TargetA.GetTarget() == TARGET_UNIT_TARGET_ENEMY)
                mark = uint32(eff.MiscValue);
        if (!mark || !target->HasAura(mark, caster->GetGUID()))
            return;
        int32 sum = 0;
        for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
            if (uint32(eff->GetMiscValue()) == GetSpellInfo()->Id && !HasTrigger(eff))
                sum += eff->GetAmount();
        if (sum)
            SetHitDamage(int32(std::min<int64>(Seal::WithBonus(damage, sum), 0x7FFFFFFF)));
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_ef_kit_marked_bonus::Hit);
    }
};

void AddEmberfallClassKitSealScripts()
{
    RegisterSpellAndAuraScriptPair(spell_ef_kit_seal_dot, aura_ef_kit_seal_dot);
    RegisterSpellAndAuraScriptPair(spell_ef_kit_torrent, aura_ef_kit_torrent);
    RegisterSpellScript(spell_ef_kit_collapse);
    RegisterSpellScript(spell_ef_kit_spread_dots);
    RegisterSpellScript(spell_ef_kit_marked_bonus);
    new EmberfallSealUnitScript();
}
