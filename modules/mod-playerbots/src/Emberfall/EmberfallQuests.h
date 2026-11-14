/*
 * Emberfall's part of mod-playerbots (bot step 2, Emberfall docs/design/playerbots-pilot.md §10): the quest kinds the
 * stock bot never finished on Emberfall's content.
 *   - use an object (a quest's goober: RequiredNpcOrGo < 0): the bot walks to the nearest one it can see (the same
 *     visibility conditions as a player's) and uses it as a client would (CMSG_GAMEOBJ_USE);
 *   - explore (an area trigger of areatrigger_involvedrelation): a client reports walking in, a bot has no client, so
 *     the server checks the bot's position against the trigger and completes the event; the bot walks to the trigger;
 *   - escort and event quests (the exploration-or-event flag without an area trigger, or a timer): bots never take them;
 *   - a quest held longer than Playerbots.Emberfall.QuestGiveUpMinutes without finishing is dropped.
 * Released under GNU GPL v2 or later, as the module.
 */

#ifndef PLAYERBOTS_EMBERFALLQUESTS_H
#define PLAYERBOTS_EMBERFALLQUESTS_H

#include "MovementActions.h"
#include "NamedObjectContext.h"
#include "Trigger.h"

#include <unordered_map>

class Quest;

namespace EmberfallBots
{
    // Reads the quests' area triggers (at the module's start, after the world's area triggers).
    void LoadQuests();
    // False for a quest a bot cannot do (escort, event, timed): never taken, dropped if held.
    bool QuestDoable(Quest const* quest);
}

class EmberfallQuestTrigger : public Trigger
{
public:
    EmberfallQuestTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ef quest", 2) {}
    bool IsActive() override;

private:
    uint32 lastGiveUpCheck = 0;  // s: the give-up clock is looked at once a minute
};

class EmberfallQuestAction : public MovementAction
{
public:
    EmberfallQuestAction(PlayerbotAI* botAI) : MovementAction(botAI, "ef quest") {}
    bool Execute(Event event) override;

private:
    std::unordered_map<uint32, uint32> heldSince;  // quest -> when this bot first held it (s)
};

#endif
