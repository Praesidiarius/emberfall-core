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
// Water Ritualist" v3.0 since 2026-10-28): curses read their bonuses at each tick, a ritual that spends the Blood Seals
// to lengthen and flare every curse, the detonation of what the curses still had to deal, curses copied round the
// target. Generic scripts, their numbers in the spells' own effects (Emberfall's classkit/scripts_seal.py checks the
// contracts); the arithmetic is classkit/EfSealLogic.h (tests/classkit_tests.cpp).
//
//   spell_ef_kit_seal_dot      a curse (a spell and aura script under one name). Effect 1 periodic damage at the enemy.
//                              Effect 2 a dummy on the caster: misc = the window aura (Crimson Flood: its effect 1 the
//                              percent more tick damage), misc B = enemies at a time (0: any; Drowning Vow 1: a new target
//                              takes it off the oldest); optionally value = spec power points a generating tick gives and
//                              trigger = a shared cooldown aura (a self helper: while the caster has it no tick of any
//                              curse gives power; the window's effect 2 in ms while it holds); v3.0's curses give none.
//                              Effect 3 a dummy on the caster: value = the cap of a recast (spell_ef_kit_capped_extend),
//                              misc = the caster's partner aura beside which the ticks deal misc B percent more
//                              (Bloodbinding beside Crimson Brine), or a number 1-100: the health percent below which
//                              they do (Drowning Vow below 35 %); misc B may be 0 (only the talents' percent). The
//                              caster's dummy auras naming the curse (misc):
//                                no trigger, misc B 0: their value adds to that bonus (Crimson Bond, Executioner's Vow);
//                                no trigger, misc B 1: their value more enemies at a time (Twin Vows);
//                                trigger, misc B 0: their value in percent more while the target lacks the caster's
//                                  trigger aura;
//                                trigger, misc B 1: the trigger cast on the caster with their value in percent of the tick;
//                                trigger, misc B 2: every value-th tick of the curse casts the trigger on the caster
//                                  (Blood Harvest: a Blood Seal);
//                                trigger, misc B 3: each tick a chance of value percent, value more after each tick that
//                                  missed, kept per caster (through recasts) until it hits: the trigger on the caster
//                                  (Crimson Flow);
//                                trigger, misc B 4: the trigger at the enemy while the curse holds there (Binding Grasp).
//                              The multiplier is applied in the periodic damage hook (before mitigation, after the
//                              snapshot of spell power and spell mods), so Torrent's flares and the window count for
//                              curses cast before it. A Torrent's flare is a tick too (the counters, the chance). Every
//                              curse of a caster on any enemy is kept in a list on the caster (Torrent, Collapse:
//                              "regardless of distance", on the caster's map).
//   spell_ef_kit_torrent       a channel spending the spec power: effect 1 a periodic dummy on the caster (the flares),
//                              effect 2 a dummy at the enemy it starts at, value = ms added to every curse of the caster
//                              per point spent (Eternal Torrent), past every cap, misc B = the least points it needs (5);
//                              effect 3 a dummy on the caster, value = percent more damage of the flares (Violent Torrent),
//                              misc = an aura of the caster's that makes it free (Freeflow Ritual: taken by the cast, no
//                              point taken, no least), misc B = the points it counts as then (10). At the channel's start
//                              the power is taken and the cursed enemies are fixed; each period every curse of the caster
//                              still on them ticks once more (its own tick: the bonuses). The caster's dummy auras naming
//                              it: with a trigger, after each flare the trigger healing the caster for value per mille of
//                              its maximum health for each curse that ticked (Sanguine Leech); without one, only the
//                              enemy it starts at is fixed (Eternal Torrent).
//   spell_ef_kit_collapse      effect 1 a dummy on the caster: value = percent of what the curses still had to deal
//                              (Devastating Collapse), trigger = the hit cast at each cursed enemy with that amount; the
//                              curses go. Remaining = the ticks left (rounded up) x the tick as it would land now. Then the
//                              caster's dummy auras naming it with a trigger cast that on the caster (Ritual Aftershock).
//                              Refused when no curse of the caster is up. Its other effects are free (charges).
//   spell_ef_kit_spread_dots   effect 1 a dummy at the enemy: value = how many more enemies (Rising Tide), its radius
//                              round the target (Expanding Tide), misc = a talent with which a copy carries at most the
//                              curse's base duration (Eternal Torrent); effects 2 and 3 name the curses it copies (misc);
//                              effect 2's value = power per newly affected enemy (v3.0: 0), misc B = at most that many
//                              pay. The enemies lacking the most of the target's curses first, then the closest; each copy
//                              gets the target's time left and tick phase, never shortening a longer one. Refused without
//                              one of them on the target.
//   spell_ef_kit_damage_echo   a damage spell: the caster's dummy auras naming it with a trigger (a periodic damage aura)
//                              cast that at the enemy struck with value percent of the damage dealt, over its ticks
//                              (Lingering Ruin on Crimson Collapse's hit).
//   spell_ef_kit_marked_bonus  a dummy at the enemy names an aura of the caster's: on an enemy carrying it, the caster's
//                              dummy auras naming this spell without a trigger add their value in percent to its damage
//                              (unused since v3.0).

#include "EfClassKit.h"
#include "EfClassKitLogic.h"
#include "EfSealLogic.h"

#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "ObjectAccessor.h"
#include "Random.h"
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
    char const* const kRampKey = "ef_seal_ramps";
    char const* const kTorrentKey = "ef_seal_torrent";

    // Every curse of the caster on an enemy: (enemy, curse spell).
    struct Cursed : public DataMap::Base
    {
        std::set<std::pair<ObjectGuid, uint32>> held;
    };

    struct Limited : public DataMap::Base
    {
        std::map<uint32, std::deque<ObjectGuid>> bySpell;
    };

    // Crimson Flow's chances: per trigger, the ticks that missed since the last hit (kept through recasts).
    struct Ramps : public DataMap::Base
    {
        std::map<uint32, int32> fails;
    };

    // The enemy the caster's last Sanguine Torrent started at (Eternal Torrent fixes only it).
    struct TorrentTarget : public DataMap::Base
    {
        ObjectGuid target;
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

    uint32 TriggerOf(AuraEffect const* eff)
    {
        return eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell;
    }

    bool HasTrigger(AuraEffect const* eff)
    {
        return TriggerOf(eff) != 0;
    }

    // The caster's dummy auras naming `spell` (misc) with a trigger and misc B `kind`: (trigger, value) each.
    std::vector<std::pair<uint32, int32>> Named(Unit* caster, uint32 spell, int32 kind)
    {
        std::vector<std::pair<uint32, int32>> out;
        if (!caster)
            return out;
        for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
            if (uint32(eff->GetMiscValue()) == spell && eff->GetMiscValueB() == kind && HasTrigger(eff))
                out.push_back({ TriggerOf(eff), eff->GetAmount() });
        return out;
    }

    // Whether the caster has a dummy aura naming `spell` without a trigger and with misc B `kind`.
    bool NamedPlain(Unit* caster, uint32 spell, int32 kind)
    {
        if (!caster)
            return false;
        for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
            if (uint32(eff->GetMiscValue()) == spell && eff->GetMiscValueB() == kind && !HasTrigger(eff))
                return true;
        return false;
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
            if (!trigger && eff->GetMiscValueB() == 0)
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
        int32 const base = GetSpellInfo()->Effects[EFFECT_1].MiscValueB;
        if (!caster || !target || target == caster || base <= 0 || !target->HasAura(spell, caster->GetGUID()))
            return;
        int32 limit = base;
        for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
            if (uint32(eff->GetMiscValue()) == spell && eff->GetMiscValueB() == 1 && !HasTrigger(eff))
                limit += eff->GetAmount();     // Twin Vows
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

    uint32 _ticks = 0;    // this curse's ticks so far, a Torrent's flares included (Blood Harvest)

    // the curse lands: on the caster's list; its companions at the enemy (misc B 4: Binding Grasp's slow)
    void Applied(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetTarget();
        Track(caster, target, GetId(), true);
        if (!caster || !target || !target->IsAlive())
            return;
        for (auto const& [trigger, value] : Named(caster, GetId(), 4))
            caster->CastSpell(target, trigger, true);
    }

    // the curse goes: off the list, its companions with it
    void Removed(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetTarget();
        Track(caster, target, GetId(), false);
        if (caster && target)
            for (auto const& [trigger, value] : Named(caster, GetId(), 4))
                target->RemoveAurasDueToSpell(trigger, GetCasterGUID());
    }

    // inside AuraEffect::PeriodicTick, before the damage (a Torrent's flare too): Seals under a shared cooldown (none in
    // v3.0), Blood Harvest, Crimson Flow, Drink Deep
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
        // Blood Harvest (misc B 2): every value-th tick of this curse casts the trigger on the caster
        ++_ticks;
        for (auto const& [trigger, every] : Named(caster, GetId(), 2))
            if (Seal::EveryNth(_ticks, every))
                caster->CastSpell(caster, trigger, true);
        // Crimson Flow (misc B 3): a chance that grows with every tick that missed, per caster, until it hits
        for (auto const& [trigger, step] : Named(caster, GetId(), 3))
        {
            int32& fails = caster->CustomData.GetDefault<Ramps>(kRampKey)->fails[trigger];
            if (roll_chance_i(Seal::RampChance(fails, step)))
            {
                fails = 0;
                caster->CastSpell(caster, trigger, true);
            }
            else
                ++fails;
        }
        // Drink Deep (misc B 1): the trigger with a share of the tick
        std::vector<std::pair<uint32, int32>> const heals = Named(caster, GetId(), 1);
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
        Unit* caster = GetCaster();
        SpellInfo const* info = GetSpellInfo();
        uint32 const freeAura = uint32(std::max(0, info->Effects[EFFECT_2].MiscValue));
        Seal::TorrentSpend const spend = Seal::TorrentPoints(Emberfall::ClassKit::SpecPowerPoints(caster),
            info->Effects[EFFECT_1].MiscValueB, freeAura && caster && caster->HasAura(freeAura), info->Effects[EFFECT_2].MiscValueB);
        return spend.points > 0 ? SPELL_CAST_OK : SPELL_FAILED_NO_POWER;
    }

    // the enemy it starts at (Eternal Torrent fixes only it)
    void Record()
    {
        Unit* caster = GetCaster();
        Unit* target = GetExplTargetUnit();
        if (caster)
            caster->CustomData.GetDefault<TorrentTarget>(kTorrentKey)->target = target ? target->GetGUID() : ObjectGuid::Empty;
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_ef_kit_torrent::CheckCast);
        BeforeCast += SpellCastFn(spell_ef_kit_torrent::Record);
    }
};

class aura_ef_kit_torrent : public AuraScript
{
    PrepareAuraScript(aura_ef_kit_torrent);

    std::vector<std::pair<ObjectGuid, uint32>> _fixed;

    // the channel starts: the points spent (or the free aura's), the cursed enemies fixed (Eternal Torrent: only the one
    // it started at), each curse on them lengthened past its cap
    void Started(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;
        SpellInfo const* info = GetSpellInfo();
        uint32 const freeAura = uint32(std::max(0, info->Effects[EFFECT_2].MiscValue));
        bool const isFree = freeAura && caster->HasAura(freeAura);
        Seal::TorrentSpend const spend = Seal::TorrentPoints(Emberfall::ClassKit::SpecPowerPoints(caster),
            info->Effects[EFFECT_1].MiscValueB, isFree, info->Effects[EFFECT_2].MiscValueB);
        if (isFree)
            caster->RemoveAurasDueToSpell(freeAura);
        else if (spend.takesPower)
            Emberfall::ClassKit::SetSpecPowerPoints(caster, 0);
        int32 const ext = Seal::TorrentExtension(spend.points, info->Effects[EFFECT_1].CalcValue(caster));
        ObjectGuid only;
        if (NamedPlain(caster, GetId(), 0))
            if (TorrentTarget const* t = caster->CustomData.Get<TorrentTarget>(kTorrentKey))
                only = t->target;
        _fixed.clear();
        for (Held const& h : CursesOf(caster))
        {
            if (!only.IsEmpty() && h.target->GetGUID() != only)
                continue;
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
        int32 ticked = 0;
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
            ++ticked;
        }
        // Sanguine Leech: per mille of the caster's maximum health for each extra tick (one heal a flare)
        if (!ticked || !caster->IsAlive())
            return;
        for (auto const& [trigger, perMille] : Named(caster, GetId(), 0))
        {
            int32 const amount = Seal::LeechHeal(caster->GetMaxHealth(), perMille, ticked);
            if (amount > 0)
                caster->CastCustomSpell(caster, trigger, &amount, nullptr, nullptr, true);
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
            b.removed.push_back(h.aura->GetId());
        }
        // every curse goes, then each enemy takes its hit (Lingering Ruin rides on the hit: spell_ef_kit_damage_echo)
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
        }
        // Ritual Aftershock: the caster's dummies naming the Collapse with a trigger, after it consumed a curse
        if (!bursts.empty())
            for (auto const& [trigger, value] : Named(caster, info->Id, 0))
                caster->CastSpell(caster, trigger, true);
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
        // Eternal Torrent (effect 1's misc, a talent): a copy carries at most the curse's base duration
        uint32 const capTalent = uint32(std::max(0, reach.MiscValue));
        bool const capped = capTalent && caster->HasAura(capTalent);
        int32 newly = 0;
        for (std::size_t i : Seal::PickSpread(candidates, count))
        {
            Unit* unit = units[i];
            bool gained = false;
            for (auto const& [spell, from] : source)
            {
                int32 const left = Seal::CopyCapped(from->GetDuration(), capped ? from->GetSpellInfo()->GetMaxDuration() : 0);
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

// ---- Lingering Ruin -----------------------------------------------------------------------------------------------

class spell_ef_kit_damage_echo : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_damage_echo);

    // after the hit: the damage dealt (crit, mitigation and absorbs in), a share of it over the trigger's ticks
    void AfterHitTarget()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        int32 const dealt = GetHitDamage();
        if (!caster || !target || target == caster || dealt <= 0 || !target->IsAlive())
            return;
        for (auto const& [trigger, pct] : Named(caster, GetSpellInfo()->Id, 0))
        {
            SpellInfo const* dot = sSpellMgr->GetSpellInfo(trigger);
            if (!dot)
                continue;
            int32 const period = int32(dot->Effects[EFFECT_0].Amplitude);
            int32 const ticks = period > 0 ? dot->GetMaxDuration() / period : 0;
            int32 const amount = Seal::EchoTick(dealt, pct, ticks);
            if (amount > 0)
                caster->CastCustomSpell(target, trigger, &amount, nullptr, nullptr, true);
        }
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_ef_kit_damage_echo::AfterHitTarget);
    }
};

void AddEmberfallClassKitSealScripts()
{
    RegisterSpellAndAuraScriptPair(spell_ef_kit_seal_dot, aura_ef_kit_seal_dot);
    RegisterSpellAndAuraScriptPair(spell_ef_kit_torrent, aura_ef_kit_torrent);
    RegisterSpellScript(spell_ef_kit_collapse);
    RegisterSpellScript(spell_ef_kit_spread_dots);
    RegisterSpellScript(spell_ef_kit_marked_bonus);
    RegisterSpellScript(spell_ef_kit_damage_echo);
    new EmberfallSealUnitScript();
}
