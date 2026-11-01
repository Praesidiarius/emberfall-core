/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
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

// EMBERFALL: (PATCHES.md #37, Emberfall feedback #251) the quest log of 50 slots. 3.3.5a keeps the quest log in the
// player's update fields, PLAYER_QUEST_LOG_1_1 .. 25_5 (five words a slot: quest, state, two words of four 16-bit
// counters, timer), and every later player field follows them, so the protocol has room for 25. Slots 0-24 stay
// there; slots 25-49 are words of the player's own (Player::m_emberfallQuestLogExt) behind the same accessors
// (Player.h), and mod-emberfall's EFQL channel tells the Emberfall client about them. This header has the rule
// without any core header, so mod-emberfall's tests (tests/questlog_tests.cpp) run it standalone.

#ifndef EMBERFALL_QUEST_LOG_H
#define EMBERFALL_QUEST_LOG_H

#include <cstdint>

namespace Emberfall::QuestLog
{
    constexpr std::uint16_t kSlots = 50;      // MAX_QUEST_LOG_SIZE
    constexpr std::uint16_t kFieldSlots = 25; // in the update fields
    constexpr std::uint16_t kExtSlots = kSlots - kFieldSlots;
    constexpr std::uint8_t kSlotWords = 5;    // MAX_QUEST_OFFSET
    constexpr std::uint32_t kExtWords = std::uint32_t(kExtSlots) * kSlotWords;

    // Where one word of a slot lives.
    enum class Store : std::uint8_t
    {
        None,  // no such slot or word
        Field, // the update field PLAYER_QUEST_LOG_1_1 + index
        Ext,   // m_emberfallQuestLogExt[index]
    };

    struct Where
    {
        Store store = Store::None;
        std::uint32_t index = 0;
    };

    constexpr Where Locate(std::uint16_t slot, std::uint8_t word)
    {
        if (word >= kSlotWords || slot >= kSlots)
            return {};
        if (slot < kFieldSlots)
            return { Store::Field, std::uint32_t(slot) * kSlotWords + word };
        return { Store::Ext, std::uint32_t(slot - kFieldSlots) * kSlotWords + word };
    }

    // The four 16-bit counters of a slot: two words, the low one first (as Object::GetUInt64Value reads them).
    constexpr std::uint64_t Counters(std::uint32_t low, std::uint32_t high) { return std::uint64_t(low) | (std::uint64_t(high) << 32); }
    constexpr std::uint16_t Counter(std::uint64_t counters, std::uint8_t counter)
    {
        return counter < 4 ? std::uint16_t(counters >> (counter * 16)) : 0;
    }
    constexpr std::uint64_t WithCounter(std::uint64_t counters, std::uint8_t counter, std::uint16_t count)
    {
        if (counter >= 4)
            return counters;
        counters &= ~(std::uint64_t(0xFFFF) << (counter * 16));
        return counters | (std::uint64_t(count) << (counter * 16));
    }
}

#endif
