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

// The core's companion taunt rule (src/server/game/AI/CoreAI/EmberfallPetTaunt.h, PATCHES.md #44; Emberfall
// docs/design/classes/hunter-marksman.md §41): a hunter's companion autocasts its taunt only at an enemy that attacks
// someone else.

#include "EmberfallPetTaunt.h"

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
    using namespace Emberfall::PetTaunt;
    // (ownData, hunterPet, isTaunt, enemyHasVictim, enemyAttacksPet)
    check(AutocastWaits(true, true, true, true, true), "the enemy already attacks the companion: the taunt waits");
    check(!AutocastWaits(true, true, true, true, false), "the enemy attacks the hunter or a group member: the taunt goes");
    check(AutocastWaits(true, true, true, false, false), "the enemy attacks nobody yet (the pull): the taunt waits");
    check(!AutocastWaits(true, true, false, true, true), "the basic attack and every other spell: as upstream");
    check(!AutocastWaits(false, true, true, true, true), "without own data: upstream");
    check(!AutocastWaits(true, false, true, true, true), "a summoned or charmed pet: upstream");
    std::cout << "pet taunt tests: " << (g_checks - g_failed) << "/" << g_checks << " passed\n";
    return g_failed ? 1 : 0;
}
