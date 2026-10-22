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

// Standalone tests of account friends (src/accountfriends/EfAccountFriendsLogic.h; tests/run_tests.sh builds and runs
// them): the EFF bodies and their size, labels, what an ADD comes to (ignores never told), the rate limit and the
// offline debounce of a character switch.

#include "EfAccountFriendsLogic.h"

#include <iostream>
#include <string>

using namespace Emberfall::AccountFriends;

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

    template <typename A, typename B>
    void eq(A const& a, B const& b, std::string const& what)
    {
        bool const ok = a == b;
        if (!ok)
            std::cerr << "  got [" << a << "] want [" << b << "]\n";
        check(ok, what);
    }

    void TestBodies()
    {
        FriendRow on;
        on.id = 7;
        on.label = "Brin";
        on.online = true;
        on.character = "Corvina";
        on.level = 23;
        on.cls = 8;
        on.area = 90012;
        eq(FriendBody(on, "on"), std::string("1;0;F;7;on;Brin;Corvina;23;8;90012;0"), "an online row");
        FriendRow off;
        off.id = 7;
        off.label = "Brin; the bold";
        off.seenAgo = 3600;
        eq(FriendBody(off), std::string("1;0;F;7;-;Brin%3B the bold;;0;0;0;3600"), "an offline row: no character, the label encoded");
        eq(RequestBody(12, true, "Aelin", 5, true), std::string("1;0;REQ;12;in;Aelin;5;new"), "an incoming request");
        eq(RequestBody(12, false, "Aelin", 0, false), std::string("1;0;REQ;12;out;Aelin;0;-"), "an outgoing request");
        eq(GoneBody(7), std::string("1;0;FGONE;7"), "FGONE");
        eq(RequestGoneBody(12), std::string("1;0;RGONE;12"), "RGONE");
        eq(AllBody(2, 1), std::string("1;0;ALL;2;1"), "ALL");
        eq(OkBody(4, "SENT"), std::string("1;0;OK;4;SENT"), "OK with a detail");
        eq(ErrBody(4, "NOTFOUND", "Nobody"), std::string("1;0;ERR;4;NOTFOUND;Nobody"), "ERR");

        // The longest row fits a message: a label of 20 two-byte letters (the most a label may encode to) and a
        // 12-character name of them.
        FriendRow big;
        big.id = 4000000000u;
        big.online = true;
        for (int i = 0; i < 20; ++i)
            big.label += "\xC3\xA9";
        for (int i = 0; i < 12; ++i)
            big.character += "\xC3\xA9";
        big.level = 60;
        big.cls = 11;
        big.area = 4294967295u;
        check(ParseLabel(ef::eft::encodeName(big.label)).has_value(), "20 accented letters are a label");
        check(!ParseLabel(ef::eft::encodeName(big.label + "\xC3\xA9")), "21 are too long on the wire");
        check(FriendBody(big, "new").size() <= ef::eft::kMaxBody, "the longest row fits 251 bytes");
        check(RequestBody(4000000000u, true, big.character, 4000000000u, true).size() <= ef::eft::kMaxBody, "the longest request fits");
    }

    void TestLabels()
    {
        eq(ParseLabel("Brin").value_or("?"), std::string("Brin"), "a plain label");
        eq(ParseLabel("  Brin  ").value_or("?"), std::string("Brin"), "spaces cut");
        eq(ParseLabel("Brin%3B%20Co").value_or("?"), std::string("Brin; Co"), "decoded");
        check(!ParseLabel(""), "empty: no");
        check(!ParseLabel("   "), "only spaces: no");
        check(!ParseLabel("a%0Ab"), "a control character: no");
        check(!ParseLabel("abcdefghijklmnopqrstuvwxy"), "25 characters: no");
        check(ParseLabel("abcdefghijklmnopqrstuvwx").has_value(), "24 characters: yes");
        check(!ParseLabel("%ZZ"), "a broken escape: no");
    }

    AddFacts Base()
    {
        AddFacts f;
        f.self = 1;
        f.target = 2;
        return f;
    }

    void TestAdd()
    {
        check(DecideAdd(Base()).outcome == AddOutcome::Request, "a plain request");
        AddFacts f = Base();
        f.target = 0;
        eq(DecideAdd(f).code, std::string("NOTFOUND"), "no such character");
        f = Base();
        f.target = 1;
        eq(DecideAdd(f).code, std::string("SELF"), "one of our own characters");
        f = Base();
        f.friends = true;
        eq(DecideAdd(f).code, std::string("ALREADY"), "already friends");
        f = Base();
        f.outgoing = true;
        eq(DecideAdd(f).code, std::string("PENDING"), "asked already");
        f = Base();
        f.reverse = true;
        f.ignored = true;
        check(DecideAdd(f).outcome == AddOutcome::Link, "they asked us first: linked (their own request, so no ignore applies)");
        f = Base();
        f.ignored = true;
        check(DecideAdd(f).outcome == AddOutcome::Silent, "the target ignores us: answered as sent, nothing stored");
        f.theirIncoming = kMaxIncoming;
        check(DecideAdd(f).outcome == AddOutcome::Silent, "an ignore is not told even when their requests are full");
        f = Base();
        f.myOutgoing = kMaxOutgoing;
        eq(DecideAdd(f).code, std::string("LIMIT"), "too many waiting requests");
        f = Base();
        f.myFriends = kMaxFriends;
        eq(DecideAdd(f).code, std::string("FULL"), "our list is full");
        f = Base();
        f.theirIncoming = kMaxIncoming;
        eq(DecideAdd(f).code, std::string("THEIRFULL"), "their requests are full");
        f = Base();
        f.reverse = true;
        f.theirFriends = kMaxFriends;
        eq(DecideAdd(f).code, std::string("THEIRFULL"), "linking to a full list");
        f = Base();
        f.rateOk = false;
        f.target = 0;
        eq(DecideAdd(f).code, std::string("RATE"), "the rate limit first (no probing names past it)");
    }

    void TestRate()
    {
        RateLimit r;
        for (uint32_t i = 0; i < kAddsPerWindow; ++i)
            check(r.Allow(1000 + i), "within the limit");
        check(!r.Allow(2000), "one more in the window: no");
        check(!r.Allow(1000 + kAddWindowMs - 1), "still in the window: no");
        check(r.Allow(1000 + kAddWindowMs), "the first one aged out: yes");
    }

    void TestOffline()
    {
        OfflineDebounce d;
        d.Logout(5, 1000);
        check(d.Due(1000 + kOfflineAfterMs - 1).empty(), "not offline yet");
        check(d.Login(5), "a character switch: back in time");
        check(d.Due(1000 + kOfflineAfterMs).empty(), "a switch never reads as offline");
        d.Logout(5, 2000);
        d.Logout(6, 2500);
        auto due = d.Due(2000 + kOfflineAfterMs);
        check(due.size() == 1 && due[0] == 5, "the first logout is due");
        due = d.Due(2500 + kOfflineAfterMs);
        check(due.size() == 1 && due[0] == 6, "then the second");
        check(!d.Login(6), "a login after going offline: nothing pending");
        check(Ordered(9, 3).lo == 3 && Ordered(9, 3).hi == 9 && Ordered(3, 9).lo == 3, "the pair is ordered");
    }
}

int main()
{
    TestBodies();
    TestLabels();
    TestAdd();
    TestRate();
    TestOffline();
    std::cout << "account friend tests: " << (g_checks - g_failed) << "/" << g_checks << " passed\n";
    return g_failed ? 1 : 0;
}
