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

#ifndef EMBERFALL_PET_SIZE_MODULE_H
#define EMBERFALL_PET_SIZE_MODULE_H

// The companion's size and growth (src/emberfall_petsize.cpp) for the module's other parts: the taming HELLO sends every
// companion's growth (GROW), the beast at heel (emberfall_journeys_hunter.cpp) stands at its stabled beast's size.

#include "Define.h"

class Player;

namespace Emberfall::PetSize
{
    // GROW for every companion the hunter has (out, stabled, sent off), after the client's taming HELLO.
    void SendGrowth(Player* player);

    // The object scale a companion of CreatureFamily `family` wearing `display` stands at with its growth (pet number
    // `pet`, character_pet.id); false without the size data for them (then the core's size).
    bool ScaleForPet(uint32 family, uint32 display, uint32 pet, float& scale);
}

#endif
