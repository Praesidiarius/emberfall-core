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

// Emberfall profession specialisations (Emberfall docs/design/professions.md §10): the trees loaded from the
// ef_prof_* world tables, the per-character ranks in ef_character_prof_*, the EFP protocol, the rank spells and the
// effects the rank spells cannot carry themselves (extra yield and upgrades on loot, extra output, refunds, upgrades
// and discovery on a craft, far nodes on the minimap, stronger bandages and proc glyphs, a bandage's cleanse, cheaper
// repairs), and the reset at the profession trainer. The hooks are src/emberfall_profspecs.cpp.
//
// On when the ef_prof_* tables have rows and Emberfall.ProfSpecs.Enable (default 1; AC_EMBERFALL_PROFSPECS_ENABLE) is
// set; independent of the talent system and of Emberfall.OwnData.

#pragma once

#include "Define.h"
#include "ObjectGuid.h"

#include <string>
#include <string_view>
#include <vector>

class Creature;
class Player;
class Spell;
class SpellInfo;
class Unit;
struct Loot;
struct SkillLineAbilityEntry;

namespace Emberfall::ProfSpecs
{
    void ReadConfig(bool reload);
    // Loads the ef_prof_* tables (startup, `.efprof reload`); false (and the system off) when they are missing,
    // empty or inconsistent.
    bool Load();
    bool Enabled();

    // Hooks.
    void OnLogin(Player* player);
    void HandleMessage(Player* player, std::string_view body);   // the text after `EFP\t`
    void OnSetSkill(Player* player, uint32 skill, uint32 newValue);
    void OnBeforeSendLoot(Player* player, ObjectGuid lootGuid, Loot* loot);
    void OnCraft(Player* player, SkillLineAbilityEntry const* ability);
    void OnUpdate(Player* player, uint32 diff);
    // Potency (feedback #204): a periodic tick (damage or heal: bandages) or a spell's direct damage (the proc glyphs'
    // hits) of `caster`'s, raised by its ranks. Cheap for every spell the trees do not name.
    void OnTick(Unit* target, Unit* caster, uint32& amount, SpellInfo const* spellInfo);
    void OnSpellDamage(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo);
    // Cleanse: a bandage (an item spell of a node's families) going on draws a poison or closes a bleed, by chance.
    void OnSpellCast(Player* player, Spell* spell);
    // Repair discounts: the vendor's repair price times the ranks' factor.
    void OnRepair(Player* player, float& discountMod);
    // The profession trainer's gossip: its own menu plus a "reset" line per tree with points; true when handled.
    bool OnGossipHello(Player* player, Creature* creature);
    bool OnGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action);

    // GM commands (`.efprof ...`).
    void Dump(Player* player, std::vector<std::string>& out);
    bool GmReset(Player* player, uint32 skill, std::string& out);
}
