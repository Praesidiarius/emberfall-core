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

// `.eftest zone` (src/emberfall_testzone.cpp): a test character put where a player arriving at a zone would be. The
// command itself is registered with the other test tools (src/emberfall_testtools.cpp), which gate it.

#ifndef EMBERFALL_TEST_ZONE_H
#define EMBERFALL_TEST_ZONE_H

#include "Define.h"

#include <string_view>

class ChatHandler;
class Player;

namespace Emberfall::TestZone
{
    // `.eftest level`'s change: the level, XP 0, the talent points of the level (the talent system's level hook), full
    // health and mana.
    void SetLevel(Player* player, uint8 level);

    // Every weapon skill the character has (the class's, all given at creation) and Defence at the level's maximum
    // (Emberfall feedback #130): GiveLevel only raises their cap, so a character lifted from 10 to 40 kept its level-10
    // skill and missed. SetLevel does it; so does the Warden's level 60.
    void MaxCombatSkills(Player* player);

    // `.eftest zone list`: the zones in route order with their levels.
    bool List(ChatHandler* handler);

    // `.eftest zone <zone> [level]` (level 0: the zone's entry level).
    bool Arrive(ChatHandler* handler, Player* player, std::string_view zone, uint32 level);
}

#endif
