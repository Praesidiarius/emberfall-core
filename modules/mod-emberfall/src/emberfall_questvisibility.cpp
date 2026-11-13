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

// Emberfall's quest objects only for the players who need them (Emberfall feedback #511, no core patch): a quest's
// pick-ups (the Stonereach's kindling bundles, a stolen eel trap) carry OBJECT_VISIBILITY conditions (the conditions
// table, SourceType 30, written by Emberfall's world compiler: tools/py/emberfall_content/quest_objects.py): the quest in
// progress and fewer of its item than the quest collects. The core asks them whenever it decides whether a player sees
// an object (WorldObject::CanSeeOrDetect -> Player::CanSeeObjectByVisibilityConditions), but it decides that only when
// the player moves far enough (the relocation notifier's distance) or the object changes: a player standing at the
// quest giver who takes the quest would see nothing appear until they walked, and one who loots the last ice block
// would keep seeing the others.
//
// So this script brings the player's view of such objects up to date when their quest state changes: taking, abandoning
// or turning in a quest, looting or receiving an item, and using a game object (a goober's credit). The update runs on
// the player's next tick (their events), once however many hooks fired, and asks Player::UpdateVisibilityOf for every
// game object with visibility conditions within the player's sight range (creates the ones that now pass, destroys the
// ones that no longer do). Objects without conditions are not touched.

#include "CellImpl.h"
#include "ConditionMgr.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Item.h"
#include "Log.h"
#include "Player.h"
#include "PlayerScript.h"
#include "AllGameObjectScript.h"
#include "ScriptMgr.h"

#include <list>

namespace
{
    // The player's pending refresh (one per tick, however many hooks asked).
    struct PendingRefresh : DataMap::Base
    {
    };
    constexpr char const* kPending = "ef_quest_visibility";

    void Refresh(Player* player)
    {
        player->CustomData.Erase(kPending);
        if (!player->IsInWorld() || player->IsDuringRemoveFromWorld())
            return;
        float const range = player->GetSightRange();
        std::list<GameObject*> objects;
        Acore::GameObjectInRangeCheck check(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), range);
        Acore::GameObjectListSearcher<Acore::GameObjectInRangeCheck> searcher(player, objects, check);
        Cell::VisitObjects(player, searcher, range);
        uint32 updated = 0;
        for (GameObject* go : objects)
        {
            if (sConditionMgr->GetConditionsForObjectVisibility(go).empty())
                continue;
            player->UpdateVisibilityOf(go);
            ++updated;
        }
        LOG_DEBUG("scripts", "Emberfall quest visibility: {} - {} conditioned objects of {} in {:.0f} yd updated", player->GetName(),
                  updated, objects.size(), range);
    }

    // Asks for a refresh on the player's next tick (after the hook's change has landed: AddQuest before
    // OnPlayerQuestAccept's caller completes the quest, the item counted after OnPlayerLootItem ...).
    void Schedule(Player* player)
    {
        if (!player || !player->IsInWorld() || player->CustomData.Get<PendingRefresh>(kPending))
            return;
        player->CustomData.Set(kPending, new PendingRefresh());
        player->m_Events.AddEventAtOffset([player]() { Refresh(player); }, 0ms);
    }

    class emberfall_quest_visibility_player : public PlayerScript
    {
    public:
        emberfall_quest_visibility_player()
            : PlayerScript("emberfall_quest_visibility_player", { PLAYERHOOK_ON_PLAYER_QUEST_ACCEPT, PLAYERHOOK_ON_QUEST_ABANDON,
                                                                  PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST, PLAYERHOOK_ON_LOOT_ITEM,
                                                                  PLAYERHOOK_ON_STORE_NEW_ITEM }) { }

        void OnPlayerQuestAccept(Player* player, Quest const* /*quest*/) override { Schedule(player); }
        void OnPlayerQuestAbandon(Player* player, uint32 /*questId*/) override { Schedule(player); }
        // (the reward: Player::RewardQuest)
        void OnPlayerCompleteQuest(Player* player, Quest const* /*quest*/) override { Schedule(player); }
        void OnPlayerLootItem(Player* player, Item* /*item*/, uint32 /*count*/, ObjectGuid /*lootguid*/) override { Schedule(player); }
        void OnPlayerStoreNewItem(Player* player, Item* /*item*/, uint32 /*count*/) override { Schedule(player); }
    };

    // A game object used by a player (a goober's credit: GameObject::Use sets GO_ACTIVATED with the user; a chest's
    // opening too): that player's quest objects may have changed.
    class emberfall_quest_visibility_objects : public AllGameObjectScript
    {
    public:
        emberfall_quest_visibility_objects() : AllGameObjectScript("emberfall_quest_visibility_objects") { }

        void OnGameObjectLootStateChanged(GameObject* /*go*/, uint32 state, Unit* unit) override
        {
            if (state == GO_ACTIVATED && unit)
                Schedule(unit->ToPlayer());
        }
    };
}

void AddEmberfallQuestVisibilityScripts()
{
    new emberfall_quest_visibility_player();
    new emberfall_quest_visibility_objects();
}
