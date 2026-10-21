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

#include "GameObject.h"
#include "GameObjectAI.h"
#include "GameObjectScript.h"
#include "GameTime.h"
#include "Log.h"
#include "LootMgr.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
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
    new emberfall_questloot_player();
}
