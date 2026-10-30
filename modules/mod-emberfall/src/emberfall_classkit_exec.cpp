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


// Emberfall class kits: the Warrior's Executioner (Emberfall docs/design/classes/warrior-executioner.md: v2.0, the
// designers' handoff of 2026-10-24, personal Openings, the spec power, max 5; the Talent Tree v1.0 of 2026-10-26, ankarna's
// approved handoff, §15: the baselines revised and their talents). Generic scripts, their numbers in the spells' own
// effects (Emberfall's classkit/model.py and classkit/scripts_exec.py check the contracts); the arithmetic is
// EfClassKitLogic.h (tests/classkit_tests.cpp).
//
//   spell_ef_kit_finisher   a spender of the caster's spec power (its Spell.dbc cost: the core asks for the cost and
//                           takes exactly it, 3 Openings). A dummy effect on the caster with misc = an aura of the caster's
//                           makes the cast free (Perfect Execution: read before the cast; the cost is still asked for, then
//                           given back, and the aura goes). v1.0: no sure crit any more (the normal chance; Decisive Strike
//                           and Deadly Whirl are crit chance spell mods), no refund of its own (Headsman's Due retired;
//                           Exploit the Opening is a rule) (Final Verdict, Whirling Steel)
//   spell_ef_kit_sweep      a dummy on the caster with misc = an aura of the caster's that makes the strike sweep, trigger
//                           = the spell cast at the other enemies, value = how many, misc B = yards round the target: after
//                           the hit on the target, while the caster holds the aura (it is not spent: Sweeping Deathblow's
//                           6 s), the trigger strikes that many other enemies nearest the target, once a cast (Deathblow)
//   spell_ef_kit_sweep_hit  effect 1 deals the damage, its misc B = the percent of it that lands (Sweeping Deathblow's
//                           other targets, Relentless Follow-Up's second strike, Aftershock, Storm of Blades: 50 % of their
//                           ability's own numbers; misc B since 2026-10-30: a weapon strike's misc is its weapon percent,
//                           the core's PATCHES.md #28)
//   spell_ef_kit_rules      the caster's talent rules for this ability (EfClassKit.h Rules; a rule is a talent passive bound
//                           to a spell_ef_kit_rule_* aura script, which does nothing itself, its numbers in its dummy):
//                           on each enemy before the damage   _low_health_damage (+value % at or below misc B % health),
//                                                             _count_damage (+value % for each enemy struck beyond the first,
//                                                             counting at most misc B), _single_damage (+value % with exactly
//                                                             one struck), and a crit there marks _crit_low_health (trigger on
//                                                             the caster: a crit on an enemy at or below misc B %)
//                           on each enemy after the damage    _kill_cut (a killing blow: -value ms off trigger's cooldown)
//                           once the cast is done             _crit_refund (any crit: value points back), _crit_ramp (struck
//                                                             without a crit: a stack of trigger; a crit: none), _crit_extend
//                                                             (any crit while the caster carries trigger: value ms longer, at
//                                                             most misc B s an activation), _count_trigger (at least misc B
//                                                             enemies struck: trigger on the caster after value ms),
//                                                             _hit_heal (struck: trigger heals value % of max health),
//                                                             _cooldown_cut (struck: -value ms off trigger's cooldown),
//                                                             _delayed_area (trigger at the spot misc B yd ahead after value
//                                                             ms; misc B -1: at the cast's destination value ms after the
//                                                             leap to it lands: Eagle's Descent); the Corsair v2.0's
//                                                             (warrior-corsair.md §16):
//                                                             _crit_cooldown_cut (any crit: -value ms off trigger's cooldown:
//                                                             Ruthless Barrage), _lone_echo (exactly one enemy struck: trigger
//                                                             at it with value % of the damage done to it, before armour:
//                                                             Relentless Strike), _count_buff (trigger, a self aura, with
//                                                             value x the enemies struck, at most misc B; the old one goes
//                                                             first, none struck: none: Evasive Sweep), _follow_through (while
//                                                             the caster holds the trigger's CasterAuraSpell: trigger at the
//                                                             primary enemy struck with value % of the damage done to it:
//                                                             Vicious Follow-Through in No Quarter), _rhythm (a cast that
//                                                             struck spends the trigger, a self buff; one that differs from
//                                                             the last that struck gives value points of the spec power and the
//                                                             trigger anew; the rule passive's other effect's trigger is the
//                                                             tracking marker, a fight marker whose amount is that last spell:
//                                                             Perfect Rhythm)
//                           The build rules (_build_low_health, _build_count, _build_floor) are spell_ef_kit_build_power's;
//                           _kill_cut with no ability named (Opportunistic Throw: any killing blow) the kill hook's below.
//   spell_ef_kit_rule_*     the rules' own aura scripts: markers (nothing happens in them)
//   spell_ef_kit_once_a_cast  a proc aura that procs at most once a cast (ankarna's feedback of 2026-10-26: the Deathblow
//                           window; Whirling Steel, Earthbreaker and Reaping Arc critting several enemies open it once)

#include "EfClassKit.h"
#include "EfClassKitLogic.h"
#include "EfFlowLogic.h"

#include "CellImpl.h"
#include "Creature.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerScript.h"
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
#include <cmath>
#include <list>
#include <utility>
#include <vector>

namespace
{
    uint32 TriggerOf(AuraEffect const* eff)
    {
        return eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell;
    }

    // A killing blow of `caster`'s: every kill-cut rule that names `spellId` (0: the rules that name none, any killing
    // blow) takes its value off its trigger's running cooldown (a value beyond the cooldown resets it).
    void KillCuts(Unit* caster, uint32 spellId)
    {
        for (AuraEffect const* eff : Emberfall::ClassKit::Rules(caster, "spell_ef_kit_rule_kill_cut", spellId))
            if (uint32 const trigger = TriggerOf(eff); trigger && eff->GetAmount() < 0)
                Emberfall::ClassKit::ShortenCooldown(caster, trigger, -eff->GetAmount());
    }
}

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

    void Finish()
    {
        Unit* caster = GetCaster();
        if (!caster || !_free)
            return;
        // Perfect Execution: the 3 Openings the core took come back, and the buff is spent by this finisher (Perfect
        // Timing's damage is the buff's own spell mod, read by the hits before)
        Powers const slot = Powers(GetSpellInfo()->PowerType);
        if (int32 const cost = GetSpell()->GetPowerCost(); cost > 0)
            caster->ModifyPower(slot, cost);
        caster->RemoveAurasDueToSpell(_free);
    }

    void Register() override
    {
        BeforeCast += SpellCastFn(spell_ef_kit_finisher::Before);
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
        SetHitDamage(Emberfall::ClassKit::Logic::SweepDamage(GetHitDamage(), GetSpellInfo()->Effects[EFFECT_0].MiscValueB));
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_ef_kit_sweep_hit::HandleHit);
    }
};

class spell_ef_kit_rules : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_rules);

    uint32 _hits = 0;                  // enemies struck (AfterHit runs only for a hit)
    bool _crit = false;                // an enemy struck critically
    std::vector<uint32> _lowCrits;     // the triggers of _crit_low_health rules a crit earned (cast once, after the cast)
    std::vector<std::pair<ObjectGuid, int32>> _struck;  // each enemy struck and the damage done to it (before armour)

    // the enemies this cast strikes (no miss, dodge or parry), known before the first hit lands
    uint32 EnemiesStruck()
    {
        Unit* caster = GetCaster();
        uint32 n = 0;
        for (TargetInfo const& info : *GetSpell()->GetUniqueTargetInfo())
            if ((!caster || info.targetGUID != caster->GetGUID()) && info.missCondition == SPELL_MISS_NONE)
                ++n;
        return n;
    }

    bool CritOn(Unit const* target)
    {
        for (TargetInfo const& info : *GetSpell()->GetUniqueTargetInfo())
            if (info.targetGUID == target->GetGUID())
                return info.crit;
        return false;
    }

    // before the damage is dealt: the damage rules, and whether a crit on a low-health enemy earned a trigger
    void HandleHit()
    {
        using namespace Emberfall::ClassKit;
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || target == caster)
            return;
        uint32 const id = GetSpellInfo()->Id;
        float const health = target->GetHealthPct();
        uint32 const struck = EnemiesStruck();
        int32 pct = 0;
        for (AuraEffect const* eff : Rules(caster, "spell_ef_kit_rule_low_health_damage", id))
            if (Logic::AtOrBelowHealth(health, eff->GetMiscValueB()))
                pct += eff->GetAmount();
        for (AuraEffect const* eff : Rules(caster, "spell_ef_kit_rule_count_damage", id))
            pct += Logic::CountBonusPct(struck, eff->GetAmount(), eff->GetMiscValueB());
        for (AuraEffect const* eff : Rules(caster, "spell_ef_kit_rule_single_damage", id))
            pct += Logic::SingleTargetPct(struck, eff->GetAmount());
        if (pct > 0 && GetHitDamage() > 0)
            SetHitDamage(Logic::DamageWithPct(GetHitDamage(), pct));
        _struck.emplace_back(target->GetGUID(), GetHitDamage());
        if (!CritOn(target))
            return;
        _crit = true;
        for (AuraEffect const* eff : Rules(caster, "spell_ef_kit_rule_crit_low_health", id))
            if (uint32 const trigger = TriggerOf(eff); trigger && Logic::AtOrBelowHealth(health, eff->GetMiscValueB())
                && std::find(_lowCrits.begin(), _lowCrits.end(), trigger) == _lowCrits.end())
                _lowCrits.push_back(trigger);
    }

    // after the damage: a killing blow
    void AfterHitTarget()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || target == caster)
            return;
        ++_hits;
        if (!target->IsAlive())
            KillCuts(caster, GetSpellInfo()->Id);
    }

    void Finish()
    {
        using namespace Emberfall::ClassKit;
        Unit* caster = GetCaster();
        if (!caster)
            return;
        uint32 const id = GetSpellInfo()->Id;
        // Exploit the Opening: points back once the cast struck critically (after its cost; the core's maximum holds)
        if (_crit)
            for (AuraEffect const* eff : Rules(caster, "spell_ef_kit_rule_crit_refund", id))
                GainSpecPower(caster, eff->GetAmount(), id);
        // Growing Precision: a cast that struck without a crit adds a stack, a crit takes them all
        if (_hits)
            for (AuraEffect const* eff : Rules(caster, "spell_ef_kit_rule_crit_ramp", id))
            {
                uint32 const stackSpell = TriggerOf(eff);
                SpellInfo const* info = sSpellMgr->GetSpellInfo(stackSpell);
                if (!info)
                    continue;
                Aura* aura = caster->GetAura(stackSpell, caster->GetGUID());
                int32 const have = aura ? aura->GetStackAmount() : 0;
                int32 const after = Logic::CritRampStacks(have, _crit, int32(info->StackAmount));
                if (after <= 0)
                    caster->RemoveAurasDueToSpell(stackSpell);
                else if (!aura)
                    caster->AddAura(stackSpell, caster);
                else if (after > have)
                    aura->ModStackAmount(after - have);
            }
        // Unending Slaughter: a critical cast while the aura holds lengthens it, at most misc B s an activation
        if (_crit)
            for (AuraEffect const* eff : Rules(caster, "spell_ef_kit_rule_crit_extend", id))
                if (uint32 const aura = TriggerOf(eff); aura && caster->HasAura(aura))
                    Lengthen(caster, aura, eff->GetAmount(), eff->GetMiscValueB());
        // Death's Momentum: renewed, not stacked (the old buff goes first: a charge this cast spent must not take the new one)
        for (uint32 trigger : _lowCrits)
        {
            caster->RemoveAurasDueToSpell(trigger);
            caster->CastSpell(caster, trigger, true);
        }
        // Focused Carnage, Storm of Blades: at least misc B enemies struck
        for (AuraEffect const* eff : Rules(caster, "spell_ef_kit_rule_count_trigger", id))
        {
            uint32 const trigger = TriggerOf(eff);
            if (!trigger || !Logic::CountReached(_hits, eff->GetMiscValueB()))
                continue;
            if (eff->GetAmount() <= 0)
            {
                caster->CastSpell(caster, trigger, true);
                continue;
            }
            // the event lives in the caster's own queue: it goes with the caster
            caster->m_Events.AddEventAtOffset([caster, trigger]()
            {
                if (caster->IsInWorld() && caster->IsAlive())
                    caster->CastSpell(caster, trigger, true);
            }, Milliseconds(eff->GetAmount()));
        }
        if (_hits)
        {
            // Reaper's Vitality: value % of max health
            for (AuraEffect const* eff : Rules(caster, "spell_ef_kit_rule_hit_heal", id))
                if (uint32 const trigger = TriggerOf(eff))
                    if (int32 const heal = Logic::PctOfMaxHealth(caster->GetMaxHealth(), eff->GetAmount()); heal > 0)
                        caster->CastCustomSpell(caster, trigger, &heal, nullptr, nullptr, true);
            // Blood Frenzy: once a cast that struck
            for (AuraEffect const* eff : Rules(caster, "spell_ef_kit_rule_cooldown_cut", id))
                if (uint32 const trigger = TriggerOf(eff); trigger && eff->GetAmount() < 0)
                    ShortenCooldown(caster, trigger, -eff->GetAmount());
        }
        CorsairRules(caster, id);
        // Aftershock: at the spot misc B yards ahead of where the caster stood, value ms later, whatever was struck.
        // Eagle's Descent (misc B -1): at the cast's destination (a leap to a spot: charge_dest), value ms after the leap
        // lands (its distance at the core's charge speed, Logic::LandingDelayMs); a cast without a destination: nothing
        for (AuraEffect const* eff : Rules(caster, "spell_ef_kit_rule_delayed_area", id))
        {
            uint32 const trigger = TriggerOf(eff);
            if (!trigger)
                continue;
            float x, y, z;
            int32 delay = std::max(0, eff->GetAmount());
            if (eff->GetMiscValueB() < 0)
            {
                WorldLocation const* dest = GetExplTargetDest();
                if (!dest)
                    continue;
                x = dest->GetPositionX();
                y = dest->GetPositionY();
                z = dest->GetPositionZ();
                delay = Logic::LandingDelayMs(caster->GetExactDist(x, y, z), SPEED_CHARGE, eff->GetAmount());
            }
            else
            {
                float const ahead = float(eff->GetMiscValueB());
                float const o = caster->GetOrientation();
                x = caster->GetPositionX() + ahead * std::cos(o);
                y = caster->GetPositionY() + ahead * std::sin(o);
                z = caster->GetPositionZ();
            }
            caster->m_Events.AddEventAtOffset([caster, trigger, x, y, z]()
            {
                if (caster->IsInWorld())
                    caster->CastSpell(x, y, z, trigger, true);
            }, Milliseconds(delay));
        }
    }

    // The primary enemy struck: the cast's own target, else (a spin round the caster: Stormwake) the caster's selected enemy
    // or victim when it was struck, else the first struck; the damage done to it.
    std::pair<Unit*, int32> Primary(Unit* caster)
    {
        if (_struck.empty())
            return { nullptr, 0 };
        auto find = [this](Unit const* unit) -> std::pair<ObjectGuid, int32> const*
        {
            for (auto const& s : _struck)
                if (unit && s.first == unit->GetGUID())
                    return &s;
            return nullptr;
        };
        Unit* choice = GetExplTargetUnit();
        if (!choice || choice == caster || !find(choice))
        {
            Player* player = caster->ToPlayer();
            choice = player ? player->GetSelectedUnit() : nullptr;
            if (!find(choice))
                choice = caster->GetVictim();
        }
        std::pair<ObjectGuid, int32> const* hit = find(choice);
        if (!hit)
            hit = &_struck.front();
        return { ObjectAccessor::GetUnit(*caster, hit->first), hit->second };
    }

    // the Corsair v2.0's rules (the module doc above), once the cast is done
    void CorsairRules(Unit* caster, uint32 id)
    {
        using namespace Emberfall::ClassKit;
        // Ruthless Barrage: a crit takes value ms off the trigger's cooldown
        if (_crit)
            for (AuraEffect const* eff : Rules(caster, "spell_ef_kit_rule_crit_cooldown_cut", id))
                if (uint32 const trigger = TriggerOf(eff); trigger && eff->GetAmount() < 0)
                    ShortenCooldown(caster, trigger, -eff->GetAmount());
        // Evasive Sweep: the old buff goes, the new one counts the enemies struck
        for (AuraEffect const* eff : Rules(caster, "spell_ef_kit_rule_count_buff", id))
        {
            uint32 const trigger = TriggerOf(eff);
            if (!trigger)
                continue;
            caster->RemoveAurasDueToSpell(trigger);
            if (int32 const amount = Flow::CountAmount(_hits, eff->GetAmount(), eff->GetMiscValueB()); amount > 0)
                caster->CastCustomSpell(caster, trigger, &amount, nullptr, nullptr, true);
        }
        Rhythm(caster, id);
        if (_struck.empty())
            return;
        auto [primary, damage] = Primary(caster);
        // Relentless Strike: exactly one enemy struck, it is struck again
        if (_struck.size() == 1 && primary && primary->IsAlive())
            for (AuraEffect const* eff : Rules(caster, "spell_ef_kit_rule_lone_echo", id))
                if (uint32 const trigger = TriggerOf(eff))
                    if (int32 const echo = Flow::ShareOf(damage, eff->GetAmount()); echo > 0)
                        caster->CastCustomSpell(primary, trigger, &echo, nullptr, nullptr, true);
        // Vicious Follow-Through: while the trigger's caster aura holds (No Quarter)
        if (primary && primary->IsAlive())
            for (AuraEffect const* eff : Rules(caster, "spell_ef_kit_rule_follow_through", id))
            {
                uint32 const trigger = TriggerOf(eff);
                SpellInfo const* info = trigger ? sSpellMgr->GetSpellInfo(trigger) : nullptr;
                if (!info || (info->CasterAuraSpell && !caster->HasAura(info->CasterAuraSpell)))
                    continue;
                if (int32 const extra = Flow::ShareOf(damage, eff->GetAmount()); extra > 0)
                    caster->CastCustomSpell(primary, trigger, &extra, nullptr, nullptr, true);
            }
    }

    // Perfect Rhythm: every finisher spends the buff, landed or not (the miss rules, #143); a landed one that differs from
    // the last landed gives it anew (and its Energy)
    void Rhythm(Unit* caster, uint32 id)
    {
        using namespace Emberfall::ClassKit;
        for (AuraEffect const* eff : Rules(caster, "spell_ef_kit_rule_rhythm", id))
        {
            uint32 const buff = TriggerOf(eff);
            uint32 track = 0;
            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                if (i != eff->GetEffIndex() && eff->GetSpellInfo()->Effects[i].TriggerSpell)
                    track = eff->GetSpellInfo()->Effects[i].TriggerSpell;
            if (!buff || !track)
                continue;
            AuraEffect const* last = caster->GetAuraEffect(track, EFFECT_0, caster->GetGUID());
            uint32 const previous = last ? uint32(last->GetAmount()) : 0;
            caster->RemoveAurasDueToSpell(buff);
            if (_struck.empty())
                continue;
            if (Flow::RhythmChanged(previous, id))
            {
                GainSpecPower(caster, eff->GetAmount(), id);
                caster->CastSpell(caster, buff, true);
            }
            if (previous != id)
            {
                int32 const mark = int32(id);
                caster->RemoveAurasDueToSpell(track);
                caster->CastCustomSpell(caster, track, &mark, nullptr, nullptr, true);
            }
        }
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_ef_kit_rules::HandleHit);
        AfterHit += SpellHitFn(spell_ef_kit_rules::AfterHitTarget);
        AfterCast += SpellCastFn(spell_ef_kit_rules::Finish);
    }
};

// A rule passive's own script: a marker the rules are found by (EfClassKit.h Rules); nothing happens in it.
class spell_ef_kit_rule : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_rule);

    void Register() override { }
};

// A proc aura that procs at most once a cast (the Deathblow window, ankarna 2026-10-26): an area cast's hits each raise a
// proc event; the first that procs marks the cast, the others of that cast are refused (EfClassKitLogic.h OncePerCast).
class spell_ef_kit_once_a_cast : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_once_a_cast);

    Emberfall::ClassKit::Logic::OncePerCast _once;

    static uint64 Now() { return uint64(GameTime::GetGameTimeMS().count()); }

    static uint32 SpellIdOf(Spell const* spell) { return spell ? spell->GetSpellInfo()->Id : 0; }

    bool CheckProc(ProcEventInfo& info)
    {
        Spell const* spell = info.GetProcSpell();
        return _once.Allows(spell, SpellIdOf(spell), Now());
    }

    void Took(ProcEventInfo& info)
    {
        if (Spell const* spell = info.GetProcSpell())
            _once.Took(spell, SpellIdOf(spell), Now());
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_ef_kit_once_a_cast::CheckProc);
        AfterProc += AuraProcFn(spell_ef_kit_once_a_cast::Took);
    }
};

// Opportunistic Throw: any killing blow of the player's (its damage over time too) runs the kill-cut rules that name no
// ability.
class emberfall_classkit_exec_kills : public PlayerScript
{
public:
    emberfall_classkit_exec_kills() : PlayerScript("emberfall_classkit_exec_kills",
        { PLAYERHOOK_ON_CREATURE_KILL, PLAYERHOOK_ON_PVP_KILL }) { }

    void OnPlayerCreatureKill(Player* killer, Creature* /*killed*/) override { KillCuts(killer, 0); }

    void OnPlayerPVPKill(Player* killer, Player* /*killed*/) override { KillCuts(killer, 0); }
};

void AddEmberfallClassKitExecScripts()
{
    RegisterSpellScript(spell_ef_kit_finisher);
    RegisterSpellScript(spell_ef_kit_sweep);
    RegisterSpellScript(spell_ef_kit_sweep_hit);
    RegisterSpellScript(spell_ef_kit_rules);
    RegisterSpellScript(spell_ef_kit_once_a_cast);
    for (char const* name : { "spell_ef_kit_rule_build_low_health", "spell_ef_kit_rule_build_count",
                              "spell_ef_kit_rule_build_floor", "spell_ef_kit_rule_crit_refund", "spell_ef_kit_rule_crit_ramp",
                              "spell_ef_kit_rule_crit_extend", "spell_ef_kit_rule_crit_low_health", "spell_ef_kit_rule_kill_cut",
                              "spell_ef_kit_rule_cooldown_cut", "spell_ef_kit_rule_low_health_damage",
                              "spell_ef_kit_rule_count_damage", "spell_ef_kit_rule_single_damage",
                              "spell_ef_kit_rule_count_trigger", "spell_ef_kit_rule_hit_heal", "spell_ef_kit_rule_delayed_area",
                              // the Corsair v2.0's (here and in src/emberfall_classkit_flow.cpp)
                              "spell_ef_kit_rule_crit_cooldown_cut", "spell_ef_kit_rule_lone_echo",
                              "spell_ef_kit_rule_count_buff", "spell_ef_kit_rule_follow_through", "spell_ef_kit_rule_rhythm",
                              "spell_ef_kit_rule_stack_bonus", "spell_ef_kit_rule_hook_arrival", "spell_ef_kit_rule_hook_rush" })
        RegisterSpellScriptWithArgs(spell_ef_kit_rule, name);
    new emberfall_classkit_exec_kills();
}
