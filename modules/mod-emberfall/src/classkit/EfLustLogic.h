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

// The arithmetic of the Barbarian's generic scripts (Emberfall docs/design/classes/warrior-barbarian.md;
// src/emberfall_classkit_lust.cpp): stacks with a timer each, the heal from the enemies struck hardest, the damage scaled
// by the number of enemies, the percent more a builder builds. No core types, so tests/classkit_lust_tests.cpp runs it
// without a server.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <vector>

namespace Emberfall::ClassKit::LustLogic
{
    // Timed stacks (Savage Resilience): one expiry time per stack, each its own timer. The stacks that ran out by `now`
    // go; returns how many are left.
    inline uint32_t PruneStacks(std::vector<uint64_t>& expiries, uint64_t now)
    {
        expiries.erase(std::remove_if(expiries.begin(), expiries.end(), [now](uint64_t at) { return at <= now; }),
                       expiries.end());
        return uint32_t(expiries.size());
    }

    // `count` new stacks of `durationMs` each; gaining stacks never refreshes the old ones. Above `cap` the stacks with the
    // shortest time left make room (the new ones always stay while `count` <= `cap`). Returns the stacks held.
    inline uint32_t AddStacks(std::vector<uint64_t>& expiries, uint64_t now, uint32_t count, uint32_t durationMs, uint32_t cap)
    {
        PruneStacks(expiries, now);
        if (cap == 0 || durationMs == 0)
            return uint32_t(expiries.size());
        for (uint32_t i = 0; i < count; ++i)
            expiries.push_back(now + durationMs);
        if (expiries.size() > cap)
        {
            std::sort(expiries.begin(), expiries.end());
            expiries.erase(expiries.begin(), expiries.begin() + std::ptrdiff_t(expiries.size() - cap));
        }
        return uint32_t(expiries.size());
    }

    // The time until the last stack runs out (the aura's duration), 0 without stacks.
    inline uint32_t LongestLeft(std::vector<uint64_t> const& expiries, uint64_t now)
    {
        uint64_t last = 0;
        for (uint64_t at : expiries)
            last = std::max(last, at);
        return last > now ? uint32_t(last - now) : 0;
    }

    // The sum of the `n` largest amounts (Thunderous Bellow: the 5 enemies that took the most; fewer struck: all of them).
    inline int64_t TopSum(std::vector<int32_t> amounts, uint32_t n)
    {
        std::sort(amounts.begin(), amounts.end(), std::greater<int32_t>());
        int64_t sum = 0;
        for (uint32_t i = 0; i < amounts.size() && i < n; ++i)
            sum += std::max<int32_t>(0, amounts[i]);
        return sum;
    }

    // A share of an amount in percent (never below 0).
    constexpr int32_t Pct(int64_t amount, int32_t pct)
    {
        if (amount <= 0 || pct <= 0)
            return 0;
        return int32_t(amount * pct / 100);
    }

    // The heal from the damage dealt: `pct` percent of the `n` largest hits (0 when nothing was struck).
    inline int32_t TopHeal(std::vector<int32_t> const& amounts, uint32_t n, int32_t pct)
    {
        return Pct(TopSum(amounts, n), pct);
    }

    // The damage percent by the number of enemies struck (Ruinous Cleave: 1 250 %, 2 200 %, 3 150 %, 4 120 %, 5+ 100 %):
    // `table` holds the percent for 1, 2, ... enemies; more enemies than entries take the last.
    inline int32_t CountScalePct(uint32_t enemies, std::vector<int32_t> const& table)
    {
        if (table.empty())
            return 100;
        if (enemies == 0)
            return table.front();
        return table[std::min<std::size_t>(enemies, table.size()) - 1];
    }

    // A builder's gain with `pct` percent more (Primal Fury: +50 %; 20 -> 30, 30 -> 45, 50 -> 75), rounded down.
    constexpr int32_t BuildPct(int32_t gain, int32_t pct)
    {
        if (gain <= 0)
            return 0;
        return std::max<int32_t>(0, int32_t(int64_t(gain) * (100 + pct) / 100));
    }

    // ---- the finalized tree (ankarna's "Barbarian Tank" handoff, 2026-10-29) --------------------------------------------

    // Overflowing Fury: the absorb a heal's overheal leaves. `heal` was meant, `missing` the health the caster lacked,
    // `existing` what the shield still holds, `capAmount` its most. 0: no overheal (nothing changes).
    constexpr int64_t OverhealShield(int64_t heal, int64_t missing, int64_t existing, int64_t capAmount)
    {
        int64_t const over = heal - std::max<int64_t>(0, missing);
        if (heal <= 0 || over <= 0 || capAmount <= 0)
            return 0;
        return std::min(capAmount, std::max<int64_t>(0, existing) + over);
    }

    // Vengeful Fury: `want` points a hit, at most `capPerSecond` in a 1 s window (0: no cap). The window starts with the
    // first gain after the last one ran out. Returns the points granted.
    inline int32_t CappedGain(uint64_t& windowStart, int32_t& gained, uint64_t now, int32_t want, int32_t capPerSecond)
    {
        if (want <= 0)
            return 0;
        if (capPerSecond <= 0)
            return want;
        if (gained == 0 || now >= windowStart + 1000)
        {
            windowStart = now;
            gained = 0;
        }
        int32_t const granted = std::max<int32_t>(0, std::min(want, capPerSecond - gained));
        gained += granted;
        return granted;
    }

    // Focused Devastation: `atOne` percent against one enemy, 100 from `fullAt` enemies on, linear between (floored).
    constexpr int32_t LinearCountPct(uint32_t enemies, int32_t atOne, int32_t fullAt)
    {
        if (enemies <= 1)
            return atOne;
        if (fullAt <= 1 || int64_t(enemies) >= fullAt)
            return 100;
        int64_t const num = int64_t(100 - atOne) * int64_t(enemies - 1);
        int64_t const den = fullAt - 1;
        // floor for the descending case too
        int64_t step = num / den;
        if (num % den != 0 && num < 0)
            --step;
        return int32_t(atOne + step);
    }

    // Savage Juggernaut: the stacks are at their maximum.
    constexpr bool AtFullStacks(uint32_t held, uint32_t cap) { return cap > 0 && held >= cap; }

    // Crippling Blow / Bloodbound Warlord: the strongest of the reductions (percent), 0..100; they never add up.
    inline int32_t StrongestWard(std::vector<int32_t> const& pcts)
    {
        int32_t best = 0;
        for (int32_t p : pcts)
            best = std::max(best, p);
        return std::min(best, 100);
    }

    // The damage left after a reduction of `pct` percent (rounded down; never below 0).
    constexpr int64_t Warded(int64_t damage, int32_t pct)
    {
        if (damage <= 0)
            return 0;
        int32_t const p = std::clamp<int32_t>(pct, 0, 100);
        return damage * (100 - p) / 100;
    }

    // Bloodthirsty Berserker: the ms a spend takes off (`msPerPoint` negative: -100 a point); 0 otherwise.
    constexpr int32_t SpendCutMs(int32_t points, int32_t msPerPoint)
    {
        if (points <= 0 || msPerPoint >= 0)
            return 0;
        return int32_t(std::min<int64_t>(int64_t(points) * -int64_t(msPerPoint), 3600000));
    }
}
