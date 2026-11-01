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

// The EFQL logic that needs no core (Emberfall feedback #251, docs/design/quest-log.md): the quest log has 50 slots,
// and slots 25-49 are no update fields (the core's EmberfallQuestLog.h, PATCHES.md #36), so the Emberfall client hears
// of them from this channel. emberfall_questlog.cpp uses it; tests/questlog_tests.cpp checks it.
//
// Protocol (an addon whisper from the player to themselves, prefix "EFQL\t", the EFT body framing):
//   1;0;CLR                                                    forget slots 25-49 (the first message after a login)
//   1;0;SLOT;<slot>;<quest>;<state>;<c0>;<c1>;<c2>;<c3>;<time>  one slot, 25-49, whose words changed; <quest> 0: empty
//     state  QUEST_STATE_* flags (1 complete, 2 failed); c0-c3 the four 16-bit counters; time the slot's timer word
//     (a timed quest's end, Unix seconds)
// The longest body is 68 bytes (every number at its maximum), well under the 251 an EFT body may have.

#ifndef EMBERFALL_QUEST_LOG_LOGIC_H
#define EMBERFALL_QUEST_LOG_LOGIC_H

#include "EmberfallQuestLog.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Emberfall::QuestLogSync
{
    using namespace Emberfall::QuestLog;

    constexpr std::string_view kPrefix = "EFQL\t";
    constexpr std::uint32_t kVersion = 1;
    constexpr std::size_t kMaxBody = 251; // eft::kMaxBody: 255 bytes of text less the prefix "EFT\t"

    // One slot's five words, in the update fields' order (quest, state, counters low, counters high, time).
    using Words = std::array<std::uint32_t, kSlotWords>;
    // Slots 25-49, index 0 = slot 25.
    using Ext = std::array<Words, kExtSlots>;

    struct State
    {
        bool started = false; // the CLR went out
        Ext told{};           // what the client was told
    };

    struct SlotLine
    {
        std::uint16_t slot = 0; // 25-49
        Words words{};
    };

    // `1;0;SLOT;<slot>;<quest>;<state>;<c0>;<c1>;<c2>;<c3>;<time>`
    inline std::string SlotBody(SlotLine const& l)
    {
        std::uint64_t const counters = Counters(l.words[2], l.words[3]);
        std::string b = std::to_string(kVersion) + ";0;SLOT;" + std::to_string(l.slot) + ";" + std::to_string(l.words[0]) + ";"
            + std::to_string(l.words[1]);
        for (std::uint8_t c = 0; c < 4; ++c)
            b += ";" + std::to_string(Counter(counters, c));
        return b + ";" + std::to_string(l.words[4]);
    }
    inline std::string ClearBody() { return std::to_string(kVersion) + ";0;CLR"; }
    inline std::string Text(std::string const& body) { return std::string(kPrefix) + body; }

    // The bodies to send for the slots now (`now`): a CLR and every slot that is not empty the first time, then each
    // slot whose words changed; `state` is updated to them.
    inline std::vector<std::string> Diff(State& state, Ext const& now)
    {
        std::vector<std::string> out;
        if (!state.started)
        {
            out.push_back(ClearBody());
            state.started = true;
            state.told = Ext{};
        }
        for (std::uint16_t i = 0; i < kExtSlots; ++i)
            if (now[i] != state.told[i])
            {
                out.push_back(SlotBody({ std::uint16_t(kFieldSlots + i), now[i] }));
                state.told[i] = now[i];
            }
        return out;
    }

    // A body read back (the client's decoder does the same; the tests round-trip with it). nullopt: malformed.
    struct Decoded
    {
        bool clear = false;
        SlotLine line;
    };

    inline std::optional<std::uint64_t> Number(std::string_view t, std::uint64_t max)
    {
        if (t.empty() || t.size() > 10)
            return std::nullopt;
        std::uint64_t v = 0;
        for (char const c : t)
        {
            if (c < '0' || c > '9')
                return std::nullopt;
            v = v * 10 + std::uint64_t(c - '0');
        }
        if (v > max)
            return std::nullopt;
        return v;
    }

    inline std::optional<Decoded> Decode(std::string_view body)
    {
        if (body.empty() || body.size() > kMaxBody)
            return std::nullopt;
        std::vector<std::string_view> f;
        std::size_t start = 0;
        while (true)
        {
            std::size_t const pos = body.find(';', start);
            f.push_back(body.substr(start, pos == std::string_view::npos ? std::string_view::npos : pos - start));
            if (pos == std::string_view::npos)
                break;
            start = pos + 1;
        }
        if (f.size() < 3 || f[0] != "1" || f[1] != "0")
            return std::nullopt;
        Decoded d;
        if (f[2] == "CLR" && f.size() == 3)
        {
            d.clear = true;
            return d;
        }
        if (f[2] != "SLOT" || f.size() != 11)
            return std::nullopt;
        auto const slot = Number(f[3], kSlots - 1);
        if (!slot || *slot < kFieldSlots)
            return std::nullopt;
        d.line.slot = std::uint16_t(*slot);
        std::uint64_t counters = 0;
        for (std::size_t i = 0; i < 7; ++i)
        {
            auto const v = Number(f[4 + i], (i >= 2 && i < 6) ? 0xFFFF : 0xFFFFFFFFull);
            if (!v)
                return std::nullopt;
            if (i == 0)
                d.line.words[0] = std::uint32_t(*v);
            else if (i == 1)
                d.line.words[1] = std::uint32_t(*v);
            else if (i < 6)
                counters = WithCounter(counters, std::uint8_t(i - 2), std::uint16_t(*v));
            else
                d.line.words[4] = std::uint32_t(*v);
        }
        d.line.words[2] = std::uint32_t(counters);
        d.line.words[3] = std::uint32_t(counters >> 32);
        return d;
    }
}

#endif
