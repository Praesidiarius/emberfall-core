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

// Emberfall's escorts are their taker's (feedback #564, #567; Emberfall docs/contributing/quests.md "Escorts"). Before,
// the escortee's one spawn walked off for whoever took the quest: with bots and other players about, a player found
// them gone from their place, walking with somebody else, and could "rescue" a walk they never took (the escortee went
// on for any player near). Now the spawn never leaves its place:
//   - taking the quest from it, or speaking to it with the walk still to do, summons a copy of it for the player (the
//     player is the copy's summoner): the copy's SmartAI (just_summoned) walks the path (Emberfall's compiler,
//     tools/py/emberfall_content/quests/escorts.py); the copy carries no quest or gossip flag (no marker over it);
//   - who sees the copy, the spawn and the copy's ambushers: escorts/EfEscortLogic.h, through the core's per-viewer
//     creature filter (EmberfallVisibility.h, shared with the world editor: visibility/EfCreatureVisibility.h). The
//     copy counts only the players who see it for its "anyone near?" checks (the core's SmartAI near_players, PATCHES.md
//     "near players are those who see it");
//   - the views change when a copy comes or goes and when a player's quest state changes: the quest visibility refresh
//     (emberfall_questvisibility.cpp) asks again for the creatures this file concerns.
// Which quests are escorts: the world table ef_escort (quest, npc), written by Emberfall's compiler.

#include "AllCreatureScript.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "EmberfallOwnData.h"
#include "GossipDef.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerScript.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "TemporarySummon.h"
#include "WorldScript.h"
#include "escorts/EfEscortLogic.h"
#include "visibility/EfCreatureVisibility.h"

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

void EmberfallQuestVisibilitySchedule(Player* player);

namespace
{
    using namespace Emberfall::Escort;

    // escortee entry -> its escort quest (read once at startup, read-only afterwards)
    std::unordered_map<uint32, uint32> s_questOf;
    // the escortees who also take their quest back (creature_questender): handed it where the walk ends
    std::unordered_set<uint32> s_takesBack;

    struct Copy
    {
        uint32 entry = 0;
        ObjectGuid summoner;
    };
    // the copies out, by their GUID (map threads add and remove them; the filter reads them)
    std::mutex s_lock;
    std::unordered_map<ObjectGuid, Copy> s_copies;
    std::atomic<uint32> s_out{0};

    QuestState StateOf(Player const* player, uint32 quest)
    {
        if (player->GetQuestRewardStatus(quest))
            return QuestState::Rewarded;
        switch (player->GetQuestStatus(quest))
        {
            case QUEST_STATUS_INCOMPLETE: return QuestState::ToDo;
            case QUEST_STATUS_COMPLETE: return QuestState::Done;
            default: return QuestState::None;
        }
    }

    Relation RelationTo(Player const* viewer, ObjectGuid summoner)
    {
        if (viewer->GetGUID() == summoner)
            return Relation::Summoner;
        Group const* group = viewer->GetGroup();
        return group && group->IsMember(summoner) ? Relation::Group : Relation::Stranger;
    }

    // A copy of an escortee walking for the viewer or the viewer's group.
    bool OwnCopyOut(Player const* viewer, uint32 entry)
    {
        if (!s_out.load())
            return false;
        std::lock_guard<std::mutex> guard(s_lock);
        for (auto const& [guid, copy] : s_copies)
            if (copy.entry == entry && RelationTo(viewer, copy.summoner) != Relation::Stranger)
                return true;
        return false;
    }

    // A player-summoned escortee: an escort's copy.
    bool IsCopy(Creature const* creature)
    {
        TempSummon const* summon = creature->ToTempSummon();
        return summon && summon->GetSummonerGUID().IsPlayer() && s_questOf.count(creature->GetEntry());
    }

    // The core's per-viewer filter: true hides the creature from the viewer.
    bool Hide(Creature const* creature, Player const* viewer)
    {
        if (s_questOf.empty() || viewer->IsGameMaster())
            return false;
        if (TempSummon const* summon = creature->ToTempSummon())
        {
            ObjectGuid summoner = summon->GetSummonerGUID();
            uint32 entry = creature->GetEntry();
            if (!summoner.IsPlayer())
            {
                // an ambusher of a walk: seen as its copy is
                if (!s_out.load() || !summoner.IsCreature())
                    return false;
                std::lock_guard<std::mutex> guard(s_lock);
                auto copy = s_copies.find(summoner);
                if (copy == s_copies.end())
                    return false;
                summoner = copy->second.summoner;
                entry = copy->second.entry;
            }
            auto quest = s_questOf.find(entry);
            if (quest == s_questOf.end())
                return false;
            return !SeesCopy(RelationTo(viewer, summoner), StateOf(viewer, quest->second), s_takesBack.count(entry) != 0);
        }
        auto quest = s_questOf.find(creature->GetEntry());
        if (quest == s_questOf.end())
            return false;
        QuestState const state = StateOf(viewer, quest->second);
        return state == QuestState::ToDo && HidesHome(OwnCopyOut(viewer, creature->GetEntry()), state);
    }

    // The players whose view of a copy (and its spawn at home) changes with it: its summoner and their group, on its map.
    void ScheduleFor(Creature const* copy, ObjectGuid summoner)
    {
        Player* player = ObjectAccessor::FindPlayer(summoner);
        if (!player || player->GetMap() != copy->GetMap())
            return;
        EmberfallQuestVisibilitySchedule(player);
        if (Group* group = player->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource(); member && member != player && member->GetMap() == copy->GetMap())
                    EmberfallQuestVisibilitySchedule(member);
    }

    // Sets a walk off for the player from the escortee's spawn: the copy (its SmartAI does the rest). False when the
    // player may not start one (the walk not to do, a copy of theirs or their group's already out).
    bool Start(Player* player, Creature* home, uint32 quest)
    {
        if (!MayStart(OwnCopyOut(player, home->GetEntry()), StateOf(player, quest)))
            return false;
        TempSummon* copy = player->SummonCreature(home->GetEntry(), home->GetPosition(), TEMPSUMMON_TIMED_OR_CORPSE_DESPAWN,
                                                  kCopyLifetimeMs);
        if (!copy)
        {
            LOG_ERROR("module", "Emberfall escorts: {} could not summon the walk of {} (quest {})", player->GetName(),
                      home->GetEntry(), quest);
            return false;
        }
        LOG_DEBUG("module", "Emberfall escorts: {} sets off {} (quest {})", player->GetName(), home->GetEntry(), quest);
        return true;
    }

    class emberfall_escorts_world : public WorldScript
    {
    public:
        emberfall_escorts_world() : WorldScript("emberfall_escorts_world", { WORLDHOOK_ON_STARTUP }) { }

        void OnStartup() override
        {
            s_questOf.clear();
            s_takesBack.clear();
            Emberfall::ModuleVisibility::SetEscorts(nullptr);
            if (!Emberfall::OwnData())
                return;
            if (QueryResult exists = WorldDatabase.Query("SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = "
                                                         "DATABASE() AND TABLE_NAME = 'ef_escort'"))
                if (exists->Fetch()[0].Get<uint64>() == 0)
                {
                    LOG_ERROR("server.loading", ">> Emberfall escorts: the world table ef_escort is missing "
                                                "(data/sql/db-world): escorts walk for nobody");
                    return;
                }
            if (QueryResult r = WorldDatabase.Query("SELECT `quest`, `npc` FROM `ef_escort`"))
                do
                {
                    Field* f = r->Fetch();
                    uint32 const quest = f[0].Get<uint32>(), npc = f[1].Get<uint32>();
                    s_questOf[npc] = quest;
                    QuestRelationBounds enders = sObjectMgr->GetCreatureQuestInvolvedRelationBounds(npc);
                    for (auto it = enders.first; it != enders.second; ++it)
                        if (it->second == quest)
                            s_takesBack.insert(npc);
                } while (r->NextRow());
            Emberfall::ModuleVisibility::SetEscorts(s_questOf.empty() ? nullptr : &Hide);
            LOG_INFO("server.loading", ">> Emberfall escorts: {} escorts, each walked by a copy of its escortee for its taker",
                     s_questOf.size());
        }
    };

    class emberfall_escorts_creatures : public AllCreatureScript
    {
    public:
        emberfall_escorts_creatures() : AllCreatureScript("emberfall_escorts_creatures") { }

        void OnCreatureAddWorld(Creature* creature) override
        {
            if (!IsCopy(creature))
                return;
            // no marker over the walk and nobody sets it off again (the spawn at home does that)
            creature->RemoveNpcFlag(NPCFlags(UNIT_NPC_FLAG_QUESTGIVER | UNIT_NPC_FLAG_GOSSIP));
            ObjectGuid const summoner = creature->ToTempSummon()->GetSummonerGUID();
            {
                std::lock_guard<std::mutex> guard(s_lock);
                s_copies[creature->GetGUID()] = Copy{creature->GetEntry(), summoner};
                s_out.store(uint32(s_copies.size()));
            }
            ScheduleFor(creature, summoner);
        }

        void OnCreatureRemoveWorld(Creature* creature) override
        {
            if (!s_out.load() || !IsCopy(creature))
                return;
            ObjectGuid summoner;
            {
                std::lock_guard<std::mutex> guard(s_lock);
                auto it = s_copies.find(creature->GetGUID());
                if (it == s_copies.end())
                    return;
                summoner = it->second.summoner;
                s_copies.erase(it);
                s_out.store(uint32(s_copies.size()));
            }
            ScheduleFor(creature, summoner);
        }

        // (OnQuestAccept: Player::AddQuestAndCheckCompletion, after the quest is in the log)
        bool CanCreatureQuestAccept(Player* player, Creature* creature, Quest const* quest) override
        {
            auto it = s_questOf.find(creature->GetEntry());
            if (it != s_questOf.end() && it->second == quest->GetQuestId() && !creature->IsSummon())
                Start(player, creature, quest->GetQuestId());
            return false;
        }

        // Spoken to with the walk still to do and none out: they set off again (after a death or a give-up).
        bool CanCreatureGossipHello(Player* player, Creature* creature) override
        {
            auto it = s_questOf.find(creature->GetEntry());
            if (it == s_questOf.end() || creature->IsSummon() || !Start(player, creature, it->second))
                return false;
            CloseGossipMenuFor(player);
            return true;
        }
    };

    // A player's quest state changed: their copies and spawns may show or hide (the refresh asks the filter again).
    class emberfall_escorts_player : public PlayerScript
    {
    public:
        emberfall_escorts_player()
            : PlayerScript("emberfall_escorts_player", { PLAYERHOOK_ON_BEFORE_QUEST_COMPLETE }) { }

        // (Player::CompleteQuest: the walk's credit made the quest complete; the quest visibility script covers taking,
        // abandoning and handing in)
        bool OnPlayerBeforeQuestComplete(Player* player, uint32 /*questId*/) override
        {
            EmberfallQuestVisibilitySchedule(player);
            return true;
        }
    };
}

// The quest visibility refresh asks this for each creature in range: does the escort rule decide whether it is seen?
bool EmberfallEscortConcerns(Creature const* creature)
{
    if (s_questOf.empty())
        return false;
    if (s_questOf.count(creature->GetEntry()))
        return true;
    TempSummon const* summon = creature->ToTempSummon();
    if (!summon || !s_out.load() || !summon->GetSummonerGUID().IsCreature())
        return false;
    std::lock_guard<std::mutex> guard(s_lock);
    return s_copies.count(summon->GetSummonerGUID()) != 0;
}

void AddEmberfallEscortScripts()
{
    new emberfall_escorts_world();
    new emberfall_escorts_creatures();
    new emberfall_escorts_player();
}
