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

#pragma once

// The arithmetic of the Damnator's scripts (Emberfall docs/design/classes/warlock-damnator.md;
// src/emberfall_classkit_seal.cpp), free of the core so tests/classkit_tests.cpp can run it: a curse's tick bonus, the
// tick multiplied, the ticks left, Crimson Collapse's burst, Sanguine Torrent's extension, Blood Tide's picks and Seals.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Emberfall::ClassKit::Seal
{
    // The percent more a curse's tick deals (spell_ef_kit_seal_dot): the conditional bonus (effect 3's misc B plus the
    // talents naming the curse: Bloodbinding beside Brine, Drowning Vow below 35 %) when its condition holds, Cursed
    // Shoals' while the target lacks the caster's Vow, the window's (Crimson Flood +50 %) while it holds, a Torrent flare's
    // (Red Wake). Never below -100.
    constexpr int32_t TickBonus(bool conditionMet, int32_t conditional, int32_t lacking, int32_t window, int32_t flare)
    {
        int32_t const pct = (conditionMet ? conditional : 0) + lacking + window + flare;
        return std::max<int32_t>(-100, pct);
    }

    // `amount` with `pct` percent more (rounded down, never below 0).
    constexpr int64_t WithBonus(int64_t amount, int32_t pct)
    {
        return std::max<int64_t>(0, amount * (100 + int64_t(pct)) / 100);
    }

    // The ticks still to come of a curse with `leftMs` to run, a tick every `amplitudeMs`: the time left rounded up to
    // whole ticks (Crimson Collapse; the doc's §3).
    constexpr int32_t TicksLeft(int32_t leftMs, int32_t amplitudeMs)
    {
        if (leftMs <= 0 || amplitudeMs <= 0)
            return 0;
        return int32_t((int64_t(leftMs) + amplitudeMs - 1) / amplitudeMs);
    }

    // What one curse still had to deal: its ticks left times the tick as it would land now.
    constexpr int64_t Remaining(int32_t leftMs, int32_t amplitudeMs, int64_t tickNow)
    {
        return int64_t(TicksLeft(leftMs, amplitudeMs)) * std::max<int64_t>(0, tickNow);
    }

    // Crimson Collapse's hit on one enemy: `pct` percent (125; Hungry Deep 135/145) of everything left (the window's
    // +50 % already in each tick: 10,000 left -> 12,500, in Crimson Flood 18,750). Capped at what an int32 base point holds.
    constexpr int32_t CollapseHit(int64_t remaining, int32_t pct)
    {
        int64_t const hit = std::max<int64_t>(0, remaining) * std::max<int32_t>(0, pct) / 100;
        return int32_t(std::min<int64_t>(hit, 0x7FFFFFFF));
    }

    // Sanguine Torrent: `seals` spent, `msPerSeal` (1000; Rising Water 1250/1500) added to each curse, past every cap.
    constexpr int32_t TorrentExtension(int32_t seals, int32_t msPerSeal)
    {
        return std::max<int32_t>(0, seals) * std::max<int32_t>(0, msPerSeal);
    }

    // A curse's duration after the Torrent: `leftMs` + the extension; its max duration never below the new time.
    constexpr int32_t Extended(int32_t leftMs, int32_t extensionMs)
    {
        return std::max<int32_t>(0, leftMs) + std::max<int32_t>(0, extensionMs);
    }

    // Blood Tide's candidates: how many of the target's curses each lacks and its distance (squared) to the target.
    struct Candidate
    {
        std::size_t index = 0;
        int32_t lacking = 0;
        float distSq = 0.0f;
    };

    // The `count` enemies Blood Tide reaches: most curses lacking first (both, then one, then none), the closest on a
    // tie; their indices in that order.
    inline std::vector<std::size_t> PickSpread(std::vector<Candidate> candidates, int32_t count)
    {
        std::stable_sort(candidates.begin(), candidates.end(), [](Candidate const& a, Candidate const& b)
        {
            if (a.lacking != b.lacking)
                return a.lacking > b.lacking;
            return a.distSq < b.distSq;
        });
        std::vector<std::size_t> out;
        for (Candidate const& c : candidates)
        {
            if (int32_t(out.size()) >= count)
                break;
            out.push_back(c.index);
        }
        return out;
    }

    // Blood Tide's Seals: `perEnemy` for each newly affected enemy, at most `maxEnemies` of them pay (1 each, at most 2).
    constexpr int32_t SpreadSeals(int32_t newlyAffected, int32_t perEnemy, int32_t maxEnemies)
    {
        int32_t const paying = maxEnemies > 0 ? std::min(newlyAffected, maxEnemies) : newlyAffected;
        return std::max<int32_t>(0, paying) * std::max<int32_t>(0, perEnemy);
    }

    // Blood Tide copies a curse only where it lengthens the enemy's own (never shortening a longer one).
    constexpr bool CopyLengthens(int32_t targetLeftMs, int32_t ownLeftMs)
    {
        return targetLeftMs > 0 && targetLeftMs > std::max<int32_t>(0, ownLeftMs);
    }

    // Drink Deep: `pct` percent of a tick's damage as healing (rounded down).
    constexpr int32_t ShareOf(int64_t damage, int32_t pct)
    {
        return int32_t(std::min<int64_t>(std::max<int64_t>(0, damage) * std::max<int32_t>(0, pct) / 100, 0x7FFFFFFF));
    }
}
