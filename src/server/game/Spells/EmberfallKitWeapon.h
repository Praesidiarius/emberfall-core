/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
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

#ifndef EMBERFALL_KIT_WEAPON_H
#define EMBERFALL_KIT_WEAPON_H

// EMBERFALL: the class kits' weapon strikes (PATCHES.md #28; Emberfall docs/design/classes.md §4.8, 2026-10-30: the
// physical damage abilities deal weapon damage, Classic-style, so a better weapon makes them hit harder). Upstream's
// Spell::EffectWeaponDmg knows "weapon damage + N" (SPELL_EFFECT_WEAPON_DAMAGE, NORMALIZED_WEAPON_DMG) and "N % of it"
// only as a second effect (WEAPON_PERCENT_DAMAGE), which a kit spell with three effects has no room for, and it handles
// every weapon effect of a spell at the last one, whichever units that one reaches: a kit strike whose effect 1 hits the
// target and effect 2 the enemies round it (a splash that leaves the target out) dealt the target nothing. For a spell of
// a class kit's family (100 + class: only Emberfall's own data has one), the core:
//   - counts only the weapon effects that reach the unit (its target info's effect mask), the damage at the last of them;
//   - reads a weapon effect's misc value as the percent of the weapon's damage it deals (0: 100 %), its value (the
//     points) added after the percent: misc 45, value 12 = 45 % of the weapon's damage + 12 (WEAPON_PERCENT_DAMAGE
//     effects multiply into the same percent).
// The weapon's damage is upstream's (CalculateDamage: the weapon's own damage plus attack power / 14 x the weapon's
// speed, normalised for NORMALIZED_WEAPON_DMG; the off hand's half; the total percent auras on physical strikes, which
// also scale the points). No core header: modules/mod-emberfall/tests/kitweapon_tests.cpp tests this file on its own.

#include <cstdint>

namespace Emberfall::KitWeapon
{
    // Whether a spell family is a class kit's (tables.py SPELL_FAMILIES: 100 + the class, professions 120); stock
    // families end at 17.
    constexpr bool IsKitFamily(uint32_t family)
    {
        return family >= 100;
    }

    // A weapon effect's percent of the weapon's damage: its misc value, or 100 without one.
    constexpr float EffectPct(int32_t miscValue)
    {
        return miscValue > 0 ? float(miscValue) : 100.0f;
    }

    // The strike's damage before the hit's bonuses: `pct` percent of `weapon` (the weapon's rolled damage), then the
    // points; never below 0.
    constexpr int32_t Strike(int32_t weapon, float pct, int32_t points)
    {
        int32_t const damage = int32_t(float(weapon) * pct / 100.0f) + points;
        return damage > 0 ? damage : 0;
    }
}

#endif // EMBERFALL_KIT_WEAPON_H
