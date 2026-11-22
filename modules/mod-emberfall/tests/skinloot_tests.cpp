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

// Standalone tests of the core's skin loot rule (src/server/game/Loot/EmberfallSkinLoot.h, PATCHES.md #45; Emberfall
// alpha1 feedback #709; tests/run_tests.sh builds and runs them): Player::SendLoot sends a skin with the corpse's killer
// offline (and not in a group found online); the kill's own loot still asks for the killer or their group, as upstream;
// a skin's loot is sent, taken and released within the skinning reach, every other loot within the interaction distance.

#include "EmberfallSkinLoot.h"

#include <iostream>
#include <string>

using namespace Emberfall;

namespace
{
    int g_failed = 0, g_checks = 0;

    void check(bool ok, std::string const& what)
    {
        ++g_checks;
        if (!ok)
        {
            ++g_failed;
            std::cerr << "FAILED: " << what << "\n";
        }
    }

    void testRecipient()
    {
        check(!SkinLoot::NeedsRecipientOnline(true), "a skin: sent whether the killer is online or not (#709)");
        check(SkinLoot::NeedsRecipientOnline(false), "the kill's own loot: only with the killer or their group online");
        static_assert(!SkinLoot::NeedsRecipientOnline(true), "the rule is constexpr");
    }

    void testReach()
    {
        constexpr float kInteraction = 5.5f; // the core's INTERACTION_DISTANCE
        check(SkinLoot::LootReach(false, kInteraction) == kInteraction, "the kill's own loot: the interaction distance, as upstream");
        check(SkinLoot::LootReach(true, kInteraction) == SkinLoot::kReach, "a skin: the skinning reach");
        check(SkinLoot::kReach >= 8.0f + 4.0f, "the skin's reach covers the spells' 8 yd and Long Reach's 4");
        check(SkinLoot::LootReach(true, 30.0f) == 30.0f, "never shorter than the distance the caller allows");
    }
}

int main()
{
    testRecipient();
    testReach();
    std::cout << "skin loot tests: " << (g_checks - g_failed) << "/" << g_checks << " passed\n";
    return g_failed == 0 ? 0 : 1;
}
