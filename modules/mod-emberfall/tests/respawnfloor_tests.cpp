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

// Standalone tests of the respawn floor's rule (src/respawnfloor/EfRespawnFloorLogic.h; tests/run_tests.sh builds and
// runs them): the floor holds against the dynamic respawn's shortening, never lengthens the core's time, and counts the
// corpse as the core does.

#include "EfRespawnFloorLogic.h"

#include <iostream>
#include <string>

using namespace Emberfall::RespawnFloor;

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

    // the core's m_respawnTime at a death: now + the dynamic delay + the corpse's time
    constexpr int64_t CoreTime(int64_t now, uint32_t base, uint32_t players, uint32_t corpse)
    {
        double const factor = 1.5 / players;                       // the realm's Respawn.DynamicRateCreature
        uint32_t delay = base;
        if (factor < 1.0 && base > 45)
        {
            delay = uint32_t(base * factor + 0.999999);
            if (delay < 45)                                         // Respawn.DynamicMinimumCreature
                delay = 45;
        }
        return now + delay + corpse;
    }
}

int main()
{
    int64_t const now = 1'800'000'000;

    // alone in the zone: the cave's full base (600 s) stands, longer than the floor
    check(Floored(now, CoreTime(now, 600, 1, 60), 300, 60) == now + 660, "alone: the base stands");
    check(DelayFor(now, CoreTime(now, 600, 1, 60), 300, 60) == 0, "alone: nothing to change");
    // two in the zone: 450 s, still over the floor
    check(Floored(now, CoreTime(now, 600, 2, 60), 300, 60) == now + 510, "two: 450 s stands");
    // three: 300 s, the floor exactly
    check(DelayFor(now, CoreTime(now, 600, 3, 60), 300, 60) == 0, "three: the floor exactly");
    // a zone full of bots (30): the core's 45-s minimum, lifted to the floor
    check(Floored(now, CoreTime(now, 600, 30, 60), 300, 60) == now + 360, "thirty: lifted to the floor");
    check(DelayFor(now, CoreTime(now, 600, 30, 60), 300, 60) == 360, "thirty: the delay the core is given");
    // the corpse counts as the core counts it (an elite's corpse lies longer)
    check(Floored(now, CoreTime(now, 600, 30, 300), 300, 300) == now + 600, "an elite's corpse on top");
    // no floor: the core's time as it is
    check(Floored(now, now + 105, 0, 60) == now + 105, "no floor");
    check(DelayFor(now, now + 105, 0, 60) == 0, "no floor: no delay");
    // never shorter than the core's, whatever the floor
    for (uint32_t players = 1; players <= 40; ++players)
        for (uint32_t floor : {0u, 120u, 300u, 600u, 900u})
        {
            int64_t const core = CoreTime(now, 600, players, 60);
            int64_t const got = Floored(now, core, floor, 60);
            check(got >= core, "never sooner than the core's time");
            check(!floor || got >= now + floor + 60, "never sooner than the floor after the corpse");
        }

    std::cout << "respawnfloor_tests: " << (g_checks - g_failed) << "/" << g_checks << " checks passed\n";
    return g_failed ? 1 : 0;
}
