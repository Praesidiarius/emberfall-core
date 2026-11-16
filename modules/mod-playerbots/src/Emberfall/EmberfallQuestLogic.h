/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version. Emberfall's part of the module (Copyright Emberfall / Tales of Orrin).
 */

/*
 * Which quests a bot takes and keeps (Emberfall docs/design/playerbots-pilot.md §10.1, §18), as pure logic with no core
 * types, so mod-emberfall's standalone tests (tests/botquest_tests.cpp) run it without a server. EmberfallBots::QuestDoable
 * feeds it the quest's facts.
 */

#ifndef PLAYERBOTS_EMBERFALLQUESTLOGIC_H
#define PLAYERBOTS_EMBERFALLQUESTLOGIC_H

namespace EmberfallQuest::Logic
{
    struct QuestFacts
    {
        bool timed = false;        // a time limit (races)
        bool event = false;        // QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT
        bool hasTrigger = false;   // an area trigger of areatrigger_involvedrelation completes it
        bool escort = false;       // in the world table ef_escort: the taker gets a private copy of the escortee, which
                                   // nobody else sees (mod-emberfall escorts) and no bot can walk
    };

    // A quest a bot can finish: not timed, not an escort, and an event quest only when an area trigger completes it.
    inline bool Doable(QuestFacts const& q)
    {
        if (q.timed || q.escort)
            return false;
        if (q.event && !q.hasTrigger)
            return false;
        return true;
    }
}

#endif
