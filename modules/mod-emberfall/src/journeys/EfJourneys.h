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

// The Class Journeys' calls for the rest of the module and for mod-playerbots (emberfall_journeys.cpp; Emberfall
// docs/design/class-journeys/README.md).

#ifndef EMBERFALL_JOURNEYS_H
#define EMBERFALL_JOURNEYS_H

#include "Define.h"

class Player;

namespace Emberfall::Journeys
{
    // The Hunter's taming (hunter.md §3): 0 when the player may tame a beast of this creature_template.rank, else the
    // level of the journey step that opens it (the refusal names it). A step's quest in the log opens it already (the
    // step's lesson is that tame).
    uint32 TameGateLevel(Player* player, uint32 creatureRank);

    // A tame happened: the journey's tame credits for the beast's rank and level (ef_journey_tame_credit).
    void CreditTame(Player* player, uint32 creatureRank, uint32 beastLevel);

    // A playerbot: every step at or below its level counts as done (README §2.5; mod-playerbots calls it where it
    // applies a bot's talents, at the bot's making and each level-up).
    void BotSteps(Player* bot);

    // The step's mark (or second spell) of the class's journey: true for a spell some step teaches.
    bool IsJourneySpell(uint32 spell);
}

#endif
