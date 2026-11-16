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

// The respawn floor's rule (Emberfall feedback #593, docs/design/caves.md §13, docs/design/respawn.md §3.1), with no
// core types so tests/respawnfloor_tests.cpp runs it standalone.
//
// The realm's dynamic respawn (Map::ApplyDynamicModeRespawnScaling) divides a dying creature's respawn by the players
// in its whole zone; playerbots count, so a band's zone full of bots brought a cave's creatures back after the 45-s
// minimum while a party was still inside. A spawn with an ef_respawn_floor row (the creatures inside a cave, a mine, a
// cellar: Emberfall's compiler writes them) never comes back sooner than its floor after its death, plus its corpse's
// time as the core counts it. The floor never lengthens a respawn the core gave (a player alone waits the full base,
// which is longer), and a floor of 0 does nothing.

#ifndef EMBERFALL_RESPAWN_FLOOR_LOGIC_H
#define EMBERFALL_RESPAWN_FLOOR_LOGIC_H

#include <cstdint>

namespace Emberfall::RespawnFloor
{
    // The time (seconds since the epoch, as the core's m_respawnTime) the creature that died at `now` respawns at: the
    // core's `respawnAt` (its dynamic delay plus the corpse's), or `now + floorSecs + corpseSecs` if that is later.
    constexpr int64_t Floored(int64_t now, int64_t respawnAt, uint32_t floorSecs, uint32_t corpseSecs)
    {
        if (!floorSecs)
            return respawnAt;
        int64_t const earliest = now + int64_t(floorSecs) + int64_t(corpseSecs);
        return respawnAt < earliest ? earliest : respawnAt;
    }

    // The delay (seconds from now) Creature::SetRespawnTime takes for that time; 0 when the core's time stands.
    constexpr uint32_t DelayFor(int64_t now, int64_t respawnAt, uint32_t floorSecs, uint32_t corpseSecs)
    {
        int64_t const at = Floored(now, respawnAt, floorSecs, corpseSecs);
        return at == respawnAt ? 0u : uint32_t(at - now);
    }
}

#endif
