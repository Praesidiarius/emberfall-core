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

// Standalone tests of the dev vendors' gating (src/devvendor/EfDevVendorLogic.h; tests/run_tests.sh builds and runs
// them): with Emberfall.DevVendor.Enable off (a public realm) no spawn group is spawned, no dev vendor stays in the
// world and no gossip (so no vendor list) opens, whatever the group; on, only a manual, non-system group is spawned.

#include "EfDevVendorLogic.h"

#include <iostream>
#include <string>

using namespace Emberfall::DevVendor;

namespace
{
    int g_failed = 0, g_checks = 0;

    void check(bool ok, std::string const& what)
    {
        ++g_checks;
        if (!ok)
        {
            ++g_failed;
            std::cerr << "FAIL: " << what << "\n";
        }
    }

    void TestOffMeansNothing()
    {
        for (bool exists : { false, true })
            for (std::uint32_t flags = 0; flags < 0x20; ++flags)
                check(!SpawnsGroup(false, exists, flags), "off: no group is spawned (flags " + std::to_string(flags) + ")");
        check(!VendorStays(false), "off: a dev vendor in the world leaves it");
        check(!GossipOpens(false), "off: its gossip (and vendor list) opens nothing");
    }

    void TestOn()
    {
        check(SpawnsGroup(true, true, kFlagManualSpawn), "on: our manual group is spawned");
        check(!SpawnsGroup(true, false, kFlagManualSpawn), "on: a missing group is not");
        check(!SpawnsGroup(true, true, 0), "on: a non-manual group is not (it spawns by itself)");
        check(!SpawnsGroup(true, true, kFlagSystem), "on: a system group is not");
        check(!SpawnsGroup(true, true, kFlagSystem | kFlagManualSpawn), "on: a system group is not, whatever its flags");
        check(VendorStays(true) && GossipOpens(true), "on: the vendor stays and its gossip opens");
    }

    void TestIds()
    {
        // Emberfall content/data/ids.yaml spawn_group dev block 92001-92009; content/data/dev/npcs.yaml.
        check(kSpawnGroup == 92001, "the dev vendors' spawn group is 92001");
        check(kSwitch == "Emberfall.DevVendor.Enable", "the switch's name");
        check(kScriptName == "npc_ef_dev_vendor", "the script's name");
    }
}

int main()
{
    TestOffMeansNothing();
    TestOn();
    TestIds();
    std::cout << "dev vendor tests: " << g_checks << " checks, " << g_failed << " failed\n";
    return g_failed ? 1 : 0;
}
