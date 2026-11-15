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

// Standalone tests of the escort rule (src/escorts/EfEscortLogic.h; tests/run_tests.sh builds and runs them): who sees
// a walking copy, when the spawn at home hides, and who may set a walk off (feedback #564, #567).

#include "EfEscortLogic.h"

#include <iostream>
#include <string>

using namespace Emberfall::Escort;

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
    // the copy: its own players, while they hold the quest
    check(SeesCopy(Relation::Summoner, QuestState::ToDo), "the taker sees their walk");
    check(SeesCopy(Relation::Group, QuestState::ToDo), "a group member on the quest sees it (they may join)");
    check(SeesCopy(Relation::Summoner, QuestState::Done), "the taker still sees it at the end (done, not handed in)");
    check(!SeesCopy(Relation::Stranger, QuestState::ToDo), "#564: a stranger on the same quest sees no one else's walk");
    check(!SeesCopy(Relation::Stranger, QuestState::None), "#564: a player without the quest sees no walk at all");
    check(!SeesCopy(Relation::Group, QuestState::None), "a group member without the quest is on no escort");
    check(!SeesCopy(Relation::Summoner, QuestState::None), "the taker who abandoned the quest no longer sees it");
    check(SeesCopy(Relation::Summoner, QuestState::Rewarded, true), "handed in to the copy at the end: it does not vanish at once");
    check(!SeesCopy(Relation::Summoner, QuestState::Rewarded, false), "handed in elsewhere: the walk goes (the next place is a `seen` spawn)");
    check(!SeesCopy(Relation::Stranger, QuestState::Rewarded), "a stranger who did the escort long ago sees no one else's");

    // the spawn at home: hidden only from the players whose walk is out and still to do
    check(HidesHome(true, QuestState::ToDo), "#567: the taker sees no second escortee (and no marker) at the start");
    check(!HidesHome(false, QuestState::ToDo), "no walk out (it died or gave up): the spawn is there to set off again");
    check(!HidesHome(true, QuestState::Done), "the walk done: the spawn is seen again (a quest-phased `seen` may hide it)");
    check(!HidesHome(false, QuestState::None), "#564: everyone else sees the escortee waiting at home");
    check(!HidesHome(true, QuestState::None), "a group member without the quest still sees the spawn (and may take it)");

    // setting off: one walk per group, only with the walk to do
    check(MayStart(false, QuestState::ToDo), "taking the quest sets a walk off");
    check(!MayStart(true, QuestState::ToDo), "a second walk for the same group: no");
    check(!MayStart(false, QuestState::Done), "the walk done: speaking to them sets nothing off");
    check(!MayStart(false, QuestState::None), "nobody is on an escort without the quest");
    check(!MayStart(false, QuestState::Rewarded), "nor after handing it in");

    check(kCopyLifetimeMs >= 30u * 60u * 1000u, "the copy's net outlasts the longest walk (about 10 minutes)");

    std::cout << "escort tests: " << (g_checks - g_failed) << "/" << g_checks << " passed\n";
    return g_failed ? 1 : 0;
}
