/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef EMBERFALL_PET_AURA_H
#define EMBERFALL_PET_AURA_H

// EMBERFALL: a player's companion auras (PATCHES.md #43; Emberfall docs/design/classes/hunter-marksman.md §40). Upstream's
// UnitAura::FillTargetMap gives an APPLY_AREA_AURA_PET effect to the aura's owner and to the owner's own owner: the stock
// spells of that kind are cast by a pet (its aura reaches the pet and its master). Our class kits cast it from the
// player for "an aura the companion holds while near" (the Marksman's Pack Bond, Thick Pelt, Steady Companion, Hunting
// Pair, the companion's share): upstream gave it to the hunter and never to the companion. For a class kit's spell
// (family 100 + class) on our own data, with a player as the owner, the effect reaches the player's companion within
// its radius and not the player. Everything else as upstream. No core header: modules/mod-emberfall/tests/petaura_tests.cpp
// tests this file on its own.

#include <cstdint>

namespace Emberfall::PetAura
{
    // Whether an APPLY_AREA_AURA_PET effect goes to the owner's companion instead of the owner (and its owner).
    constexpr bool ToCompanion(bool ownData, uint32_t spellFamily, bool ownerIsPlayer)
    {
        return ownData && ownerIsPlayer && spellFamily >= 100;
    }

    // Whether the companion takes it: one there, alive, within the effect's radius.
    constexpr bool CompanionInReach(bool hasCompanion, bool alive, float distance, float radius)
    {
        return hasCompanion && alive && distance <= radius;
    }
}

#endif // EMBERFALL_PET_AURA_H
