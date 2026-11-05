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

// Standalone tests of the mini-encounter framework (tests/run_tests.sh builds and runs them): the EFN frame
// (src/encounters/EfEncounterWire.h, shared with the client) with round trips, the 255-byte rule and a decode fuzz,
// the throttle, and EfEncounterLogic.h's item groups, book (one encounter per player) and message log.

#include "EfEncounterLogic.h"
#include "EfEncounterWire.h"

#include <iostream>
#include <random>
#include <string>

using namespace Emberfall::Encounters;

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

    void TestFrame()
    {
        efenc::Message m;
        m.seq = 42;
        m.kind = efenc::kFishing;
        m.cmd = "END";
        m.fields = {"7", "L", "30000", "", "1/2/3"};
        std::string const body = efenc::Encode(m);
        check(body == "1;42;F;END;7;L;30000;;1/2/3", "encode: " + body);
        auto back = efenc::Decode(body);
        check(back && back->seq == 42 && back->kind == 'F' && back->cmd == "END" && back->fields.size() == 5 &&
                  back->fields[3].empty() && back->fields[4] == "1/2/3",
              "decode round trip");
        check(!efenc::Decode("2;1;F;HELLO"), "another version is refused");
        check(!efenc::Decode("1;x;F;HELLO"), "a bad sequence number is refused");
        check(!efenc::Decode("1;1;FF;HELLO"), "a kind is one letter");
        check(!efenc::Decode("1;1;F;"), "a command is needed");
        check(!efenc::Decode("1;1;F"), "four parts at least");
        check(efenc::Decode("1;1;T;HELLO").has_value(), "the taming kind decodes too");

        m.fields = {"a;b"};
        check(efenc::Encode(m).empty(), "a field with ';' is not encoded");
        m.fields = {std::string(300, 'x')};
        check(efenc::Encode(m).empty(), "over 255 bytes is not encoded");
        m.fields = {std::string(255 - 4 - std::string("1;42;F;END;").size(), 'x')};
        check(efenc::Encode(m).size() + 4 == 255, "exactly 255 bytes is encoded");
        check(!efenc::Decode(std::string(260, '1')), "an over-long body is refused");

        check(efenc::Hex(0x1fffffd000007ull) == "1fffffd000007", "hex");
        check(efenc::ParseUint<std::uint64_t>("f110000000001234", 16) == 0xf110000000001234ull, "parse hex");
        check(!efenc::ParseUint<std::uint32_t>("12x"), "parse refuses trailing junk");
        check(!efenc::ParseUint<std::uint32_t>(""), "parse refuses empty");

        // fuzz: random bodies never crash and every decoded message re-encodes to itself
        std::mt19937 rng(1234);
        std::string const alphabet = "0123456789;FTLENDOPHEL/:x-\t ";
        int decoded = 0;
        for (int i = 0; i < 20000; ++i)
        {
            std::string s;
            std::size_t const n = rng() % 60;
            for (std::size_t k = 0; k < n; ++k)
                s += alphabet[rng() % alphabet.size()];
            if (i % 3 == 0)
                s = "1;" + std::to_string(rng() % 100) + ";F;" + s;
            if (auto d = efenc::Decode(s))
            {
                ++decoded;
                std::string const again = efenc::Encode(*d);
                auto d2 = efenc::Decode(again);
                check(d2 && d2->cmd == d->cmd && d2->fields == d->fields && d2->seq == d->seq, "fuzz re-encode: " + s);
            }
        }
        check(decoded > 100, "the fuzz decodes some");
    }

    void TestThrottle()
    {
        efenc::Throttle t;
        int allowed = 0;
        for (int i = 0; i < 30; ++i)
            allowed += t.Allow(1000) ? 1 : 0;
        check(allowed == 20, "20 a second");
        check(!t.Allow(1999), "still the same second");
        check(t.Allow(2000), "a new second");
        efenc::Throttle five;
        int n = 0;
        for (int i = 0; i < 10; ++i)
            n += five.Allow(5000, 5) ? 1 : 0;
        check(n == 5, "a lower limit");
    }

    void TestGroups()
    {
        ItemGroups g;
        g.Add("live_bait", 99276, 3);
        g.Add("live_bait", 99272, 3);
        g.Add("raw_fish", 99272, 3);
        check(g.Has("live_bait", 99276) && g.Has("raw_fish", 99272), "membership");
        check(!g.Has("live_bait", 99277) && !g.Has("nothing", 99276), "not a member");
        check(g.Band("live_bait", 99272) == std::optional<std::uint8_t>(3), "band");
        check(!g.Band("raw_fish", 99276), "no band outside the group");
        check(g.Groups() == 2 && g.Rows() == 3, "counts");
        g.Clear();
        check(g.Rows() == 0, "clear");
    }

    void TestBook()
    {
        Book b;
        auto a = b.Start(1, 'F', 0xAA, 100);
        check(a && a->id != 0 && a->kind == 'F', "start");
        check(!b.Start(1, 'T', 0xBB, 200), "one encounter per player");
        auto c = b.Start(2, 'F', 0xCC, 300);
        check(c && c->id != a->id, "another player's own id");
        check(b.BySubject(0xCC) == std::optional<std::uint64_t>(2), "by subject");
        check(!b.End(1, a->id + 100), "the wrong id ends nothing");
        check(b.End(1, a->id) && !b.Of(1), "end");
        check(b.Start(1, 'T', 0xBB, 400).has_value(), "a new one after the end");
        check(b.End(2) && b.Size() == 1, "end whichever");
    }

    void TestLog()
    {
        MessageLog log;
        for (int i = 0; i < 15; ++i)
            log.Add(7, i % 2 == 0, "m" + std::to_string(i));
        auto q = log.Of(7);
        check(q.size() == MessageLog::kKeep && q.front() == "> m5" && q.back() == "< m14", "the last ten");
        log.Forget(7);
        check(log.Of(7).empty(), "forget");
    }
}

int main()
{
    TestFrame();
    TestThrottle();
    TestGroups();
    TestBook();
    TestLog();
    if (g_failed)
    {
        std::cerr << "encounter_tests: " << g_failed << " of " << g_checks << " checks failed\n";
        return 1;
    }
    std::cout << "encounter_tests: " << g_checks << " checks passed\n";
    return 0;
}
