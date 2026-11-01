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

// Standalone tests of the quest log of 50 (Emberfall feedback #251; tests/run_tests.sh builds and runs them): the
// core's slot layout (EmberfallQuestLog.h, PATCHES.md #36: slots 0-24 in the update fields, 25-49 in the player's own
// words, nothing past 49) and the EFQL channel (src/questlog/EfQuestLogLogic.h): the login's CLR and slots in use,
// one message per changed slot, every body within the 251 bytes, the round trip, malformed bodies refused.

#include "EfQuestLogLogic.h"

#include <iostream>
#include <set>
#include <string>

using namespace Emberfall::QuestLogSync;

namespace
{
    int g_failed = 0, g_checks = 0;

    void check(bool ok, std::string const& what)
    {
        ++g_checks;
        if (!ok)
        {
            ++g_failed;
            std::cerr << "FAIL: " << what << "\n";
        }
    }

    // A player's quest log as the core keeps it: the update fields' words and the extension's, through Locate only
    // (the way Player::GetQuestSlotField / SetQuestSlotField do).
    struct Log
    {
        std::array<std::uint32_t, kFieldSlots * kSlotWords> fields{};
        std::array<std::uint32_t, kExtWords> ext{};

        std::uint32_t Get(std::uint16_t slot, std::uint8_t word) const
        {
            Where const w = Locate(slot, word);
            return w.store == Store::Field ? fields.at(w.index) : w.store == Store::Ext ? ext.at(w.index) : 0;
        }
        void Set(std::uint16_t slot, std::uint8_t word, std::uint32_t v)
        {
            Where const w = Locate(slot, word);
            if (w.store == Store::Field)
                fields.at(w.index) = v;
            else if (w.store == Store::Ext)
                ext.at(w.index) = v;
        }
        void SetCounter(std::uint16_t slot, std::uint8_t c, std::uint16_t n)
        {
            std::uint64_t const v = WithCounter(Counters(Get(slot, 2), Get(slot, 3)), c, n);
            Set(slot, 2, std::uint32_t(v));
            Set(slot, 3, std::uint32_t(v >> 32));
        }
        std::uint16_t GetCounter(std::uint16_t slot, std::uint8_t c) const { return Counter(Counters(Get(slot, 2), Get(slot, 3)), c); }
        Ext Extension() const
        {
            Ext e{};
            for (std::uint16_t i = 0; i < kExtSlots; ++i)
                for (std::uint8_t w = 0; w < kSlotWords; ++w)
                    e[i][w] = Get(std::uint16_t(kFieldSlots + i), w);
            return e;
        }
    };

    void Layout()
    {
        check(kSlots == 50 && kFieldSlots == 25 && kExtSlots == 25 && kSlotWords == 5, "50 slots: 25 in fields, 25 more");
        check(Locate(0, 0).store == Store::Field && Locate(0, 0).index == 0, "slot 0's quest is PLAYER_QUEST_LOG_1_1");
        check(Locate(24, 4).store == Store::Field && Locate(24, 4).index == 124, "slot 24's timer is the last field word (25_5)");
        check(Locate(25, 0).store == Store::Ext && Locate(25, 0).index == 0, "slot 25 is the extension's first word");
        check(Locate(49, 4).store == Store::Ext && Locate(49, 4).index == kExtWords - 1, "slot 49's timer is the extension's last");
        check(Locate(50, 0).store == Store::None, "slot 50 does not exist");
        check(Locate(3, 5).store == Store::None, "a sixth word does not exist");

        // every word of every slot has a place of its own
        std::set<std::pair<int, std::uint32_t>> seen;
        for (std::uint16_t s = 0; s < kSlots; ++s)
            for (std::uint8_t w = 0; w < kSlotWords; ++w)
                seen.insert({ int(Locate(s, w).store), Locate(s, w).index });
        check(seen.size() == std::size_t(kSlots) * kSlotWords, "no two slot words share a place");

        // writing over the boundary: slot 24 and slot 25 do not touch each other
        Log log;
        log.Set(24, 0, 1001);
        log.Set(25, 0, 1002);
        log.SetCounter(24, 3, 7);
        log.SetCounter(25, 0, 9);
        log.SetCounter(25, 3, 65535);
        check(log.Get(24, 0) == 1001 && log.Get(25, 0) == 1002, "slots 24 and 25 hold their own quests");
        check(log.GetCounter(24, 3) == 7 && log.GetCounter(24, 0) == 0, "slot 24's counters are its own");
        check(log.GetCounter(25, 0) == 9 && log.GetCounter(25, 3) == 65535 && log.GetCounter(25, 1) == 0, "slot 25's counters too");
        log.SetCounter(25, 0, 10);
        check(log.GetCounter(25, 0) == 10 && log.GetCounter(25, 3) == 65535, "a counter changes alone");
        log.Set(49, 4, 1800000000u);
        check(log.Get(49, 4) == 1800000000u && log.ext.back() == 1800000000u, "slot 49's timer");
        log.Set(50, 0, 5);
        check(log.Get(50, 0) == 0, "a write past slot 49 goes nowhere");
        check(Counter(~0ull, 4) == 0 && WithCounter(5, 4, 1) == 5, "a fifth counter does not exist");
    }

    void Channel()
    {
        Log log;
        State state;
        std::vector<std::string> first = Diff(state, log.Extension());
        check(first.size() == 1 && first[0] == "1;0;CLR", "an empty extension at login: just the CLR");
        check(Diff(state, log.Extension()).empty(), "nothing changed: nothing sent");

        // a quest taken into slot 28, a kill credited, completed
        log.Set(28, 0, 91028);
        auto lines = Diff(state, log.Extension());
        check(lines.size() == 1 && lines[0] == "1;0;SLOT;28;91028;0;0;0;0;0;0", "the 29th quest: one SLOT line");
        log.SetCounter(28, 0, 3);
        lines = Diff(state, log.Extension());
        check(lines.size() == 1 && lines[0] == "1;0;SLOT;28;91028;0;3;0;0;0;0", "a kill credit: the slot again");
        log.Set(28, 1, 1);
        log.Set(40, 0, 91040);
        lines = Diff(state, log.Extension());
        check(lines.size() == 2 && lines[0] == "1;0;SLOT;28;91028;1;3;0;0;0;0" && lines[1] == "1;0;SLOT;40;91040;0;0;0;0;0;0",
            "two slots changed: two lines in slot order");
        for (std::uint8_t w = 0; w < kSlotWords; ++w)
            log.Set(28, w, 0);
        lines = Diff(state, log.Extension());
        check(lines.size() == 1 && lines[0] == "1;0;SLOT;28;0;0;0;0;0;0;0", "turned in: the slot is empty");
        log.Set(5, 0, 91005);
        check(Diff(state, log.Extension()).empty(), "a field slot (0-24) is the update fields' business");

        // a relog: a fresh state sends CLR and the slots in use
        State relog;
        lines = Diff(relog, log.Extension());
        check(lines.size() == 2 && lines[0] == "1;0;CLR" && lines[1] == "1;0;SLOT;40;91040;0;0;0;0;0;0", "relog: CLR, then slot 40");

        // sizes: every number at its maximum
        Words big{ 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
        std::string const longest = SlotBody({ 49, big });
        check(longest.size() == 68, "the longest body is 68 bytes (" + std::to_string(longest.size()) + ")");
        check(longest.size() <= kMaxBody && Text(longest).size() <= 255, "within EFT's 251 and the chat's 255");
        Log full;
        for (std::uint16_t s = 0; s < kSlots; ++s)
            for (std::uint8_t w = 0; w < kSlotWords; ++w)
                full.Set(s, w, 0xFFFFFFFFu);
        State fresh;
        bool fits = true;
        for (std::string const& b : Diff(fresh, full.Extension()))
            fits = fits && b.size() <= kMaxBody;
        check(fits, "a full log's login burst: every message fits");

        // round trip
        Log some;
        some.Set(33, 0, 123456);
        some.Set(33, 1, 2);
        some.SetCounter(33, 0, 1);
        some.SetCounter(33, 1, 2);
        some.SetCounter(33, 2, 300);
        some.SetCounter(33, 3, 65535);
        some.Set(33, 4, 1767225600u);
        State rt;
        auto rtLines = Diff(rt, some.Extension());
        auto const d = Decode(rtLines.at(1));
        check(d && !d->clear && d->line.slot == 33, "the slot reads back");
        check(d && d->line.words == some.Extension()[33 - kFieldSlots], "every word reads back");
        auto const c = Decode(rtLines.at(0));
        check(c && c->clear, "CLR reads back");

        // malformed
        check(!Decode(""), "empty");
        check(!Decode("2;0;CLR"), "another version");
        check(!Decode("1;0;CLR;x"), "CLR with a field");
        check(!Decode("1;0;SLOT;24;1;0;0;0;0;0;0"), "slot 24 is a field slot");
        check(!Decode("1;0;SLOT;50;1;0;0;0;0;0;0"), "slot 50 does not exist");
        check(!Decode("1;0;SLOT;30;1;0;65536;0;0;0;0"), "a counter past 16 bits");
        check(!Decode("1;0;SLOT;30;1;0;0;0;0;0"), "a field short");
        check(!Decode("1;0;SLOT;30;1;0;0;0;0;0;0;0"), "a field too many");
        check(!Decode("1;0;SLOT;30;-1;0;0;0;0;0;0"), "a sign");
        check(!Decode("1;0;SLOT;30;4294967296;0;0;0;0;0;0"), "a quest past 32 bits");
        check(!Decode("1;0;NOPE"), "another command");
        check(!Decode(std::string(300, '1')), "too long");
    }
}

int main()
{
    Layout();
    Channel();
    std::cout << "questlog_tests: " << g_checks - g_failed << "/" << g_checks << " passed\n";
    return g_failed ? 1 : 0;
}
