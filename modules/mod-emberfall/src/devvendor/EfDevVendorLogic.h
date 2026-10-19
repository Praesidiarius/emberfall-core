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

// The dev vendors' gating, without the core (Emberfall docs/design/gear/frostguard-set.md §5; content/data/dev/npcs.yaml).
// emberfall_devvendor.cpp uses it; tests/devvendor_tests.cpp (tests/run_tests.sh) checks it, among others that with the
// switch off nothing is spawned, nothing stays in the world and no vendor list opens.
//
// The dev vendors stand in a MANUAL spawn group (spawn_group_template flag 0x4): the core never spawns such a group by
// itself. The module spawns it on its map at boot only with Emberfall.DevVendor.Enable = 1 (the local dev server,
// Emberfall's tools/server/ef_server.sh); a public realm (tools/gameserver) leaves the switch 0, so the group stays out
// of the world there. The NPCs' script is the second lock: with the switch off they leave the world at once and their
// gossip (and so their vendor list) opens nothing.

#ifndef EMBERFALL_DEV_VENDOR_LOGIC_H
#define EMBERFALL_DEV_VENDOR_LOGIC_H

#include <cstdint>
#include <string_view>

namespace Emberfall::DevVendor
{
    // The config switch (AC_EMBERFALL_DEV_VENDOR_ENABLE), default off. Independent of Emberfall.TestTools.Enable, which
    // the closed alpha realm has on for every player.
    constexpr std::string_view kSwitch = "Emberfall.DevVendor.Enable";
    // content/data/dev/npcs.yaml `spawn_groups` (Emberfall ids.yaml spawn_group, dev block 92001-92009).
    constexpr std::uint32_t kSpawnGroup = 92001;
    // The creature script every dev vendor names (the SQL compiler requires it of an NPC in a manual spawn group).
    constexpr std::string_view kScriptName = "npc_ef_dev_vendor";
    // The core's SpawnGroupFlags (SpawnData.h).
    constexpr std::uint32_t kFlagSystem = 0x01;
    constexpr std::uint32_t kFlagManualSpawn = 0x04;

    // Whether the module spawns a spawn group at boot: only with the switch on, and only a manual group of ours (a
    // non-manual group spawns by itself; a system group is the core's).
    inline bool SpawnsGroup(bool switchOn, bool groupExists, std::uint32_t groupFlags)
    {
        return switchOn && groupExists && (groupFlags & kFlagManualSpawn) && !(groupFlags & kFlagSystem);
    }

    // Whether a dev vendor that is in the world (say a GM's `.npc add`) may stay there: only with the switch on.
    inline bool VendorStays(bool switchOn) { return switchOn; }

    // Whether its gossip (the menu with "Let me see what you have.", and so the vendor list) opens.
    inline bool GossipOpens(bool switchOn) { return switchOn; }
}

#endif
