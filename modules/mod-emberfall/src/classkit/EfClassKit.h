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

#pragma once

// Emberfall class kits (Emberfall docs/design/class-kit-pipeline.md): our own class abilities on a world whose
// classes would otherwise keep their stock spell kits (the "ef-classes" profile: the stock world, our kits).
//
// The kit itself is data: Emberfall's compiler writes the spells (spell_dbc and the shared *_dbc rows), the
// trainers' lists, the starting skill line and buttons, the specs and talent trees (the ef_talent_* tables, run by
// src/talents) and two tables of this module:
//
//   ef_class_kit        class, skill_line (the kit's class skill line), stock_family (the SpellFamilyName of the
//                       stock kit the class replaces)
//   ef_class_kit_spell  class, spell, level, source (1 start, 2 trainer, 3 spec baseline, 4 talent, 5 helper)
//
// This module adds what data cannot: for a player whose class has a kit it takes away whatever of the stock kit
// still reaches the character (a stock class skill line, a spell of the stock family, a button pointing at a
// spell it no longer knows, an aura of the stock family) at login and at every level change, and runs the mechanics
// the kits' spell scripts share (stacks, the shell, src/emberfall_classkit.cpp); a GM command
// shows the state. Switch `Emberfall.ClassKit.Enable` (default 0; AC_EMBERFALL_CLASS_KIT_ENABLE).

#include "Define.h"

#include <string>
#include <vector>

class Player;
class Unit;

namespace Emberfall::ClassKit
{
    enum class Source : uint8 { Start = 1, Trainer = 2, Spec = 3, Talent = 4, Helper = 5 };

    struct KitSpell
    {
        uint32 spell = 0;
        uint8 level = 0;
        Source source = Source::Trainer;
    };

    struct Kit
    {
        uint8 classId = 0;
        uint32 skillLine = 0;
        uint32 stockFamily = 0;
        std::vector<KitSpell> spells;
    };

    void ReadConfig(bool reload);
    // Loads ef_class_kit and ef_class_kit_spell; false when the system is off or a table is missing.
    bool Load();
    bool Enabled();
    // The kit of the player's class, or nullptr (no kit, or the system is off).
    Kit const* KitOf(Player const* player);

    // Takes away the stock kit's remains (class skill lines other than the kit's, stock-family spells, buttons of
    // unknown spells); `removed` gets one line per thing taken. Returns how many things were taken.
    uint32 Purge(Player* player, std::vector<std::string>* removed = nullptr);

    void OnLogin(Player* player);
    void OnLevelChanged(Player* player);

    // Mechanics the kits' spell scripts share (src/emberfall_classkit.cpp).
    // Adds `count` stacks of `stackSpell` (cast by `caster`) to `target`, up to the aura's maximum, refreshing it; then
    // the caster's threshold passives (a dummy aura effect with misc = stackSpell, misc B = the stacks it needs and a
    // trigger spell: cast on the target with the effect's amount, negated) follow the new count. Returns the stacks.
    uint32 AddStacks(Unit* caster, Unit* target, uint32 stackSpell, int32 count);
    // Takes up to `count` stacks (0: all) of `stackSpell` from `target`; returns how many were taken.
    uint32 TakeStacks(Unit* caster, Unit* target, uint32 stackSpell, uint32 count);
    // One shell per caster: `add` joins the shell aura `shellSpell` (school absorb, effect 2 = its cap in percent of
    // max health), capped; its duration becomes the longest of the remaining and `durationMs`.
    //   The shell spell's own spell mods apply: its effect 1's (SPELLMOD_EFFECT1) scale every amount added, its effect
    //   2's move the cap (Deep Shell, Winter's Embrace). The caster's conditional passives follow at once.
    void AddToShell(Unit* caster, uint32 shellSpell, int32 add, int32 durationMs);
    // Whether `spellId` is a shell aura (the trigger of a spell_ef_kit_shatter kit spell, found by Load, or any spell
    // AddToShell was called with).
    bool IsShellSpell(uint32 spellId);
    // The most a shell aura `shellSpell` of `caster` can hold: its effect 2's percent (spell mods included) of the
    // caster's max health (AddToShell's cap; src/emberfall_absorbs.cpp reports it).
    int32 ShellCap(Unit* caster, uint32 shellSpell);
    // Hit bonuses: every caster aura effect of type dummy whose misc value is `spellId` (the spell that hit), whose misc
    // B is at most `stacks` (the stacks the target carried; 0: any) and that has a trigger spell rolls its amount as a
    // chance in percent (0 or 100+: always) and then casts the trigger at the target; a stacking trigger (an aura with a
    // stack maximum) adds one stack through AddStacks instead (Splintering Cold, Driving Snow, Frozen Grip, Whiteout,
    // Improved Glacial Rupture).
    //   A trigger whose effect 1 targets the caster (a self aura: Dawn Ready, Endless Crown's lengthening; a self stack
    //   aura: Twin Brand's Sunblade) goes to the caster instead, and only when `selfTriggers` (the spenders pass it for
    //   their first enemy only, so an area spend counts once). A trigger whose ExcludeCasterAuraSpell the caster carries
    //   is skipped (Heaven's Answer while Heaven's Answer Spent holds).
    void HitBonuses(Unit* caster, Unit* target, uint32 spellId, uint32 stacks, bool selfTriggers = true);
    // Self stacks (the Paladin's Sunblades): adds `count` stacks of the caster's own aura `stackSpell`, up to its cap
    // (effect 1's value with the caster's spell mods: Crown of Blades raises it; EfClassKitLogic.h StackCap), refreshing
    // it; stacks above a cap that fell stay. Returns the stacks.
    uint32 AddSelfStacks(Unit* caster, uint32 stackSpell, int32 count);
    // The caster's heal per stack spent (Wrath's Mercy): every passive caster aura with a dummy effect whose misc value is
    // `stackSpell`, misc B 0 and no trigger heals its amount in percent of max health per stack. Returns the health given.
    int32 SpendHeals(Unit* caster, uint32 stackSpell, uint32 spent);
    // Lengthens the caster's own aura `auraSpell` by `addMs`, at most `maxExtraSeconds` beyond its duration (spell mods
    // in; Endless Crown on Crown of Blades). Returns the ms added.
    int32 Lengthen(Unit* caster, uint32 auraSpell, int32 addMs, int32 maxExtraSeconds);
    // Whether a spell's effect 1 targets its caster (a self aura).
    bool TargetsCaster(uint32 spellId);
    // `consumed` stacks of `stackSpell` were shattered: every non-passive caster aura with a dummy effect whose misc value
    // is `stackSpell`, whose misc B (seconds) is above 0 and that has no trigger spell lasts the effect's amount in ms
    // longer per stack, until it has run misc B seconds beyond its spell's duration (Hoarstorm).
    void ExtendByStacks(Unit* caster, uint32 stackSpell, uint32 consumed);
    // The unit's conditional passives (spell_ef_kit_conditional) recompute their amounts now.
    void RefreshConditionals(Unit* unit);
    // Whether a creature counts as a boss for the kits' crowd control (world boss, dungeon boss, boss rank).
    bool IsBoss(Unit const* unit);
    // The most injured ally of `caster` within `range` yards, by the share of health missing (EfClassKitLogic.h
    // MostInjured): the caster, its group's players and everything the caster or a group member controls (pets,
    // guardians: the .efparty companions); only friendly, living units below full health. nullptr: everyone is full
    // (the Warlock's Overflow and Harvest Tide, spell_ef_kit_transfuse).
    Unit* MostInjuredAlly(Unit* caster, float range);

    // ---- A spec's own power (Emberfall docs/design/classes/druid-stormbringer.md §3; src/classkit/EfSpecPower.cpp) ----
    // ef_class_kit_spec_power (spec, class, power, max_power, drain_delay_ms, drain_per_second; optional table): while a
    // player's active spec has a row, the player's power is that slot (the Stormbringer: runic power under the name
    // Static) instead of the class's (mana); it starts at 0 and drains out of combat after the delay (an aura
    // SPELL_AURA_MOD_POWER_REGEN_PERCENT of that power slows it: Static Memory). The class kits' helper auras that last
    // until cancelled are fight markers (First Flash Spent): they go when the fight ends.
    struct SpecPower
    {
        uint32 spec = 0;
        uint8 classId = 0;
        uint8 power = 0;           // Powers
        uint32 max = 0;            // stored units (runic power x 10)
        uint32 drainDelayMs = 0;
        uint32 drainPerSecond = 0; // stored units
    };
    // Loads ef_class_kit_spec_power (Load calls it); the fight markers are registered by Load from the kit spells.
    void LoadSpecPowers();
    void ClearFightMarkers();
    void AddFightMarker(uint32 spell);
    // The power of a spec, or nullptr (the spec keeps the class's power).
    SpecPower const* SpecPowerOf(uint32 spec);
    // Sets the player's power to its active spec's own (or back to the class's): at login, level change and spec change.
    void ApplySpecPower(Player* player);
    // Every player tick: the drain out of combat and the end of the fight markers.
    void UpdateSpecPower(Player* player, uint32 diff);
    // Stored units per point shown (rage and runic power: 10).
    int32 PowerScale(uint8 power);
    // Gives `points` (as shown) of the caster's spec power (nothing while the caster's power is the class's mana) as an
    // energize of `spellId` (the combat log). Returns the stored units given.
    int32 GainSpecPower(Unit* caster, int32 points, uint32 spellId);
    // End bonuses: when the aura `auraSpell` (cast by `caster`) leaves `owner`, every dummy aura effect of the caster with
    // misc = auraSpell and a trigger casts the trigger with its amount as base points 1: on the caster when the trigger
    // targets its caster (Stored Charge when Galeward ends), else on the owner (Slow Waking when Becalm ends, Echoing
    // Peal when Stormpeal's stun ends).
    void EndBonuses(Unit* caster, Unit* owner, uint32 auraSpell);
    // Charges (Second Gust): `spell` was cast and its talent gives it `max` charges; the charge aura `chargeAura` holds the
    // charges recharging as stacks and the time to the next as its duration. While a charge is left the cooldown is
    // cleared; with none left it ends with the next charge's recharge.
    void SpendCharge(Player* player, uint32 spell, uint32 chargeAura, uint32 max);
    // The charge aura ran out: one charge is back; the next one starts recharging when more are spent.
    void ChargeBack(Unit* owner, uint32 chargeAura, uint32 stacksBefore);

    // `.efkit` / `.efkit dump`: the kit of the selected player's class and what it knows of it.
    void Dump(Player* player, std::vector<std::string>& lines);
    // `.efkit auras`: the class kit auras on a unit (the selected one), one line each: "EFAURA <spell> <stacks>
    // <amount 1> <amount 2> <amount 3> <ms left> <caster is the viewer>"; for a player also "EFAURA cd <spell> <ms left>"
    // per kit spell on cooldown (the server's, spell mods included); last "EFAURA end <name> <count> health <hp> <max>
    // combat <0|1>".
    void DumpAuras(Unit* unit, Player* viewer, std::vector<std::string>& lines);
}
