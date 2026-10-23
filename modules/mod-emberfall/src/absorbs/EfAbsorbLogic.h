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

// The absorb report logic that needs no core (Emberfall docs/design/cooldown-manager.md §9.3): which absorb amounts a
// player is told about, the 100 ms throttle, the EFA message bodies. emberfall_absorbs.cpp uses it;
// tests/absorb_tests.cpp (tests/run_tests.sh) checks it.
//
// Protocol (an addon whisper from the player to themselves, prefix "EFA\t"): one message per absorb aura whose numbers
// changed, `1;0;ABS;<spellId>;<left>;<cap>`; an aura that went away: `1;0;ABS;<spellId>;0;0`. Other units the player
// watches (the group's members on the player's map, the player's selection; Emberfall feedback #48: the absorb segment
// on the client's health bars): `1;0;UNI;<guid>;<total>` when a unit's total of absorbs left changed, `...;0` when it
// has none any more or is no longer watched (<guid> in decimal).

#ifndef EMBERFALL_ABSORB_LOGIC_H
#define EMBERFALL_ABSORB_LOGIC_H

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace Emberfall::Absorbs
{
    constexpr std::string_view kPrefix = "EFA\t";
    constexpr std::uint32_t kVersion = 1;
    constexpr std::uint32_t kPeriodMs = 100;
    // the other units' totals: a little slower (a raid's 39 members for each of its players)
    constexpr std::uint32_t kUnitPeriodMs = 200;

    // One absorb aura on the player now (the core glue sums an aura's absorb effects; several auras of one spell add up).
    struct Snapshot
    {
        std::uint32_t spellId = 0;
        std::int64_t left = 0;        // what it can still absorb
        std::int64_t fixedCap = -1;   // the cap when the core knows it (a class kit shell); -1: the highest amount seen
        std::int64_t applyTime = 0;   // Aura::GetApplyTime(): a new value is a new aura
        std::uint64_t instance = 0;   // the aura object's identity: a new value is a new aura
    };

    // What the player was last told about one spell.
    struct Tracked
    {
        std::int64_t left = 0;
        std::int64_t cap = 0;
        std::int64_t high = 0;        // the highest amount seen since the aura was (re)applied
        std::int64_t applyTime = 0;
        std::uint64_t instance = 0;
    };

    struct Line
    {
        std::uint32_t spellId = 0;
        std::int64_t left = 0;
        std::int64_t cap = 0;
    };

    // The per-player state: the tracked spells and the throttle's clock.
    struct State
    {
        std::map<std::uint32_t, Tracked> spells;
        std::uint32_t elapsedMs = 0;

        // Adds `diffMs`; true once every kPeriodMs (then the caller looks at the auras).
        bool Tick(std::uint32_t diffMs, std::uint32_t periodMs = kPeriodMs)
        {
            elapsedMs += diffMs;
            if (elapsedMs < periodMs)
                return false;
            elapsedMs = 0;
            return true;
        }

        void Clear()
        {
            spells.clear();
            elapsedMs = 0;
        }
    };

    // The lines to send for the absorb auras now on the player, and the state updated to them: a spell new or with
    // other numbers than last time, and `0;0` for a spell that is gone. Snapshots with a negative amount (unlimited
    // absorbs) must not be passed (the glue skips them).
    inline std::vector<Line> Diff(State& state, std::vector<Snapshot> const& now)
    {
        std::vector<Line> lines;
        std::map<std::uint32_t, Tracked> next;
        for (Snapshot const& s : now)
        {
            Tracked t;
            auto prev = state.spells.find(s.spellId);
            bool const fresh = prev == state.spells.end() || prev->second.applyTime != s.applyTime
                || prev->second.instance != s.instance;
            t.high = fresh ? s.left : std::max(prev->second.high, s.left);
            t.left = std::max<std::int64_t>(0, s.left);
            t.cap = std::max(s.fixedCap >= 0 ? s.fixedCap : t.high, t.left);
            t.applyTime = s.applyTime;
            t.instance = s.instance;
            if (prev == state.spells.end() || prev->second.left != t.left || prev->second.cap != t.cap)
                lines.push_back({ s.spellId, t.left, t.cap });
            next[s.spellId] = t;
        }
        for (auto const& [spellId, t] : state.spells)
            if (!next.count(spellId))
                lines.push_back({ spellId, 0, 0 });
        state.spells = std::move(next);
        return lines;
    }

    // The other units' totals the player was last told (guid -> total) and their throttle's clock.
    struct UnitState
    {
        std::map<std::uint64_t, std::int64_t> told;
        std::uint32_t elapsedMs = 0;

        bool Tick(std::uint32_t diffMs, std::uint32_t periodMs = kUnitPeriodMs)
        {
            elapsedMs += diffMs;
            if (elapsedMs < periodMs)
                return false;
            elapsedMs = 0;
            return true;
        }
    };

    struct UnitLine
    {
        std::uint64_t guid = 0;
        std::int64_t total = 0;
    };

    // The lines for the watched units' totals now (guid -> total; units with none may be left out), and the state
    // updated: a unit new or with another total, and `0` for a unit told before that has none now or is not watched.
    inline std::vector<UnitLine> DiffUnits(UnitState& state, std::map<std::uint64_t, std::int64_t> const& now)
    {
        std::vector<UnitLine> lines;
        std::map<std::uint64_t, std::int64_t> next;
        for (auto const& [guid, raw] : now)
        {
            std::int64_t const total = std::max<std::int64_t>(0, raw);
            if (!guid || !total)
                continue;
            auto prev = state.told.find(guid);
            if (prev == state.told.end() || prev->second != total)
                lines.push_back({ guid, total });
            next[guid] = total;
        }
        for (auto const& [guid, total] : state.told)
            if (!next.count(guid))
                lines.push_back({ guid, 0 });
        state.told = std::move(next);
        return lines;
    }

    // `1;0;UNI;<guid>;<total>` (the body) and the whole text.
    inline std::string UnitBody(UnitLine const& l)
    {
        return std::to_string(kVersion) + ";0;UNI;" + std::to_string(l.guid) + ";" + std::to_string(l.total);
    }
    inline std::string UnitText(UnitLine const& l) { return std::string(kPrefix) + UnitBody(l); }

    // Adds up snapshots of the same spell (several auras of one spell: their amounts, their caps; the newest apply
    // time; the lowest identity, so the order of the auras does not matter), in spell order.
    inline std::vector<Snapshot> Merge(std::vector<Snapshot> const& raw)
    {
        std::map<std::uint32_t, Snapshot> by;
        for (Snapshot const& s : raw)
        {
            auto [it, inserted] = by.emplace(s.spellId, s);
            if (inserted)
                continue;
            Snapshot& m = it->second;
            m.left += s.left;
            m.fixedCap = (m.fixedCap >= 0 && s.fixedCap >= 0) ? m.fixedCap + s.fixedCap : -1;
            m.applyTime = std::max(m.applyTime, s.applyTime);
            m.instance = std::min(m.instance, s.instance);
        }
        std::vector<Snapshot> out;
        out.reserve(by.size());
        for (auto const& [id, s] : by)
            out.push_back(s);
        return out;
    }

    // The message body after the prefix: `1;0;ABS;<spellId>;<left>;<cap>`.
    inline std::string Body(Line const& l)
    {
        return std::to_string(kVersion) + ";0;ABS;" + std::to_string(l.spellId) + ";" + std::to_string(l.left) + ";"
            + std::to_string(l.cap);
    }

    // The whole message text: kPrefix + Body.
    inline std::string Text(Line const& l) { return std::string(kPrefix) + Body(l); }
}

#endif
