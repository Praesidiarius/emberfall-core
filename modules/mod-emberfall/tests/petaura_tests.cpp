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

// The core's companion aura rule (src/server/game/Spells/Auras/EmberfallPetAura.h, PATCHES.md #43; Emberfall
// docs/design/classes/hunter-marksman.md §40): a class kit's APPLY_AREA_AURA_PET cast by a player reaches its companion.

#include "EmberfallPetAura.h"

#include <iostream>
#include <string>

namespace
{
    int g_checks = 0;
    int g_failed = 0;

    void check(bool ok, std::string const& what)
    {
        ++g_checks;
        if (!ok)
        {
            ++g_failed;
            std::cerr << "FAIL: " << what << "\n";
        }
    }
}

int main()
{
    using namespace Emberfall::PetAura;
    check(ToCompanion(true, 103, true), "a hunter's kit aura on our data: to the companion");
    check(!ToCompanion(false, 103, true), "without own data: upstream (the owner and its owner)");
    check(!ToCompanion(true, 9, true), "a stock family (a pet's own stock aura): upstream");
    check(!ToCompanion(true, 103, false), "a pet or creature as the owner: upstream (the pet and its master)");
    check(CompanionInReach(true, true, 30.0f, 100.0f), "a living companion within the radius takes it");
    check(!CompanionInReach(true, true, 120.0f, 100.0f), "beyond the radius: not");
    check(!CompanionInReach(true, false, 5.0f, 100.0f), "a fallen companion: not");
    check(!CompanionInReach(false, false, 0.0f, 100.0f), "no companion: nobody (not the player either)");
    std::cout << "pet aura tests: " << (g_checks - g_failed) << "/" << g_checks << " passed\n";
    return g_failed ? 1 : 0;
}
