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

// The arithmetic of the Sunwarden's generic scripts (Emberfall docs/design/classes/paladin-sunwarden.md;
// src/emberfall_classkit_sun.cpp): no core types, so tests/classkit_sun_tests.cpp runs it without a server.

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <vector>

namespace Emberfall::ClassKit::SunLogic
{
    // A share of an amount in percent (never below 0).
    constexpr int32_t Pct(int64_t amount, int32_t pct)
    {
        if (amount <= 0 || pct <= 0)
            return 0;
        return int32_t(amount * pct / 100);
    }

    // The overheal of a heal that would have restored `full` and restored `effective`, `pct` percent of it (Solenne's
    // Embrace: 75 % becomes its shield).
    constexpr int32_t OverhealShare(int32_t full, int32_t effective, int32_t pct)
    {
        return Pct(int64_t(full) - std::max<int32_t>(0, effective), pct);
    }

    // A pooled shield (spell_ef_kit_pooled_absorb): what is left of it plus what is added, at most `cap`; a shield above a
    // cap that fell (the owner's max health went down) keeps what it holds but takes nothing more.
    constexpr int32_t PooledAbsorb(int32_t left, int32_t add, int32_t cap)
    {
        int64_t const was = std::max<int32_t>(0, left);
        int64_t const sum = was + std::max<int32_t>(0, add);
        return int32_t(std::min<int64_t>(sum, std::max<int64_t>(was, cap)));
    }

    // The charges still recharging after `restore` come back at once (0: all of them, the recharge stopped; Solenne's
    // Salvation). Charges at full stay full (the restore is lost).
    constexpr uint32_t RechargingAfterRestore(uint32_t recharging, uint32_t restore)
    {
        if (restore == 0 || restore >= recharging)
            return 0;
        return recharging - restore;
    }

    // A counter of casts (Dawn's Renewal): `count` before this one, `need` for the trigger. Returns the count after and
    // whether the trigger fires (the count starts again then, whatever the trigger does).
    struct CountStep
    {
        uint32_t count;
        bool fire;
    };
    constexpr CountStep Count(uint32_t count, int32_t need)
    {
        uint32_t const n = uint32_t(std::max<int32_t>(1, need));
        uint32_t const now = count + 1;
        if (now >= n)
            return { 0, true };
        return { now, false };
    }

    // Mana back on a hit (Sunbreaker): `pct` of max mana, twice that on a crit when `doubleOnCrit`.
    constexpr int32_t ManaOnHit(uint32_t maxMana, int32_t pct, bool crit, bool doubleOnCrit)
    {
        return Pct(int64_t(maxMana), crit && doubleOnCrit ? pct * 2 : pct);
    }

    // The low-health bonus of a heal (Guiding Light): `pct` more when the target's health is below `belowPct`.
    constexpr int32_t LowHealthBonus(int32_t heal, float healthPct, int32_t belowPct, int32_t pct)
    {
        if (heal <= 0 || pct <= 0 || belowPct <= 0 || healthPct >= float(belowPct))
            return heal;
        return int32_t(int64_t(heal) * (100 + pct) / 100);
    }

    // A periodic rider's amount per tick: the whole spread over the ticks (at least 1 where anything is left).
    constexpr int32_t PerTick(int32_t whole, int32_t durationMs, int32_t periodMs)
    {
        if (whole <= 0)
            return 0;
        int32_t const ticks = (periodMs > 0 && durationMs >= periodMs) ? durationMs / periodMs : 1;
        return std::max<int32_t>(1, whole / ticks);
    }

    // The units a healing circle heals in one tick (Solenne's Embrace): every one, or in a raid at most `cap`, the most
    // injured first (the share of health missing; ties keep their order). `healthPct` per candidate; returns the indices
    // healed, in candidate order.
    inline std::vector<size_t> CircleTargets(std::vector<float> const& healthPct, bool raid, int32_t cap)
    {
        std::vector<size_t> order(healthPct.size());
        std::iota(order.begin(), order.end(), size_t(0));
        if (!raid || cap <= 0 || order.size() <= size_t(cap))
            return order;
        std::stable_sort(order.begin(), order.end(), [&healthPct](size_t a, size_t b) { return healthPct[a] < healthPct[b]; });
        order.resize(size_t(cap));
        std::sort(order.begin(), order.end());
        return order;
    }

    // The combo's next step on one button (Dawnstrike): at `stacks` of the combo aura the button casts step `stacks`
    // (0: itself); a step that hits sets the stacks to `after` (0: back to the first).
    constexpr int32_t ComboStep(int32_t stacks, int32_t steps)
    {
        if (stacks <= 0 || stacks > steps)
            return 0;
        return stacks;
    }
}
