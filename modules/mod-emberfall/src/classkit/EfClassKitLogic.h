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

// Emberfall class kits: the arithmetic of the self-stack resource (the Paladin's Sunblades, Emberfall
// docs/design/classes/paladin-wrathbringer.md) without the core, so tests/classkit_tests.cpp can check it. EfClassKit.cpp
// and src/emberfall_classkit.cpp (spell_ef_kit_self_stacks, spell_ef_kit_spend_stacks, spell_ef_kit_extend_aura) use it.

#include <algorithm>
#include <cstdint>

namespace Emberfall::ClassKit::Logic
{
    // The cap of a self-stacking aura: its effect 1 value with the caster's spell mods (Sunblades 5; Crown of Blades'
    // +3 makes it 8), never above the aura's own stack maximum (Spell.dbc StackAmount) nor below 1. A value of 0 or less
    // means the stack maximum.
    constexpr int32_t StackCap(int32_t value, int32_t stackMax)
    {
        int32_t const hard = std::max<int32_t>(1, stackMax);
        if (value <= 0)
            return hard;
        return std::clamp<int32_t>(value, 1, hard);
    }

    // The stacks a builder raises. `value` is its stack effect's value (spell mods in: Crown of Blades +1, Marked by
    // Heaven +1, Dawn's Return +1/+2); `perEnemyMax` its misc B: above 0 one stack per enemy struck, at most that many,
    // plus value - 1 (Sunwheel: 1 per enemy, at most 3; under Crown one more). A builder whose enemy effects struck
    // nobody (a miss, a dodge, nobody in reach) raises nothing; one without enemy effects (`needsHit` false) always does.
    constexpr int32_t BuilderGain(int32_t value, int32_t perEnemyMax, uint32_t hits, bool needsHit)
    {
        if (needsHit && hits == 0)
            return 0;
        if (perEnemyMax > 0)
            return std::max<int32_t>(0, std::min<int32_t>(int32_t(hits), perEnemyMax) + value - 1);
        return std::max<int32_t>(0, value);
    }

    // The stacks after a gain: at most the cap, but never fewer than were held (blades raised under Crown of Blades stay
    // when it ends, until they are launched or lapse).
    constexpr int32_t StacksAfter(int32_t have, int32_t gain, int32_t cap)
    {
        if (gain <= 0 || have >= cap)
            return have;
        return std::min<int32_t>(cap, have + gain);
    }

    // How much longer (ms) an aura may still become: `want`, but at most `maxExtraMs` beyond its own duration `baseMs`
    // (spell mods in), counting what it was lengthened already (its max duration now: `currentMaxMs`).
    constexpr int32_t LengthenBy(int32_t want, int32_t maxExtraMs, int32_t currentMaxMs, int32_t baseMs)
    {
        int32_t const room = maxExtraMs - std::max<int32_t>(0, currentMaxMs - baseMs);
        return std::max<int32_t>(0, std::min<int32_t>(want, room));
    }

    // Percent of `max` per stack, for `stacks` stacks (the spenders' mana back, Wrath's Mercy's heal).
    constexpr int64_t PctPerStack(int64_t max, int32_t pctPerStack, uint32_t stacks)
    {
        if (max <= 0 || pctPerStack <= 0)
            return 0;
        return max * pctPerStack * int64_t(stacks) / 100;
    }
}
