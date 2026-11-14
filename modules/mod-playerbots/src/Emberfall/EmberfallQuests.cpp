/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version. Emberfall's part of the module (Copyright Emberfall / Tales of Orrin).
 */

/*
 * Emberfall's part of mod-playerbots (bot step 2): see EmberfallQuests.h. Released under GNU GPL v2 or later, as the
 * module.
 */

#include "EmberfallQuests.h"

#include "Config.h"
#include "DatabaseEnv.h"
#include "EmberfallBots.h"
#include "GameObject.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "QuestPackets.h"
#include "WorldSession.h"

#include <ctime>
#include <list>
#include <vector>

namespace
{
    std::unordered_map<uint32, std::vector<uint32>> s_questTriggers;  // quest -> its area triggers
    uint32 s_giveUpMinutes = 45;
    constexpr float kObjectSearch = 80.0f;   // yd: a quest object the bot walks to
    constexpr float kTriggerSearch = 250.0f; // yd: an area trigger the bot walks to

    bool HasGoObjective(Quest const* quest)
    {
        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            if (quest->RequiredNpcOrGo[i] < 0)
                return true;
        return false;
    }

    // An incomplete quest this helper has work for (an object to use, a trigger to reach).
    bool HasWork(Player* bot, uint32 questId, Quest const* quest)
    {
        if (bot->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
            return false;
        return s_questTriggers.count(questId) || HasGoObjective(quest);
    }

    void DropQuest(Player* bot, uint16 slot, uint32 questId, char const* why)
    {
        LOG_DEBUG("playerbots", "Emberfall quests: {} drops quest {} ({})", bot->GetName(), questId, why);
        WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
        packet << uint8(slot);
        WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
        removeQuest.Read();
        bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
    }
}

namespace EmberfallBots
{
    void LoadQuests()
    {
        s_questTriggers.clear();
        s_giveUpMinutes = sConfigMgr->GetOption<uint32>("Playerbots.Emberfall.QuestGiveUpMinutes", 45);
        uint32 count = 0;
        if (QueryResult r = WorldDatabase.Query("SELECT id, quest FROM areatrigger_involvedrelation"))
        {
            do
            {
                Field* f = r->Fetch();
                uint32 const trigger = f[0].Get<uint32>();
                if (!sObjectMgr->GetAreaTrigger(trigger))
                    continue;
                s_questTriggers[f[1].Get<uint32>()].push_back(trigger);
                ++count;
            } while (r->NextRow());
        }
        LOG_INFO("server.loading", ">> Emberfall bots: {} quest area triggers of {} quests; a quest held {} min unfinished is dropped",
                 count, s_questTriggers.size(), s_giveUpMinutes);
    }

    bool QuestDoable(Quest const* quest)
    {
        if (!quest || !Enabled())
            return true;
        // a timed quest (escorts, races) or an event quest without an area trigger (an escort's end, a script's event)
        if (quest->GetTimeAllowed())
            return false;
        if (quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT) && !s_questTriggers.count(quest->GetQuestId()))
            return false;
        return true;
    }
}

bool EmberfallQuestTrigger::IsActive()
{
    // (in a player's group the bot follows its leader: no quest walks of its own)
    if (!EmberfallBots::Enabled() || !bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() || bot->GetGroup())
        return false;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
            continue;
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;
        if (HasWork(bot, questId, quest) || !EmberfallBots::QuestDoable(quest))
            return true;
    }
    // once a minute the action looks at the give-up clock of whatever the bot holds
    uint32 const now = static_cast<uint32>(std::time(nullptr));
    if (now - lastGiveUpCheck < 60)
        return false;
    lastGiveUpCheck = now;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        if (bot->GetQuestSlotQuestId(slot))
            return true;
    return false;
}

bool EmberfallQuestAction::Execute(Event /*event*/)
{
    uint32 const now = static_cast<uint32>(std::time(nullptr));
    std::unordered_map<uint32, uint32> held;
    WorldObject* goal = nullptr;
    float goalX = 0, goalY = 0, goalZ = 0, goalDist = 0;

    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
            continue;
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;
        auto since = heldSince.find(questId);
        held[questId] = since != heldSince.end() ? since->second : now;

        QuestStatus const status = bot->GetQuestStatus(questId);
        if (status == QUEST_STATUS_COMPLETE)
            continue;
        // a quest no bot can do (escort, event, timed), or one held too long without finishing: dropped
        if (!EmberfallBots::QuestDoable(quest))
        {
            DropQuest(bot, slot, questId, "a bot cannot do it");
            heldSince.erase(questId);
            return true;
        }
        if (status == QUEST_STATUS_INCOMPLETE && now - held[questId] > s_giveUpMinutes * 60)
        {
            DropQuest(bot, slot, questId, "held too long");
            heldSince.erase(questId);
            return true;
        }
        if (status != QUEST_STATUS_INCOMPLETE)
            continue;

        // explore: the server checks what a client would report
        if (auto it = s_questTriggers.find(questId); it != s_questTriggers.end())
        {
            for (uint32 const triggerId : it->second)
            {
                AreaTrigger const* at = sObjectMgr->GetAreaTrigger(triggerId);
                if (!at || at->map != bot->GetMapId())
                    continue;
                if (bot->IsInAreaTriggerRadius(at))
                {
                    LOG_DEBUG("playerbots", "Emberfall quests: {} explores area trigger {} for quest {}", bot->GetName(),
                              triggerId, questId);
                    bot->AreaExploredOrEventHappens(questId);
                    heldSince = std::move(held);
                    return true;
                }
                float const d = bot->GetDistance(at->x, at->y, at->z);
                if (d < kTriggerSearch && (!goalDist || d < goalDist))
                {
                    goal = nullptr;
                    goalX = at->x, goalY = at->y, goalZ = at->z, goalDist = d;
                }
            }
        }

        // use an object: the nearest one this bot can see (the quest objects' visibility conditions hold for bots too)
        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        {
            int32 const npcOrGo = quest->RequiredNpcOrGo[i];
            if (npcOrGo >= 0 || !quest->RequiredNpcOrGoCount[i])
                continue;
            if (bot->GetQuestSlotCounter(slot, i) >= quest->RequiredNpcOrGoCount[i])
                continue;
            std::list<GameObject*> objects;
            bot->GetGameObjectListWithEntryInGrid(objects, uint32(-npcOrGo), kObjectSearch);
            GameObject* nearest = nullptr;
            for (GameObject* go : objects)
            {
                if (!go->isSpawned() || go->GetGoState() != GO_STATE_READY || !bot->CanSeeOrDetect(go))
                    continue;
                if (!nearest || bot->GetDistance(go) < bot->GetDistance(nearest))
                    nearest = go;
            }
            if (!nearest)
                continue;
            if (bot->IsWithinDistInMap(nearest, INTERACTION_DISTANCE))
            {
                LOG_DEBUG("playerbots", "Emberfall quests: {} uses object {} for quest {}", bot->GetName(), nearest->GetEntry(),
                          questId);
                bot->StopMoving();
                WorldPacket packet(CMSG_GAMEOBJ_USE, 8);
                packet << nearest->GetGUID();
                bot->GetSession()->HandleGameObjectUseOpcode(packet);
                heldSince = std::move(held);
                return true;
            }
            float const d = bot->GetDistance(nearest);
            if (!goalDist || d < goalDist)
            {
                goal = nearest;
                goalX = nearest->GetPositionX(), goalY = nearest->GetPositionY(), goalZ = nearest->GetPositionZ();
                goalDist = d;
            }
        }
    }
    heldSince = std::move(held);

    if (!goalDist)
        return false;
    if (goal)
        return MoveNear(goal, INTERACTION_DISTANCE - 1.0f);
    return MoveTo(bot->GetMapId(), goalX, goalY, goalZ);
}
