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

// Emberfall's personal quest objects (no core patch; the pure parts in src/questloot/EfQuestLootLogic.h): a quest item
// lying in the world (a chest whose loot is only quest items: a reed stook, an eel trap, oakwort) is taken by each player
// who needs it from the same object, and taking it makes the object disappear for that player only, until their own
// respawn; the group never takes each other's items.
//
// How the stock core handles such a chest: one Loot per object, filled at the first opening (Player::SendLoot, GO_READY ->
// FillLoot for the opener, personal: the quest items' per-player list only for them), then GO_ACTIVATED; a second player
// opening while it is activated gets no list of their own; at the release of an empty loot the object goes
// GO_JUST_DEACTIVATED and GameObject::Update despawns it for everyone until spawntimesecs (WorldSession::DoLootRelease).
// So one player's loot took the object from the whole group.
//
// Here (gameobject_template.ScriptName, set by Emberfall's compiler on those chests; their items carry ITEM_FLAG_MULTI_DROP,
// also set by the compiler, so a quest item in a shared loot is one copy per player: LootItem::freeforall):
// - OnPlayerBeforeSendLoot: a player opening the object while another's loot is open gets their own quest item list
//   (Loot::FillNotNormalLootFor; the core fills it only for the player who generated the loot).
// - The AI's OnStateChanged: the release of the emptied loot (GO_JUST_DEACTIVATED with no respawn time set) puts the
//   object back to GO_READY instead of despawning it; DoLootRelease then clears the loot and the next opening fills it
//   anew for that player. A real despawn (DespawnOrUnsummon, which sets the respawn time first) still despawns.
// - OnPlayerLootItem: the player who took an item from the object is locked out of it for the spawn's respawn time
//   (EfQuestLootLogic.h Lockouts). The AI refuses their opening (GossipHello, which Spell::SendLoot and GameObject::Use
//   ask first) and, in Hide mode (emberfall_personal_quest_object), CanBeSeen says no: once their loot window is closed
//   the object is destroyed for their client (Player::UpdateVisibilityOf) and comes back when the lockout ends. In Stay
//   mode (emberfall_personal_quest_object_stays, gameobjects.yaml `consumable: 0`) it stays in sight.
//
// Everyone else keeps seeing it, with the sparkle while their quest wants the item (GO_DYNFLAG_LO_ACTIVATE /
// LO_SPARKLE per viewer, GameObject::ActivateToQuest; the client lets a quest chest be opened only with it).
//
// Personal quest goobers (emberfall_personal_quest_goober; the compiler names it on every goober whose quest objective
// counts more than one: the lifters' fold's three gates, the deserters' four cook-fires). The stock goober credits
// every use (GameObject::Use: KillCreditGO for the user, or every group member in reward distance, while the quest is
// incomplete) and stays usable, so one gate clicked three times made 3/3. Here the AI's GossipHello, which Use asks
// first, takes the use over: it credits the user and their group members in reward distance who are not locked out of
// this spawn, plays the open animation (UseDoorOrButton: GO_STATE_ACTIVE for the template's auto_close, then the core's
// goober update closes it and keeps it, consumable 0) and locks each credited player out of the spawn (UseLocks: while
// their quest is incomplete and their count still covers this credit). A locked player is refused and, once the open
// animation has played, does not see the spawn any more (CanBeSeen); it comes back when the lock ends (turned in,
// abandoned). Everyone else keeps seeing and using it.

#include "GameObject.h"
#include "GameObjectAI.h"
#include "GameObjectScript.h"
#include "GameTime.h"
#include "Group.h"
#include "Log.h"
#include "LootMgr.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"

#include "questloot/EfQuestLootLogic.h"

using namespace Emberfall::QuestLoot;

namespace
{
    int64_t Now() { return static_cast<int64_t>(GameTime::GetGameTime().count()); }

    class PersonalQuestObjectAI : public GameObjectAI
    {
    public:
        PersonalQuestObjectAI(GameObject* go, Mode mode) : GameObjectAI(go), m_mode(mode) { }

        // Hide mode: a player locked out of this object does not see it.
        bool CanBeSeen(Player const* seer) override
        {
            return m_mode != Mode::Hide || !m_lockouts.Locked(seer->GetGUID().GetRawValue(), Now());
        }

        // Asked before the object is opened (Spell::SendLoot for the Open spell, GameObject::Use): true refuses.
        bool GossipHello(Player* player, bool /*reportUse*/) override
        {
            if (!m_lockouts.Locked(player->GetGUID().GetRawValue(), Now()))
                return false;
            player->SendLootRelease(me->GetGUID());
            return true;
        }

        void OnStateChanged(uint32 state, Unit* /*unit*/) override
        {
            if (me->GetGoType() == GAMEOBJECT_TYPE_CHEST && KeepOnStateChange(state, me->isSpawnedByDefault(), me->GetRespawnTime()))
                me->SetLootState(GO_READY);
        }

        void UpdateAI(uint32 diff) override
        {
            if (m_lockouts.Empty())
                return;
            if (m_timer > diff)
            {
                m_timer -= diff;
                return;
            }
            m_timer = 500;
            int64_t const now = Now();
            if (m_mode == Mode::Hide)
            {
                auto looting = [this](uint64_t raw)
                {
                    Player* player = ObjectAccessor::GetPlayer(*me, ObjectGuid(raw));
                    return player && player->GetLootGUID() == me->GetGUID();
                };
                for (uint64_t raw : m_lockouts.TakeHideReady(now, looting))
                    Refresh(raw);
            }
            for (uint64_t raw : m_lockouts.TakeExpired(now))
                if (m_mode == Mode::Hide)
                    Refresh(raw);
        }

        // The player took an item from this object's loot.
        void Looted(Player* player)
        {
            m_lockouts.Looted(player->GetGUID().GetRawValue(), Now(), me->GetRespawnDelay());
            m_timer = 0;
            LOG_DEBUG("scripts", "Emberfall personal quest object {} (spawn {}): {} took their item, locked out for {} s",
                      me->GetEntry(), me->GetSpawnId(), player->GetName(), std::max<int64_t>(me->GetRespawnDelay(), kMinLockoutSeconds));
        }

    private:
        // Shows or hides the object for the player as CanBeSeen now says (when they are on the map and near).
        void Refresh(uint64_t raw)
        {
            if (Player* player = ObjectAccessor::GetPlayer(*me, ObjectGuid(raw)))
                if (player->IsInWorld() && player->GetMap() == me->GetMap())
                    player->UpdateVisibilityOf(me);
        }

        Mode m_mode;
        Lockouts m_lockouts;
        uint32 m_timer = 0;
    };

    // A goober whose use credits a quest objective of more than one: each player is credited once per spawn.
    class PersonalQuestGooberAI : public GameObjectAI
    {
    public:
        explicit PersonalQuestGooberAI(GameObject* go) : GameObjectAI(go)
        {
            m_quest = go->GetGOInfo()->goober.questId;
            if (Quest const* quest = sObjectMgr->GetQuestTemplate(m_quest))
                for (uint8 j = 0; j < QUEST_OBJECTIVES_COUNT; ++j)
                    if (quest->RequiredNpcOrGo[j] == -int32(go->GetEntry()))
                        m_objective = j;
        }

        bool CanBeSeen(Player const* seer) override
        {
            bool incomplete = false;
            uint32 count = 0;
            Progress(seer, incomplete, count);
            return !m_uses.Hidden(seer->GetGUID().GetRawValue(), incomplete, count, Now());
        }

        // GameObject::Use asks this first; true means the use is handled here (the stock goober use never runs). The
        // client's CMSG_GAMEOBJ_REPORT_USE after a use (reportUse) only counts achievements: left to the core.
        bool GossipHello(Player* player, bool reportUse) override
        {
            if (reportUse)
                return false;
            if (player->GetQuestStatus(m_quest) != QUEST_STATUS_INCOMPLETE || Locked(player))
                return true;
            std::vector<Player*> credited;
            if (Group* group = player->GetGroup())
            {
                for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
                    if (Player* member = itr->GetSource())
                        if (member == player || member->IsAtGroupRewardDistance(me))
                            credited.push_back(member);
            }
            else
                credited.push_back(player);

            uint32 const openMs = me->GetGOInfo()->GetAutoCloseTime() ? me->GetGOInfo()->GetAutoCloseTime() : 3000;
            int64_t const hideAt = Now() + int64_t((openMs + 999) / 1000);
            for (Player* member : credited)
            {
                if (Locked(member) || !member->HasQuestForGO(me->GetEntry()))
                    continue;
                member->KillCreditGO(me->GetEntry(), me->GetGUID());
                bool incomplete = false;
                uint32 count = 0;
                Progress(member, incomplete, count);
                m_uses.Used(member->GetGUID().GetRawValue(), count, hideAt);
                LOG_DEBUG("scripts", "Emberfall personal quest goober {} (spawn {}): {} credited ({}), locked out",
                          me->GetEntry(), me->GetSpawnId(), member->GetName(), count);
            }
            m_timer = 0;
            if (me->getLootState() == GO_READY)
                me->UseDoorOrButton(openMs, false, player);
            return true;
        }

        void UpdateAI(uint32 diff) override
        {
            if (m_uses.Empty())
                return;
            if (m_timer > diff)
            {
                m_timer -= diff;
                return;
            }
            m_timer = 500;
            for (uint64_t raw : m_uses.TakeHideReady(Now()))
                Refresh(raw);
            auto progress = [this](uint64_t raw, bool& incomplete, uint32_t& count)
            {
                Player* player = ObjectAccessor::GetPlayer(*me, ObjectGuid(raw));
                if (!player || !player->IsInWorld() || player->GetMap() != me->GetMap())
                    return false;
                uint32 c = 0;
                Progress(player, incomplete, c);
                count = c;
                return true;
            };
            for (uint64_t raw : m_uses.TakeReleased(progress))
                Refresh(raw);
        }

    private:
        // The player's quest state for this object: incomplete, and their count of its objective while the quest is in
        // their log (also once complete: the credit that completed it is recorded as the full count, so abandoning a
        // completed quest and taking it again, back to 0, frees the object).
        void Progress(Player const* player, bool& incomplete, uint32& count) const
        {
            incomplete = player->GetQuestStatus(m_quest) == QUEST_STATUS_INCOMPLETE;
            count = 0;
            uint16 const slot = player->FindQuestSlot(m_quest);
            if (slot < MAX_QUEST_LOG_SIZE && m_objective < QUEST_OBJECTIVES_COUNT)
                count = player->GetQuestSlotCounter(slot, m_objective);
        }

        bool Locked(Player const* player) const
        {
            bool incomplete = false;
            uint32 count = 0;
            Progress(player, incomplete, count);
            return m_uses.Locked(player->GetGUID().GetRawValue(), incomplete, count);
        }

        void Refresh(uint64_t raw)
        {
            if (Player* player = ObjectAccessor::GetPlayer(*me, ObjectGuid(raw)))
                if (player->IsInWorld() && player->GetMap() == me->GetMap())
                    player->UpdateVisibilityOf(me);
        }

        uint32 m_quest = 0;
        uint8 m_objective = QUEST_OBJECTIVES_COUNT;
        UseLocks m_uses;
        uint32 m_timer = 0;
    };

    PersonalQuestObjectAI* PersonalAI(GameObject* go)
    {
        return go ? dynamic_cast<PersonalQuestObjectAI*>(go->AI()) : nullptr;
    }

    GameObject* LootedObject(Player* player, ObjectGuid lootGuid)
    {
        if (!lootGuid.IsGameObject() || !player->IsInWorld())
            return nullptr;
        return player->GetMap()->GetGameObject(lootGuid);
    }

    class emberfall_personal_quest_object : public GameObjectScript
    {
    public:
        emberfall_personal_quest_object() : GameObjectScript(kScriptHide.data()) { }
        GameObjectAI* GetAI(GameObject* go) const override { return new PersonalQuestObjectAI(go, Mode::Hide); }
    };

    class emberfall_personal_quest_object_stays : public GameObjectScript
    {
    public:
        emberfall_personal_quest_object_stays() : GameObjectScript(kScriptStay.data()) { }
        GameObjectAI* GetAI(GameObject* go) const override { return new PersonalQuestObjectAI(go, Mode::Stay); }
    };

    class emberfall_personal_quest_goober : public GameObjectScript
    {
    public:
        emberfall_personal_quest_goober() : GameObjectScript(kScriptGoober.data()) { }
        GameObjectAI* GetAI(GameObject* go) const override { return new PersonalQuestGooberAI(go); }
    };

    class emberfall_questloot_player : public PlayerScript
    {
    public:
        emberfall_questloot_player()
            : PlayerScript("emberfall_questloot_player", { PLAYERHOOK_ON_BEFORE_SEND_LOOT, PLAYERHOOK_ON_LOOT_ITEM }) { }

        // A second player opening a personal object whose loot another opened: their own quest item list.
        void OnPlayerBeforeSendLoot(Player* player, ObjectGuid lootGuid, Loot* loot) override
        {
            if (!loot || !PersonalAI(LootedObject(player, lootGuid)))
                return;
            if (!loot->GetPlayerQuestItems().count(player->GetGUID()))
                loot->FillNotNormalLootFor(player);
        }

        void OnPlayerLootItem(Player* player, Item* /*item*/, uint32 /*count*/, ObjectGuid lootGuid) override
        {
            if (PersonalQuestObjectAI* ai = PersonalAI(LootedObject(player, lootGuid)))
                ai->Looted(player);
        }
    };
}

void AddEmberfallQuestLootScripts()
{
    new emberfall_personal_quest_object();
    new emberfall_personal_quest_object_stays();
    new emberfall_personal_quest_goober();
    new emberfall_questloot_player();
}
