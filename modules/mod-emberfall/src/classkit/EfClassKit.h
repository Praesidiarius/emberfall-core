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
    void AddToShell(Unit* caster, uint32 shellSpell, int32 add, int32 durationMs);

    // `.efkit` / `.efkit dump`: the kit of the selected player's class and what it knows of it.
    void Dump(Player* player, std::vector<std::string>& lines);
}
