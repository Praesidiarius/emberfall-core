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

#ifndef EMBERFALL_PET_TAUNT_H
#define EMBERFALL_PET_TAUNT_H

// EMBERFALL: a companion's autocast taunt (PATCHES.md #44; Emberfall docs/design/classes/hunter-marksman.md §41). Upstream's
// PetAI autocasts an offensive spell at the pet's victim whenever it is ready, so a taunt goes off at the pull, while the
// enemy already attacks the pet: Spell::EffectTaunt then does nothing (the caster is the current victim) and the cooldown
// runs, and the hunter's first shots take the enemy off the companion with no taunt left for the next 10 s. On our own
// data a hunter's companion autocasts a taunt (an attack-me effect or a taunt aura) only at an enemy that attacks someone
// else (its hunter, a group member). A taunt the player casts from the pet bar, other pets and stock data: as upstream.
// No core header: modules/mod-emberfall/tests/pettaunt_tests.cpp tests this file on its own.

namespace Emberfall::PetTaunt
{
    // Whether the pet AI holds back an autocast taunt this time.
    constexpr bool AutocastWaits(bool ownData, bool hunterPet, bool isTaunt, bool enemyHasVictim, bool enemyAttacksPet)
    {
        return ownData && hunterPet && isTaunt && (!enemyHasVictim || enemyAttacksPet);
    }
}

#endif // EMBERFALL_PET_TAUNT_H
