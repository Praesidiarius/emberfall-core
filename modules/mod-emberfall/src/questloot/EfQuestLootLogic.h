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
}
