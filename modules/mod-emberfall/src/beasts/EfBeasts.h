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

#ifndef EMBERFALL_BEASTS_H
#define EMBERFALL_BEASTS_H

// The Hunter's beasts (Emberfall docs/design/hunter-beasts.md): the stable helpers every hunter feature shares (the
// taming, the Class Journey's Call from the Stable and Beast at Heel), and the tame choice. emberfall_beasts.cpp.

#include "beasts/EfBeastsLogic.h"

class Player;
class PetStable;

namespace Emberfall::Beasts
{
    // The first free stall of the hunter's open stalls; -1: none.
    int FreeStall(PetStable const* stable);

    // Puts the companion out (or waiting, or dismissed: the stable's current pet or its one unslotted hunter pet) into a
    // free stall. True with `stall` the stall it took, or -1 when there was nothing to put away; false: no stall free
    // (or the pet out is not a hunter's companion).
    bool PutAway(Player* player, int& stall);

    // The stable keeper's swap without a keeper: the living companion out goes into `stall` and the beast there comes out.
    bool SwapWith(Player* player, int stall);

    // The hunter's "When tamed" choice (the characters DB's ef_character_beast_pref; default: with me).
    TameTo TamePref(Player* player);

    // The stall of the beast the hunter picked in the field list (EFB CALL) when it is still stabled; -1: none. The pick is
    // used up.
    int TakeCallPick(Player* player);

    // Whether the hunter has picked a beast in the field list (not used up).
    bool HasCallPick(Player* player);

    // Opens the stable list in field mode (the stock list with the hunter as his own keeper).
    void OpenFieldList(Player* player);

    // Call from the Stable (content/data/classes/hunter.yaml call_from_the_stable): its button opens the list, the
    // list's Call casts it with the pick.
    constexpr unsigned kSpellCallFromTheStable = 103803;
}

#endif // EMBERFALL_BEASTS_H
