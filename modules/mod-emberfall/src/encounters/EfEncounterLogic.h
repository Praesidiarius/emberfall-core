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

// The mini-encounter framework's logic that needs no core (Emberfall docs/design/mini-encounters.md §3, §5, §7): the
// item groups (ef_enc_item_group), the open encounters (one per player, found by player and by subject), and the last
// messages per player (`.efenc dump`). emberfall_encounters.cpp uses it; tests/encounter_tests.cpp checks it. The EFN
// frame itself is EfEncounterWire.h (shared with the client).

#ifndef EMBERFALL_ENCOUNTER_LOGIC_H
#define EMBERFALL_ENCOUNTER_LOGIC_H

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>

namespace Emberfall::Encounters
{
    // ef_enc_item_group: which items a group matches, and each item's level band.
    class ItemGroups
    {
    public:
        void Clear() { m_groups.clear(); }
        void Add(std::string const& group, std::uint32_t item, std::uint8_t band) { m_groups[group][item] = band; }
        bool Has(std::string const& group, std::uint32_t item) const
        {
            auto g = m_groups.find(group);
            return g != m_groups.end() && g->second.count(item) != 0;
        }
        std::optional<std::uint8_t> Band(std::string const& group, std::uint32_t item) const
        {
            auto g = m_groups.find(group);
            if (g == m_groups.end())
                return std::nullopt;
            auto i = g->second.find(item);
            if (i == g->second.end())
                return std::nullopt;
            return i->second;
        }
        std::size_t Groups() const { return m_groups.size(); }
        std::size_t Rows() const
        {
            std::size_t n = 0;
            for (auto const& g : m_groups)
                n += g.second.size();
            return n;
        }

    private:
        std::map<std::string, std::map<std::uint32_t, std::uint8_t>> m_groups;
    };

    // An open encounter: a kind ('F' fishing, 'T' taming), its subject (a game object or creature GUID, raw), when it
    // started (server ms).
    struct Open
    {
        std::uint32_t id = 0;
        char kind = 0;
        std::uint64_t subject = 0;
        std::uint64_t startMs = 0;
    };

    // The open encounters: at most one per player (keyed by the player's low GUID).
    class Book
    {
    public:
        // A new encounter, or nullopt when the player has one open already.
        std::optional<Open> Start(std::uint64_t player, char kind, std::uint64_t subject, std::uint64_t nowMs)
        {
            if (m_open.count(player))
                return std::nullopt;
            Open o;
            o.id = ++m_nextId;
            if (o.id == 0)
                o.id = ++m_nextId;
            o.kind = kind;
            o.subject = subject;
            o.startMs = nowMs;
            m_open[player] = o;
            return o;
        }
        std::optional<Open> Of(std::uint64_t player) const
        {
            auto it = m_open.find(player);
            if (it == m_open.end())
                return std::nullopt;
            return it->second;
        }
        // The player whose encounter has this subject.
        std::optional<std::uint64_t> BySubject(std::uint64_t subject) const
        {
            for (auto const& [player, o] : m_open)
                if (o.subject == subject)
                    return player;
            return std::nullopt;
        }
        // Ends the player's encounter if it is `id` (0: whichever); true if one ended.
        bool End(std::uint64_t player, std::uint32_t id = 0)
        {
            auto it = m_open.find(player);
            if (it == m_open.end() || (id != 0 && it->second.id != id))
                return false;
            m_open.erase(it);
            return true;
        }
        std::size_t Size() const { return m_open.size(); }

    private:
        std::unordered_map<std::uint64_t, Open> m_open;
        std::uint32_t m_nextId = 0;
    };

    // The last `N` EFN messages per player, both ways ("< " from the client, "> " to it).
    class MessageLog
    {
    public:
        static constexpr std::size_t kKeep = 10;
        void Add(std::uint64_t player, bool fromClient, std::string const& body)
        {
            auto& q = m_log[player];
            q.push_back(std::string(fromClient ? "< " : "> ") + body);
            while (q.size() > kKeep)
                q.pop_front();
        }
        std::deque<std::string> Of(std::uint64_t player) const
        {
            auto it = m_log.find(player);
            return it == m_log.end() ? std::deque<std::string>{} : it->second;
        }
        void Forget(std::uint64_t player) { m_log.erase(player); }

    private:
        std::unordered_map<std::uint64_t, std::deque<std::string>> m_log;
    };
}

#endif
