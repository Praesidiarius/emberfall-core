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

// The Frostguard's rules the shared kit scripts call (src/emberfall_classkit.cpp), built in
// src/emberfall_classkit_frost.cpp so the spec's rules stay in its own file.

#ifndef EMBERFALL_FROST_RULES_H
#define EMBERFALL_FROST_RULES_H

#include "Define.h"

class Unit;

namespace Emberfall::ClassKit::FrostRules
{
    // Shattering Overflow (ankarna's hand-off of 2026-11-19, Emberfall docs/design/classes/mage-frostguard.md §14): one
    // ability activation (`spellId`) made `overflow` of shell beyond the cap (EfClassKit.h AddShellStacks' return, summed
    // over the whole activation): with anything over, each spell_ef_kit_rule_shell_overflow rule of the caster's that
    // names the spell, or no spell (any shell gain), casts its burst ONCE at the caster with the burst's own numbers. Call
    // it once per activation, after the activation's whole shell gain.
    void OverflowPulse(Unit* caster, uint32 spellId, int64 overflow);
}

#endif
