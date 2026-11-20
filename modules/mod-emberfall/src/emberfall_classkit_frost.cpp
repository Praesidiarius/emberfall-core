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

// The Mage Frostguard's rework (ankarna's handoff of 2026-11-03, Emberfall docs/design/classes/mage-frostguard.md §12):
// the two mechanics no shared script had. Contracts (Emberfall's classkit/model.py SCRIPTS checks the effects):
//
//   spell_ef_kit_marked_absorb        a passive: effect 1 a school absorb (misc: the schools) whose value is the percent
//                                     of a blow it takes, only from an attacker that carries effect 2's trigger aura of
//                                     the owner's (Frozen Weakness: Glacial Spear's mark, 10 % less physical damage from
//                                     the marked enemy to the mage alone); effect 2's dummy is also the hit bonus that lays
//                                     the mark (misc: the spell)
//   spell_ef_kit_proc_attacker_stacks a proc_trigger_spell aura on the blows taken: while the owner holds the aura of its
//                                     misc (the shell), the attacker gains value stacks of its trigger (EfClassKit.h
//                                     AddStacks); the chance and the internal cooldown are the spell_proc row's (Living
//                                     Glacier: 20 %, once a second for every attacker together)
//
// The rework of 2026-11-15 (ankarna's implementation notes, mage-frostguard.md §13; the arithmetic: EfFrostLogic.h):
//
//   spell_ef_kit_capped_stacks        a marker on a stacking aura (effect 1 a dummy whose value is the cap): EfClassKit.h
//                                     AddStacks adds at most that many, the caster's spell mods in (Frost Stacks 6, Deep
//                                     Winter +2), never above the aura's own maximum (StackAmount)
//   spell_ef_kit_area_share           an area damage spell: effect 2's value enemies take its damage whole, with more
//                                     each takes damage x that / enemies struck: the total soft-capped (Shattering Overflow, 5)
//   spell_ef_kit_orb_seek             a self button: effect 1 (dummy): trigger = the orb (a spell at a destination with a
//                                     missile speed whose persistent area aura stands where it lands), value = the line's
//                                     length, misc B = its half width in yards: the orb flies to the first enemy on the line
//                                     ahead (in sight; none: the line's end, short of a wall) and stays there; effect 2
//                                     (dummy): with the talent of its misc known, value % for a second orb, its trigger (a
//                                     twin of the orb under its own id, so the two strike an enemy both stand on; only the
//                                     pressed cast rolls; no cost, no cooldown, no global cooldown of its own) on a line
//                                     misc B degrees to one side, then the other, at another enemy (none there: straight
//                                     ahead); for both orbs the caster's spell_ef_kit_rule_orb_collapse rules naming the
//                                     first orb cast their trigger where it stands as its time runs out (Frostbound Orb,
//                                     Twin Orbs, Shattering Collapse)
//   spell_ef_kit_frost_rules          the caster's Frostguard rules for this ability: spell_ef_kit_rule_first_contact (value %
//                                     more on an enemy the mage had not hurt before the cast: no threat from the mage on a
//                                     creature, not in combat with the mage otherwise; First Contact on Glacial Spear);
//                                     spell_ef_kit_rule_echo_pulse (ankarna's hand-off of 2026-11-19, mage-frostguard.md §14:
//                                     ONE roll a cast, value % for the rule's trigger, cast at the caster once the cast is
//                                     done: Echoing Frost's second pulse on Frigid Pulse, a stack-only spell of its own)
//   spell_ef_kit_rule_shell_overflow  the rule the shared shell gains read (src/emberfall_classkit.cpp, EfFrostRules.h
//                                     OverflowPulse): since 2026-11-19 a gain of one ability activation that passes the cap
//                                     casts the trigger ONCE at the caster with its own numbers (Shattering Overflow's
//                                     pulse); misc = the ability, or none: any gain (the shell over the cap only decides
//                                     whether there is a pulse, never its size)
//   spell_ef_kit_rule_*               the rules' own aura scripts: markers (nothing happens in them)

#include "EfClassKit.h"
#include "EfFrostLogic.h"
#include "EfFrostRules.h"

#include "CellImpl.h"
#include "Creature.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "Player.h"
#include "ScriptMgr.h"
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
#include <map>
#include <set>
#include <vector>

// Takes value % of a blow of its schools, only from an attacker the owner has marked.
class spell_ef_kit_marked_absorb : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_marked_absorb);

    void CalcAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        amount = -1; // unlimited: Absorb decides
    }

    void Absorb(AuraEffect* /*aurEff*/, DamageInfo& dmgInfo, uint32& absorbAmount)
    {
        absorbAmount = 0;
        Unit* owner = GetUnitOwner();
        Unit* attacker = dmgInfo.GetAttacker();
        uint32 const mark = GetSpellInfo()->Effects[EFFECT_1].TriggerSpell;
        if (!owner || !attacker || attacker == owner || !mark || !attacker->HasAura(mark, owner->GetGUID()))
            return;
        int32 const pct = std::clamp(GetSpellInfo()->Effects[EFFECT_0].CalcValue(owner), 0, 100);
        absorbAmount = uint32(uint64(dmgInfo.GetDamage()) * uint64(pct) / 100);
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_ef_kit_marked_absorb::CalcAmount, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
        OnEffectAbsorb += AuraEffectAbsorbFn(spell_ef_kit_marked_absorb::Absorb, EFFECT_0);
    }
};

// A blow taken while the owner holds the misc aura: the attacker gains stacks.
class spell_ef_kit_proc_attacker_stacks : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_proc_attacker_stacks);

    bool CheckProc(ProcEventInfo& info)
    {
        Unit* owner = GetUnitOwner();
        Unit* attacker = info.GetActor();
        int32 const need = GetSpellInfo()->Effects[EFFECT_0].MiscValue;
        if (!owner || !attacker || attacker == owner || !attacker->IsAlive() || !owner->IsValidAttackTarget(attacker))
            return false;
        return !need || owner->HasAura(uint32(need), owner->GetGUID());
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& info)
    {
        PreventDefaultAction();
        Unit* owner = GetUnitOwner();
        Unit* attacker = info.GetActor();
        uint32 const trigger = GetSpellInfo()->Effects[EFFECT_0].TriggerSpell;
        if (!owner || !attacker || !trigger)
            return;
        Emberfall::ClassKit::AddStacks(owner, attacker, trigger, std::max(1, aurEff->GetAmount()));
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_ef_kit_proc_attacker_stacks::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_ef_kit_proc_attacker_stacks::HandleProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// A stacking aura capped by its effect 1 value (EfClassKit.h AddStacks reads it): a marker, nothing happens in it.
class spell_ef_kit_capped_stacks : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_capped_stacks);

    void Register() override { }
};

// An area burst whose damage each enemy takes in full while few are struck, a share of it when many are.
class spell_ef_kit_area_share : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_area_share);

    uint32 _struck = 0;

    void Count(std::list<WorldObject*>& targets)
    {
        _struck = uint32(targets.size());
    }

    void Share()
    {
        uint32 const full = uint32(std::max(1, GetSpellInfo()->Effects[EFFECT_1].CalcValue(GetCaster())));
        if (GetHitDamage() > 0)
            SetHitDamage(Emberfall::ClassKit::FrostLogic::AreaShare(GetHitDamage(), _struck, full));
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_ef_kit_area_share::Count, EFFECT_0, TARGET_UNIT_SRC_AREA_ENEMY);
        OnHit += SpellHitFn(spell_ef_kit_area_share::Share);
    }
};

// Frostbound Orb (2026-11-15): the orb flies ahead to the first enemy on its line and stands there; Twin Orbs, Shattering
// Collapse.
class spell_ef_kit_orb_seek : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_orb_seek);

    // Where an orb sent `turn` radians off the caster's facing stops: the first enemy on the line (in sight, not `avoid`),
    // else the line's end short of a wall. False when no enemy is found and `needEnemy` (a twin's side line).
    bool Seek(Unit* caster, float turn, float length, float half, bool needEnemy, ObjectGuid avoid, Position& dest,
              ObjectGuid& found)
    {
        std::list<Unit*> units;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(caster, caster, length + 5.0f);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(caster, units, check);
        Cell::VisitObjects(caster, searcher, length + 5.0f);
        float const o = caster->GetOrientation() + turn;
        float const c = std::cos(o), s = std::sin(o);
        std::vector<Emberfall::ClassKit::FrostLogic::LineCandidate> line;
        std::vector<Unit*> who;
        for (Unit* u : units)
        {
            if (!u->IsAlive() || u->GetGUID() == avoid || !caster->IsValidAttackTarget(u) || !caster->IsWithinLOSInMap(u))
                continue;
            float const dx = u->GetPositionX() - caster->GetPositionX();
            float const dy = u->GetPositionY() - caster->GetPositionY();
            line.push_back({ dx * c + dy * s, -dx * s + dy * c, u->GetCombatReach() });
            who.push_back(u);
        }
        int const at = Emberfall::ClassKit::FrostLogic::FirstOnLine(line, half, length);
        if (at >= 0)
        {
            dest.Relocate(who[at]->GetPositionX(), who[at]->GetPositionY(), who[at]->GetPositionZ());
            found = who[at]->GetGUID();
            return true;
        }
        if (needEnemy)
            return false;
        dest = caster->GetFirstCollisionPosition(length, turn);
        return true;
    }

    // One orb to `dest`: the orb spell flies there at its own missile speed and stands; the collapse where it stands as its
    // time runs out (the rules name the first orb, `ruleOrb`, and follow the second as well).
    void Send(Unit* caster, uint32 orb, uint32 ruleOrb, Position const& dest, bool twin, ObjectGuid at)
    {
        SpellCastResult const result = caster->CastSpell(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), orb, true);
        LOG_DEBUG("module.classkit.frost", "Frostbound Orb: {} {} of {} to ({:.1f}, {:.1f}, {:.1f}) {:.1f} yd, at {}: cast result {}",
                  twin ? "second orb" : "orb", orb, caster->GetName(), dest.GetPositionX(), dest.GetPositionY(),
                  dest.GetPositionZ(), caster->GetExactDist(&dest), at.IsEmpty() ? "no enemy" : at.ToString(), uint32(result));
        SpellInfo const* info = sSpellMgr->GetSpellInfo(orb);
        if (!info)
            return;
        auto const rules = Emberfall::ClassKit::Rules(caster, "spell_ef_kit_rule_orb_collapse", ruleOrb);
        if (rules.empty())
            return;
        int32 duration = info->GetDuration();
        if (Player* player = caster->ToPlayer())
            player->ApplySpellMod(orb, SPELLMOD_DURATION, duration);
        int32 const delay = Emberfall::ClassKit::FrostLogic::FlightMs(caster->GetExactDist(&dest), info->Speed)
                            + std::max(0, duration);
        float const x = dest.GetPositionX(), y = dest.GetPositionY(), z = dest.GetPositionZ();
        for (AuraEffect const* eff : rules)
            if (uint32 const trigger = eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell)
                caster->m_Events.AddEventAtOffset([caster, trigger, x, y, z]()
                {
                    if (caster->IsInWorld() && caster->IsAlive())
                        caster->CastSpell(x, y, z, trigger, true);
                }, Milliseconds(delay));
    }

    void Launch()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;
        SpellEffectInfo const& orbEff = GetSpellInfo()->Effects[EFFECT_0];
        SpellEffectInfo const& twinEff = GetSpellInfo()->Effects[EFFECT_1];
        uint32 const orb = orbEff.TriggerSpell;
        if (!orb)
            return;
        float const length = float(std::max(1, orbEff.CalcValue(caster)));
        float const half = float(std::max(1, orbEff.MiscValueB));
        Position dest;
        ObjectGuid first;
        Seek(caster, 0.0f, length, half, false, ObjectGuid::Empty, dest, first);
        Send(caster, orb, orb, dest, false, first);
        // Twin Orbs: only this pressed cast rolls; the second orb is effect 2's trigger, cast by the script (no cost, no
        // cooldown, no global cooldown), and never rolls again. It is a spell of its own (the first orb's twin): one
        // caster's periodic ground auras of one spell only refresh each other on an enemy (Aura::CanStackWith), so the
        // first orb's spell a second time added nothing where the two overlap. A test realm's GM may force the chance
        // (`.eftest chance twin_orbs N`).
        bool const talent = twinEff.MiscValue && caster->HasAura(uint32(twinEff.MiscValue));
        int32 const chance = Emberfall::ClassKit::TestChance("twin_orbs", twinEff.CalcValue(caster));
        int32 const roll = int32(urand(0, 99));
        bool const second = Emberfall::ClassKit::FrostLogic::TwinOrb(false, talent, chance, roll);
        LOG_DEBUG("module.classkit.frost", "Twin Orbs roll of {} at ({:.1f}, {:.1f}) facing {:.2f}: talent {}, chance {}, roll {}: {}",
                  caster->GetName(), caster->GetPositionX(), caster->GetPositionY(), caster->GetOrientation(), talent ? 1 : 0,
                  chance, roll, second ? "a second orb" : "none");
        uint32 const twinSpell = Emberfall::ClassKit::FrostLogic::TwinOrbSpell(orb, twinEff.TriggerSpell);
        if (!second || !twinSpell)
            return;
        float const side = float(std::max(1, twinEff.MiscValueB)) * float(M_PI) / 180.0f;
        Position twin;
        ObjectGuid at;
        if (!Seek(caster, side, length, half, true, first, twin, at)
            && !Seek(caster, -side, length, half, true, first, twin, at))
            Seek(caster, 0.0f, length, half, false, ObjectGuid::Empty, twin, at);
        Send(caster, twinSpell, orb, twin, true, at);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_ef_kit_orb_seek::Launch);
    }
};

// The Frostguard's rules on an ability (First Contact on Glacial Spear).
class spell_ef_kit_frost_rules : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_frost_rules);

    std::map<ObjectGuid, bool> _fresh;   // the cast's target: not hurt by the mage before this cast

    static bool Fresh(Unit* caster, Unit* target)
    {
        if (target->CanHaveThreatList())
            return target->GetThreatMgr().GetThreat(caster) <= 0.0f;
        return !target->IsInCombatWith(caster);
    }

    // before the cast goes (the combat it starts and the threat of its hit come after)
    void Before()
    {
        Unit* caster = GetCaster();
        if (Unit* target = GetExplTargetUnit(); caster && target && target != caster)
            _fresh[target->GetGUID()] = Fresh(caster, target);
    }

    void HandleHit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || target == caster || GetHitDamage() <= 0)
            return;
        auto const it = _fresh.find(target->GetGUID());
        if (it == _fresh.end() || !it->second)
            return;
        int32 pct = 0;
        for (AuraEffect const* eff : Emberfall::ClassKit::Rules(caster, "spell_ef_kit_rule_first_contact", GetSpellInfo()->Id))
            pct += eff->GetAmount();
        SetHitDamage(Emberfall::ClassKit::FrostLogic::FirstContactDamage(GetHitDamage(), true, pct));
    }

    // Echoing Frost (2026-11-19): one roll for the whole cast once it is done, however many enemies it struck; the second
    // pulse is the rule's own spell (stack-only: no damage, no threat), cast by the script, triggered (no cost, no global
    // cooldown). A test realm's GM may force the chance (`.eftest chance echoing_frost N`).
    void Echo()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;
        for (AuraEffect const* eff : Emberfall::ClassKit::Rules(caster, "spell_ef_kit_rule_echo_pulse", GetSpellInfo()->Id))
        {
            uint32 const trigger = eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell;
            int32 const chance = Emberfall::ClassKit::TestChance("echoing_frost", eff->GetAmount());
            int32 const roll = int32(urand(0, 99));
            bool const echo = Emberfall::ClassKit::FrostLogic::EchoPulse(trigger != 0, chance, roll);
            LOG_DEBUG("module.classkit.frost", "Echoing Frost roll of {} on {}: chance {}, roll {}: {}", caster->GetName(),
                      GetSpellInfo()->Id, chance, roll, echo ? "a second pulse" : "none");
            if (echo)
                caster->CastSpell(caster, trigger, true);
        }
    }

    void Register() override
    {
        BeforeCast += SpellCastFn(spell_ef_kit_frost_rules::Before);
        OnHit += SpellHitFn(spell_ef_kit_frost_rules::HandleHit);
        AfterCast += SpellCastFn(spell_ef_kit_frost_rules::Echo);
    }
};

// Shattering Overflow since ankarna's hand-off of 2026-11-19 (EfFrostRules.h): one pulse a whole activation, with the
// burst's own numbers (its level and spell power values; crits as any spell; the pack share of spell_ef_kit_area_share).
void Emberfall::ClassKit::FrostRules::OverflowPulse(Unit* caster, uint32 spellId, int64 overflow)
{
    if (!caster || !Emberfall::ClassKit::FrostLogic::OverflowPulses(overflow))
        return;
    // the rules naming this ability and those naming none (any shell gain); one burst each, a burst never twice
    std::set<uint32> bursts;
    for (uint32 named : { spellId, 0u })
        for (AuraEffect const* eff : Emberfall::ClassKit::Rules(caster, "spell_ef_kit_rule_shell_overflow", named))
            if (uint32 const trigger = eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell)
                bursts.insert(trigger);
    for (uint32 trigger : bursts)
    {
        LOG_DEBUG("module.classkit.frost", "Shattering Overflow of {}: {} made {} beyond the shell's cap: one pulse ({})",
                  caster->GetName(), spellId, overflow, trigger);
        caster->CastSpell(caster, trigger, true);
    }
}

// A rule's own script: a marker the rules are found by (EfClassKit.h Rules); nothing happens in it.
class spell_ef_kit_frost_rule : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_frost_rule);

    void Register() override { }
};

void AddEmberfallClassKitFrostScripts()
{
    RegisterSpellScript(spell_ef_kit_marked_absorb);
    RegisterSpellScript(spell_ef_kit_proc_attacker_stacks);
    RegisterSpellScript(spell_ef_kit_capped_stacks);
    RegisterSpellScript(spell_ef_kit_area_share);
    RegisterSpellScript(spell_ef_kit_orb_seek);
    RegisterSpellScript(spell_ef_kit_frost_rules);
    for (char const* name : { "spell_ef_kit_rule_shell_overflow", "spell_ef_kit_rule_orb_collapse", "spell_ef_kit_rule_first_contact",
                              "spell_ef_kit_rule_echo_pulse" })
        RegisterSpellScriptWithArgs(spell_ef_kit_frost_rule, name);
}
