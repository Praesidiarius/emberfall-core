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

// A player's active spec for everyone who sees the player (Emberfall docs/design/playerbots-pilot.md §19): the talent
// system writes it into the second byte of UNIT_FIELD_BYTES_1, a public field every client in view already receives
// (for players the byte is free: the core uses it for a pet's talent points only). The client (client/src/app/
// UnitSpec.h, the same golden values in its tests) names another unit's spec power by it: a Corsair's Energy, not the
// Executioner's Openings. 0: no spec yet (or an older server: the client falls back to its first spec of the slot). No
// addon message, so neither `protocol` nor `min_client` moves.

#ifndef EMBERFALL_EFSPECFIELD_H
#define EMBERFALL_EFSPECFIELD_H

#include <cstdint>

namespace Emberfall::Talents::SpecField
{
    constexpr uint8_t kByte = 1;  // UNIT_FIELD_BYTES_1's byte (UNIT_BYTES_1_OFFSET_PET_TALENTS)

    // The byte for a spec ID (spec IDs of the class kits are 11-93; one beyond a byte is not shown).
    constexpr uint8_t Encode(uint32_t specId) { return specId <= 0xFF ? static_cast<uint8_t>(specId) : 0; }

    // The spec from the whole field value, as the client reads it.
    constexpr uint32_t Decode(uint32_t bytes1) { return (bytes1 >> (8 * kByte)) & 0xFF; }
}

#endif
