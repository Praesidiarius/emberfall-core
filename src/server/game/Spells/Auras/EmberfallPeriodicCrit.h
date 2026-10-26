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

// EMBERFALL: periodic crits (PATCHES.md #23, Emberfall alpha1 feedback #79) - which crit chance a periodic
// damage, leech or heal tick rolls. Upstream (3.3.5) lets a tick crit only with SPELL_AURA_ABILITY_PERIODIC_CRIT or for
// Rupture; Emberfall lets every tick of a player's spell crit, on the chance the spell's damage class already stands
// for: melee class the melee crit, ranged class the ranged crit, magic class the spell crit of its school. A spell of
// damage class none (a helper cast with an amount worked out already, e.g. a share of a heal that rolled its own crit)
// and a spell with SPELL_ATTR2_CANT_CRIT never crit. No core header: the raw values are checked against the enums in
// SpellAuraEffects.cpp, and modules/mod-emberfall/tests/periodiccrit_tests.cpp tests this file on its own.

#ifndef EMBERFALL_PERIODIC_CRIT_H
#define EMBERFALL_PERIODIC_CRIT_H

#include <cstdint>

namespace Emberfall::PeriodicCrit
{
    // AuraType values (SpellAuraDefines.h)
    constexpr uint32_t AURA_PERIODIC_DAMAGE = 3;
    constexpr uint32_t AURA_PERIODIC_HEAL = 8;
    constexpr uint32_t AURA_PERIODIC_LEECH = 53;
    // SpellDmgClass values (SharedDefines.h)
    constexpr uint32_t DMG_CLASS_NONE = 0;
    constexpr uint32_t DMG_CLASS_MAGIC = 1;
    constexpr uint32_t DMG_CLASS_MELEE = 2;
    constexpr uint32_t DMG_CLASS_RANGED = 3;

    // Which chance a tick rolls: none, or the caster's chance for one attack type (the spell's school for magic).
    enum class Source
    {
        None,
        Melee,      // BASE_ATTACK
        Ranged,     // RANGED_ATTACK
        Spell,      // the school's spell crit (SpellDoneCritChance's magic branch)
    };

    // `playerSpell`: the caster is a player or a player's unit (GetSpellModOwner); creatures' ticks keep upstream's
    // rule (mobs never crit with spells).
    constexpr Source Rule(uint32_t auraType, uint32_t dmgClass, bool cantCrit, bool playerSpell)
    {
        if (!playerSpell || cantCrit)
            return Source::None;
        if (auraType != AURA_PERIODIC_DAMAGE && auraType != AURA_PERIODIC_LEECH && auraType != AURA_PERIODIC_HEAL)
            return Source::None;
        switch (dmgClass)
        {
            case DMG_CLASS_MELEE:
                return Source::Melee;
            case DMG_CLASS_RANGED:
                return Source::Ranged;
            case DMG_CLASS_MAGIC:
                return Source::Spell;
            default:
                return Source::None;
        }
    }
}

#endif
