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

// The arithmetic of the Hunter Spirit Caller's v2.0 scripts (Emberfall docs/design/classes/hunter-spiritcaller.md,
// ankarna's "Spirit Caller - Developer Handoff v2.0", 2026-10-31; src/emberfall_classkit_spirit.cpp and the rules of
// src/emberfall_classkit_exec.cpp): the refresh window, the bonus by an exact count of enemies, a heal over time raised
// at low health and paid out at once, the cap on one hit, the damage a damage over time has left. No core types, so
// tests/classkit_spirit_tests.cpp runs it without a server.

#include <algorithm>
#include <cstdint>

namespace Emberfall::ClassKit::SpiritLogic
{
    // Spirit of the Rabbit: it may be cast again only while at most `windowMs` of its own aura are left (none: always).
    inline bool MayRefresh(int32_t leftMs, int32_t windowMs)
    {
        return leftMs <= 0 || leftMs <= windowMs;
    }

    // Focused Fury: `pct` more when the cast struck exactly `exactly` enemies, else nothing.
    inline int32_t ExactCountPct(uint32_t struck, uint32_t exactly, int32_t pct)
    {
        return exactly > 0 && struck == exactly ? std::max(0, pct) : 0;
    }

    // Desperate Renewal: a tick of `amount`, `pct` percent more when the owner's health (in percent) is at or below
    // `atOrBelow` at the cast.
    inline int32_t LowHealthTick(int32_t amount, float healthPct, int32_t atOrBelow, int32_t pct)
    {
        if (atOrBelow <= 0 || healthPct > float(atOrBelow) || pct <= 0)
            return amount;
        return int32_t(int64_t(amount) * (100 + pct) / 100);
    }

    // The ticks a periodic aura has left (its total less those done; never below 0).
    inline uint32_t TicksLeft(uint32_t totalTicks, uint32_t ticksDone)
    {
        return totalTicks > ticksDone ? totalTicks - ticksDone : 0;
    }

    // Undying Spirit: what the ticks left of a heal over time of `pctPerTick` percent of `maxHealth` heal together.
    inline int64_t HealLeft(uint64_t maxHealth, int32_t pctPerTick, uint32_t ticksLeft)
    {
        if (pctPerTick <= 0)
            return 0;
        return int64_t(maxHealth) * pctPerTick / 100 * int64_t(ticksLeft);
    }

    // Undying Spirit: what a hit of `damage` must lose so that `health` ends at 1 (0 when it would not kill).
    inline uint32_t FatalAbsorb(uint32_t damage, uint32_t health)
    {
        if (health == 0 || damage < health)
            return 0;
        return damage - (health - 1);
    }

    // Unbroken Spirit: what a hit of `damage` has beyond `capPct` percent of `maxHealth` (absorbed; 0 at or below it).
    inline uint32_t OverCap(uint32_t damage, uint64_t maxHealth, int32_t capPct)
    {
        if (capPct <= 0)
            return 0;
        uint64_t const cap = maxHealth * uint64_t(capPct) / 100;
        return damage > cap ? uint32_t(damage - cap) : 0;
    }

    // Serpent's Judgment: `pct` percent of what `ticksLeft` ticks of `perTick` would still deal.
    inline int32_t BurstOfLeft(int32_t perTick, uint32_t ticksLeft, int32_t pct)
    {
        if (perTick <= 0 || pct <= 0)
            return 0;
        int64_t const left = int64_t(perTick) * ticksLeft;
        return int32_t(std::min<int64_t>(left * pct / 100, INT32_MAX));
    }
}
