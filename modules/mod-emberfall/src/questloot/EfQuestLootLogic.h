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

// The pure parts of Emberfall's personal quest objects (src/emberfall_questloot.cpp; tests/questloot_tests.cpp): the
// per-player lockout of one spawned object (who took their quest item from it, until when; who still waits to have it
// hidden because their loot window is open) and the rule that tells a loot release from a real despawn. No core headers:
// players are their raw GUIDs, times are game time in seconds, the loot states are LootState's numbers (GameObject.h).

#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Emberfall::QuestLoot
{
    // LootState (GameObject.h): GO_NOT_READY 0, GO_READY 1, GO_ACTIVATED 2, GO_JUST_DEACTIVATED 3.
    constexpr uint32_t kGoReady = 1, kGoJustDeactivated = 3;

    // A player's lockout lasts the spawn's respawn time, at least this long (a spawn with spawntimesecs 0 or a few
    // seconds would let one player take the item again at once).
    constexpr int64_t kMinLockoutSeconds = 10;

    // The two script names (gameobject_template.ScriptName, Emberfall's compiler sets them on every chest whose loot is
    // only quest items): Hide = the object disappears for the player who took their item until their own respawn
    // (a reed stook, an eel trap); Stay = it stays in sight and only refuses that player (a wreck's fore-deck, an object
    // that never despawned: gameobjects.yaml `consumable: 0`).
    enum class Mode { Hide, Stay };
    constexpr std::string_view kScriptHide = "emberfall_personal_quest_object";
    constexpr std::string_view kScriptStay = "emberfall_personal_quest_object_stays";

    // A clicked quest object (a goober whose use credits a quest objective of more than one: the fold's gates, the
    // deserters' cook-fires): each player is credited once per spawn, so three gates take three gates. Emberfall's
    // compiler names this script on such goobers (gameobjects.yaml `type: goober` with `data.quest`).
    constexpr std::string_view kScriptGoober = "emberfall_personal_quest_goober";

    // The stock chest despawns when its loot is empty at a release (WorldSession::DoLootRelease: GO_JUST_DEACTIVATED,
    // then GameObject::Update deletes it until its respawn). A personal object keeps standing: it goes back to GO_READY
    // and the next player's opening fills fresh loot. DespawnOrUnsummon (a GM's despawn, a script) sets the respawn
    // time before it sets GO_JUST_DEACTIVATED, a release leaves it 0: that tells the two apart, and only a spawned
    // (not summoned) object is kept.
    inline bool KeepOnStateChange(uint32_t newState, bool spawnedByDefault, int64_t respawnTime)
    {
        return newState == kGoJustDeactivated && spawnedByDefault && respawnTime == 0;
    }

    // One spawned object's lockouts.
    class Lockouts
    {
    public:
        // The player took a quest item from the object at `now`; the lockout runs for the spawn's respawn delay.
        // Hidden later (Hide mode), once their loot window is closed (TakeHideReady).
        void Looted(uint64_t player, int64_t now, int64_t respawnDelay)
        {
            m_until[player] = now + std::max<int64_t>(respawnDelay, kMinLockoutSeconds);
            if (std::find(m_pendingHide.begin(), m_pendingHide.end(), player) == m_pendingHide.end())
                m_pendingHide.push_back(player);
        }

        // True while the player may not open the object again (and, in Hide mode, does not see it).
        bool Locked(uint64_t player, int64_t now) const
        {
            auto it = m_until.find(player);
            return it != m_until.end() && now < it->second;
        }

        // The players waiting to be hidden whose loot window on the object is closed (`looting(player)` false); they are
        // taken off the waiting list. A player whose lockout ended meanwhile is dropped without being returned.
        template <class Looting>
        std::vector<uint64_t> TakeHideReady(int64_t now, Looting&& looting)
        {
            std::vector<uint64_t> ready;
            auto keep = m_pendingHide.begin();
            for (uint64_t player : m_pendingHide)
            {
                if (!Locked(player, now))
                    continue;
                if (looting(player))
                    *keep++ = player;
                else
                    ready.push_back(player);
            }
            m_pendingHide.erase(keep, m_pendingHide.end());
            return ready;
        }

        // The players whose lockout ended at `now` (they see and may open the object again); they are forgotten.
        std::vector<uint64_t> TakeExpired(int64_t now)
        {
            std::vector<uint64_t> expired;
            for (auto it = m_until.begin(); it != m_until.end();)
            {
                if (now >= it->second)
                {
                    expired.push_back(it->first);
                    it = m_until.erase(it);
                }
                else
                    ++it;
            }
            std::sort(expired.begin(), expired.end());
            return expired;
        }

        bool Empty() const { return m_until.empty() && m_pendingHide.empty(); }
        size_t Size() const { return m_until.size(); }

    private:
        std::unordered_map<uint64_t, int64_t> m_until;
        std::vector<uint64_t> m_pendingHide;
    };

    // One spawned quest goober's uses. A player who was credited by it is locked out of it (refused, and hidden once
    // its open animation has played) while their quest is incomplete and their objective count is still at least what
    // that use made it: abandoning and taking the quest again (the count falls back to 0) or a repeatable quest's next
    // round frees the object again without a hook, and so does turning the quest in (no longer incomplete).
    class UseLocks
    {
    public:
        // The player was credited by the object; `countAfter` is their objective count after the credit; they are
        // hidden from `hideAt` on (game time in seconds, the end of the object's open animation).
        void Used(uint64_t player, uint32_t countAfter, int64_t hideAt)
        {
            m_uses[player] = Use{countAfter, hideAt, false};
        }

        // True while the player may not use the object again: their quest is incomplete and their count still covers
        // the credit this object gave.
        bool Locked(uint64_t player, bool incomplete, uint32_t count) const
        {
            auto it = m_uses.find(player);
            return it != m_uses.end() && incomplete && count >= it->second.countAfter;
        }

        // Locked and past the open animation: the player does not see the object.
        bool Hidden(uint64_t player, bool incomplete, uint32_t count, int64_t now) const
        {
            auto it = m_uses.find(player);
            return Locked(player, incomplete, count) && now >= it->second.hideAt;
        }

        // The players whose open animation has played at `now` (to be hidden by a visibility update), once each.
        std::vector<uint64_t> TakeHideReady(int64_t now)
        {
            std::vector<uint64_t> ready;
            for (auto& [player, use] : m_uses)
                if (!use.hideSent && now >= use.hideAt)
                {
                    use.hideSent = true;
                    ready.push_back(player);
                }
            std::sort(ready.begin(), ready.end());
            return ready;
        }

        // The players whose lock has ended; `progress(player, incomplete, count)` fills their quest state and returns
        // false when the player is not near (kept). They are forgotten and see and may use the object again.
        template <class Progress>
        std::vector<uint64_t> TakeReleased(Progress&& progress)
        {
            std::vector<uint64_t> released;
            for (auto it = m_uses.begin(); it != m_uses.end();)
            {
                bool incomplete = false;
                uint32_t count = 0;
                if (progress(it->first, incomplete, count) && !(incomplete && count >= it->second.countAfter))
                {
                    released.push_back(it->first);
                    it = m_uses.erase(it);
                }
                else
                    ++it;
            }
            std::sort(released.begin(), released.end());
            return released;
        }

        bool Empty() const { return m_uses.empty(); }
        size_t Size() const { return m_uses.size(); }

    private:
        struct Use
        {
            uint32_t countAfter;
            int64_t hideAt;
            bool hideSent;
        };
        std::unordered_map<uint64_t, Use> m_uses;
    };
}
