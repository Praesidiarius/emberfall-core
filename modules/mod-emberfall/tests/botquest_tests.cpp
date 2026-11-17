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

// Standalone tests of which quests a bot takes (mod-playerbots' src/Emberfall/EmberfallQuestLogic.h; tests/run_tests.sh
// builds and runs them): an escort (the taker's private escortee, ef_escort) never, a timed quest never, an event quest
// only with an area trigger.

#include "EmberfallQuestLogic.h"

#include <iostream>
#include <string>

using namespace EmberfallQuest::Logic;

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
}

int main()
{
    QuestFacts q;
    check(Doable(q), "a plain kill or collect quest is doable");
    q.escort = true;
    check(!Doable(q), "an escort (ef_escort) is never taken: the taker's copy of the escortee is invisible to all others");
    q = QuestFacts();
    q.escort = true;
    q.event = true;
    q.hasTrigger = true;
    check(!Doable(q), "an escort is refused even when an area trigger would complete its event");
    q = QuestFacts();
    q.timed = true;
    check(!Doable(q), "a timed quest is not taken");
    q = QuestFacts();
    q.event = true;
    check(!Doable(q), "an event quest without an area trigger is not taken");
    q.hasTrigger = true;
    check(Doable(q), "an explore quest (an area trigger completes it) is taken");

    std::cout << "botquest: " << (g_checks - g_failed) << "/" << g_checks << " checks passed\n";
    return g_failed ? 1 : 0;
}
