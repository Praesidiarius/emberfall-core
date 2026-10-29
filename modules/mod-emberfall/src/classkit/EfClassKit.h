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

class Aura;
class AuraEffect;
class Player;
class Spell;
class SpellInfo;
class Unit;
class WorldObject;

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
    // The stacked shell (Frost Shell): one shell aura per caster (school absorb; effect 2's value = the percent of max
    // health one stack absorbs; Spell.dbc StackAmount = the most stacks). Its absorb pool is the stacks' worth, the
    // stacks shown follow the pool (EfClassKitLogic.h ShellStacks: every started stack counts).
    //   AddShellStacks adds `stacks` stacks' worth to the pool (a worn stack is topped up), at most the cap; the duration
    //   becomes the longest of the remaining and `durationMs` (all stacks share it). The caster's conditional passives
    //   follow at once.
    void AddShellStacks(Unit* caster, uint32 shellSpell, int32 stacks, int32 durationMs);
    // The shell is absorbing `absorbed` of a blow: its stacks follow what will be left of the pool (spell_ef_kit_shell,
    // after the absorb, before the engine reduces the amount).
    void FollowShell(Aura* shell, int32 absorbed);
    // Whether `spellId` is a shell aura (the trigger of a spell_ef_kit_shatter kit spell, found by Load, or any spell
    // AddShellStacks was called with).
    bool IsShellSpell(uint32 spellId);
    // What one stack of the shell aura `shellSpell` of `caster` absorbs: effect 2's percent (its spell mods in: Deep
    // Shell, the Rimeholder's 8 pieces) of the caster's max health, scaled by the effect 1 spell mods (the Rimeholder's
    // 2 pieces).
    int32 ShellPerStack(Unit* caster, uint32 shellSpell);
    // The most a shell aura can hold: ShellPerStack x its stack maximum (src/emberfall_absorbs.cpp reports it).
    int32 ShellCap(Unit* caster, uint32 shellSpell);
    // Hit bonuses: every caster aura effect of type dummy whose misc value is `spellId` (the spell that hit) or, with misc
    // 0, whose class mask covers that spell (Hallowed Ground's burst for every holy sword), whose misc B is at most
    // `stacks` (the stacks the target carried; 0: any) and that has a trigger spell rolls its amount as a chance in
    // percent (0 or 100+: always) and then casts the trigger at the target; a stacking trigger (an aura with a stack
    // maximum) adds one stack through AddStacks instead (Splintering Cold, Driving Snow, Frozen Grip, Whiteout, Improved
    // Glacial Rupture). A trigger with a TargetAuraSpell goes only to a target carrying that aura of the caster's
    // (Oathblade's sword on a Condemned enemy; Hallowed Ground's burst on an enemy standing in it).
    //   A trigger whose effect 1 targets the caster (a self aura: Dawn Ready, Endless Crown's lengthening; a self stack
    //   aura: Twin Brand's Sunblade) goes to the caster instead, and only when `selfTriggers` (the spenders pass it for
    //   their first enemy only, so an area spend counts once). A trigger whose ExcludeCasterAuraSpell the caster carries
    //   is skipped (Heaven's Answer while Heaven's Answer Spent holds).
    void HitBonuses(Unit* caster, Unit* target, uint32 spellId, uint32 stacks, bool selfTriggers = true);
    //   A rule passive (IsRule, below) is never a hit bonus: its dummy's trigger is what the rule casts.
    // Talent rules (the Executioner's Talent Tree v1.0, Emberfall docs/design/classes/warrior-executioner.md §15; the
    // contracts: Emberfall's classkit/scripts_exec.py): a talent passive bound to one of the `spell_ef_kit_rule_*` aura
    // scripts, which do nothing by themselves. The ability's own scripts (spell_ef_kit_rules, spell_ef_kit_build_power)
    // and the kill hook find the caster's rules by that script and read their numbers from the passive's dummy effect.
    // Whether an aura is a rule passive (any of its aura scripts is a spell_ef_kit_rule_*).
    bool IsRule(Aura const* aura);
    // The caster's rules of script `script` that name `spellId`: the dummy effects whose misc is `spellId`, or misc 0 and a
    // class mask covering it; with `spellId` 0 the rules that name no spell (misc 0, no class mask: any kill).
    std::vector<AuraEffect const*> Rules(Unit* caster, char const* script, uint32 spellId);
    // Self stacks (the Paladin's Sunblades): adds `count` stacks of the caster's own aura `stackSpell`, up to its cap
    // (effect 1's value with the caster's spell mods: Crown of Blades raises it; EfClassKitLogic.h StackCap), refreshing
    // it; stacks above a cap that fell stay. Returns the stacks.
    uint32 AddSelfStacks(Unit* caster, uint32 stackSpell, int32 count);
    // Lengthens the caster's own aura `auraSpell` by `addMs`, at most `maxExtraSeconds` beyond its duration (spell mods
    // in; Endless Crown on Crown of Blades). Returns the ms added.
    int32 Lengthen(Unit* caster, uint32 auraSpell, int32 addMs, int32 maxExtraSeconds);
    // Whether a spell's effect 1 targets its caster (a self aura).
    bool TargetsCaster(uint32 spellId);
    // ---- The miss rules (Emberfall alpha1 feedback #142, #143, #146; docs/design/class-kit-pipeline.md "Misses"; the
    // core's half: src/server/game/Spells/EmberfallMissRules.h, PATCHES.md #26) ----
    // A miss, dodge, parry, evade or immunity is the player's loss: a spender pays in full (the core), a proc or stacks an
    // ability spends are spent (ConsumeOne, the spenders' own scripts), and a builder builds only for an enemy it struck
    // (Logic::Builds with StrikesOthers and Struck: Flow's marks, self stacks, spec power, the core's caster energize).
    // Whether a spell strikes others: an effect aimed at anyone but its caster (its builders need a hit; a self spell
    // with no other target, a cooldown that gives power, always builds).
    bool StrikesOthers(SpellInfo const* info);
    // The units other than the caster the cast struck (SPELL_MISS_NONE: no miss, dodge, parry, evade or immunity); the
    // hit rolls come with the target list, so this holds from the launch on (before a missile lands).
    uint32 Struck(Spell* spell, Unit const* caster);
    // Whether the cast struck `target` (SPELL_MISS_NONE).
    bool StruckUnit(Spell* spell, Unit const* target);
    // One charge (or one stack, the last one removes it) of the caster's aura `auraSpell`: the proc an ability spends,
    // hit or miss (spell_ef_kit_consume_aura: the Deathblow window, Echo Ready, Bastion's Resolve). False: no such aura.
    bool ConsumeOne(Unit* caster, uint32 auraSpell);
    // `consumed` stacks of `stackSpell` were shattered: every non-passive caster aura with a dummy effect whose misc value
    // is `stackSpell`, whose misc B (seconds) is above 0 and that has no trigger spell lasts the effect's amount in ms
    // longer per stack, until it has run misc B seconds beyond its spell's duration (Hoarstorm).
    void ExtendByStacks(Unit* caster, uint32 stackSpell, uint32 consumed);
    // `consumed` stacks of `stackSpell` were shattered: every caster aura of a stack-cooldown passive
    // (spell_ef_kit_stack_cooldown, found by Load) whose effect 1 misc is `stackSpell` takes effect 1's amount in ms per
    // stack off the cooldown of effect 1's trigger spell, at most effect 2's amount in seconds per cooldown (Winter's
    // Embrace on Hoarstorm). Nothing while that spell is not cooling down.
    void CooldownByStacks(Unit* caster, uint32 stackSpell, uint32 consumed);
    // Shortens the caster's running cooldown of `spell` by `cutMs` (never below nothing; SMSG_MODIFY_COOLDOWN tells the
    // client). Returns the ms taken (Oathbound Rush on Oathblade, Endless Crown on Crown of Blades).
    int32 ShortenCooldown(Unit* caster, uint32 spell, int32 cutMs);
    // A link of the Oathkeeper's chain (spell_ef_kit_cooldown_cut_pct): takes `pct` percent of the base cooldown of
    // `spell` (Spell.dbc's) off the caster's running cooldown, never more than is left (100: all of it), never cutting that cooldown as a whole below `floorMs` (0:
    // no floor; Solenne's Retribution 60 s: the cuts of one cooldown are kept on the player). SMSG_MODIFY_COOLDOWN tells
    // the client. Nothing while the spell is ready. Returns the ms taken.
    int32 CutCooldownPct(Unit* caster, uint32 spell, int32 pct, int32 floorMs = 0);
    // Empowerment: the sum of the caster's non-passive dummy aura effects with misc = `spell` and no trigger, in percent
    // (Solenne's Wrath: +100 % on Solenne's Blessing). RefreshEmpowered recomputes the caster's aura `spell` on the caster
    // and its group's members in the map (when the empowering aura comes or goes).
    int32 EmpowerPct(Unit* caster, uint32 spell);
    void RefreshEmpowered(Unit* caster, uint32 spell);
    // Launches stacks (Crown of Blades): every caster aura effect of type dummy whose misc value is its own spell (a
    // stacking aura), with a trigger and a class mask covering `launcher`, launches its amount of stacks (0: all) as
    // casts of the trigger: at `target` (amount above 0), or at the enemies within misc B yards of the caster, the nearest
    // first and round in turn (0); the stacks launched are taken off. Returns how many were launched.
    uint32 LaunchStacks(Unit* caster, Unit* target, SpellInfo const* launcher);
    // The unit's conditional passives (spell_ef_kit_conditional) recompute their amounts now.
    void RefreshConditionals(Unit* unit);
    // Whether a creature counts as a boss for the kits' crowd control (world boss, dungeon boss, boss rank).
    bool IsBoss(Unit const* unit);
    // The most injured ally of `caster` within `range` yards, by the share of health missing (EfClassKitLogic.h
    // MostInjured): the caster, its group's players and everything the caster or a group member controls (pets,
    // guardians: the .efparty companions); only friendly, living units below full health. nullptr: everyone is full
    // (the Warlock's Overflow and Harvest Tide, spell_ef_kit_transfuse).
    Unit* MostInjuredAlly(Unit* caster, float range);
    // The caster's party as a healer sees it (the Bloomtender: Seeds of Renewal, the echo heals, Verdant Ancient's release):
    // the caster, its group's players (in a raid only the caster's own subgroup), each one's .efparty companions and,
    // with `pets`, every other unit they control (pets, guardians); living, friendly, in the caster's map and within
    // `range` yards of `center` (the caster, an ally, the Ancient). No line-of-sight check: the callers decide.
    std::vector<Unit*> PartyUnits(Unit* caster, WorldObject const* center, float range, bool pets);

    // ---- A spec's own power (Emberfall docs/design/classes/druid-stormbringer.md §3; src/classkit/EfSpecPower.cpp) ----
    // ef_class_kit_spec_power (spec, class, power, max_power, drain_delay_ms, drain_per_second; optional table) and
    // ef_class_kit_spec_power_tier (spec, at_power, aura, requires_spell; optional, the Pyromancer's Heat): while a
    // player's active spec has a row, the player's power is that slot (the Stormbringer: runic power under the name
    // Static) instead of the class's (mana); its maximum is the slot's own plus the slot's max-power auras (Brooding Sky);
    // it starts at 0 and drains out of combat after the delay (in proportion to a larger pool; an aura
    // SPELL_AURA_MOD_POWER_REGEN_PERCENT of that power slows it, as Static Memory did until 2026-10-21). The class kits'
    // helper auras that last until cancelled are fight markers (First Flash Spent): they go when the fight ends.
    struct SpecPower
    {
        uint32 spec = 0;
        uint8 classId = 0;
        uint8 power = 0;           // Powers
        uint32 max = 0;            // stored units (runic power x 10)
        uint32 drainDelayMs = 0;
        uint32 drainPerSecond = 0; // stored units
        // ef_class_kit_spec_power_regen (optional; the Corsair's Energy, Emberfall docs/design/classes/warrior-corsair.md §3):
        // regenerated `regenPerSecond` stored units a second in and out of a fight times the slot's regen-percent auras
        // (Momentum), never haste; full at login and the spec choice with `startFull`; the fight markers go
        // `markersDelayMs` after the fight instead of at its end (Flow: 10 s).
        uint32 regenPerSecond = 0;
        bool startFull = false;
        uint32 markersDelayMs = 0;
        // `start_full` 2 (the Executioner's Openings, `start: keep`): kept as it was, at login (the core saves and loads
        // the slot) and through a level up (the core empties the runic power slot there; the module puts it back); the
        // spec choice still starts it empty.
        bool keep = false;
        // Tiers (ef_class_kit_spec_power_tier, optional; the Pyromancer's Heat): while the power is at least `at` stored
        // units and the player knows `requiredSpell` (0: always), the player carries the tier's aura (Blazing at 30, Inferno
        // at 70, Maximum at 100 Heat). The auras follow the power on every player tick, so a spell sees the tier it was
        // cast at even when its own cost takes the power below it (the cost is paid inside the cast).
        struct Tier
        {
            uint32 at = 0;         // stored units
            uint32 aura = 0;
            uint32 requiredSpell = 0;
        };
        std::vector<Tier> tiers;
    };
    // Loads ef_class_kit_spec_power (Load calls it); the fight markers are registered by Load from the kit spells.
    void LoadSpecPowers();
    void ClearFightMarkers();
    void AddFightMarker(uint32 spell);
    // The power of a spec, or nullptr (the spec keeps the class's power).
    SpecPower const* SpecPowerOf(uint32 spec);
    // Sets the player's power to its active spec's own (or back to the class's): at login (`login`), level change and spec
    // change.
    void ApplySpecPower(Player* player, bool login = false);
    // A spec power's own maximum against its slot's (the Executioner's Openings: 5, 50 stored of runic power's 1000; the
    // Pyromancer's Heat: 150, 1500 stored): what the hook after Player::UpdateMaxPower adds for `power` (negative or
    // positive; 0 for every other power and slot).
    int32 SpecPowerMaxDelta(Player const* player, uint8 power);
    // Every player tick: the drain out of combat and the end of the fight markers.
    void UpdateSpecPower(Player* player, uint32 diff);
    // Stored units per point shown (rage and runic power: 10).
    int32 PowerScale(uint8 power);
    // A hold on the player's spec power (Overheat, spell_ef_kit_power_hold): while at least one holds, nothing drains it
    // out of combat (its spenders are free by their own spell mods, so nothing consumes it either).
    void HoldSpecPower(Unit* owner, bool on);
    // Sets the player's spec power to `points` as shown (nothing while its power is the class's): Overheat's end, 0.
    void SetSpecPowerPoints(Unit* owner, int32 points);
    // The player's spec power as shown (0 while its power is the class's).
    int32 SpecPowerPoints(Unit* owner);
    // Gives `points` (as shown) of the caster's spec power (nothing while the caster's power is the class's mana) as an
    // energize of `spellId` (the combat log). Returns the stored units given.
    int32 GainSpecPower(Unit* caster, int32 points, uint32 spellId);
    // End bonuses: when the aura `auraSpell` (cast by `caster`) leaves `owner`, every dummy aura effect of the caster with
    // misc = auraSpell and a trigger casts the trigger with its amount as base points 1: on the caster when the trigger
    // targets its caster (Stored Charge when Galeward ends), else on the owner (Slow Waking when Becalm ends, Echoing
    // Peal when Stormpeal's stun ends).
    void EndBonuses(Unit* caster, Unit* owner, uint32 auraSpell);
    // Charges (Second Gust): `spell` was cast and its talent gives it `max` charges (spell_ef_kit_charges: the dummy's misc
    // a talent the player knows, or the spell itself: always, the Sunwarden's Solenne's Grace and Embrace); the charge aura `chargeAura` holds the
    // charges recharging as stacks and the time to the next as its duration. While a charge is left the cooldown is
    // cleared; with none left it ends with the next charge's recharge.
    void SpendCharge(Player* player, uint32 spell, uint32 chargeAura, uint32 max);
    // The charge aura ran out: one charge is back; the next one starts recharging when more are spent.
    void ChargeBack(Unit* owner, uint32 chargeAura, uint32 stacksBefore);
    // Charges given back by other spells (the Sunwarden: Solenne's Salvation, Solar Reckoning, Dawn's Renewal;
    // src/emberfall_classkit_sun.cpp): `count` of the charges recharging in `chargeAura` are at hand again (0: all, the
    // running recharge stopped, the next spend starts a fresh one); a running recharge runs on; at full charges nothing
    // (the charge is lost). The spell's cooldown is cleared. Returns the charges given back.
    uint32 RestoreCharges(Player* player, uint32 spell, uint32 chargeAura, uint32 count);
    // The next charge of `spell` comes `ms` sooner (Boundless Mercy): the charge aura's time left, and the spell's cooldown
    // with it while no charge is at hand; at 0 the charge is back.
    void ShortenCharge(Player* player, uint32 spell, uint32 chargeAura, int32 ms);

    // ---- A class's own power (Emberfall docs/design/classes/hunter-marksman.md: the Hunter's Focus; EfSpecPower.cpp) ----
    // ef_class_kit_class_power (class, power, max_power, regen_per_second, regen_out_of_combat; optional table): a power
    // the core never regenerates for a player (Focus: Player::Regenerate skips it, and its create maximum is 0 for a
    // player). For a player of that class whose power is that slot (no spec power in force) the module gives it its
    // maximum (`max_power` plus the slot's max-power auras: Wide Lungs), regenerates it every tick (in a fight
    // `regen_per_second` times the haste factor and the slot's regen-percent auras: Keen Draw, Held Breath; out of one
    // `regen_out_of_combat`, so every pull starts full) and fills it at login, level up and spec change.
    struct ClassPowerRule
    {
        uint8 classId = 0;
        uint8 power = 0;            // Powers
        uint32 max = 0;
        float regen = 0.0f;         // points a second in a fight
        float regenOutOfCombat = 0.0f;
    };
    // Loads ef_class_kit_class_power (Load calls it with the spec powers).
    void LoadClassPowers();
    // The class power of a class, or nullptr.
    ClassPowerRule const* ClassPowerOf(uint8 classId);
    // The base maximum of `power` for `player` from its class power (the hook after Player::UpdateMaxPower adds it, as the
    // core's create power would): the rule's maximum when `power` is its slot, else 0.
    uint32 ClassPowerBase(Player const* player, uint8 power);

    // ---- A player's melee auto attack by level (Emberfall docs/design/classes.md §4.7, feedback #168; EfSpecPower.cpp) ----
    // ef_class_kit_auto_attack (level, pct; optional table, from formulas.yaml auto_attack): a kit player's white swing
    // (main and off hand; UnitScript::ModifyMeleeDamage runs only for the auto attack, before armour) deals `pct` percent
    // of its damage at the player's level. Not the Hunter's Autoshot (a spell), not pets or companions, not weapon-damage
    // abilities.
    // Loads ef_class_kit_auto_attack (Load calls it with the spec powers).
    void LoadAutoAttack();
    // The percent of its damage a player's white swing deals at `level`, or -1 (no row: as it is).
    float AutoAttackPct(uint8 level);

    // `.efkit` / `.efkit dump`: the kit of the selected player's class and what it knows of it.
    void Dump(Player* player, std::vector<std::string>& lines);
    // `.efkit auras`: the class kit auras on a unit (the selected one), one line each: "EFAURA <spell> <stacks>
    // <amount 1> <amount 2> <amount 3> <ms left> <caster is the viewer>"; for a player also "EFAURA cd <spell> <ms left>"
    // per kit spell on cooldown (the server's, spell mods included); last "EFAURA end <name> <count> health <hp> <max>
    // combat <0|1>".
    void DumpAuras(Unit* unit, Player* viewer, std::vector<std::string>& lines);
}
