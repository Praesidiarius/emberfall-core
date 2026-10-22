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

// Account friends without the core (Emberfall docs/design/account-friends.md): the EFF bodies, the decision an ADD
// request comes to, label checks, the request rate limit and the offline debounce. src/emberfall_accountfriends.cpp
// does the database, the sessions and the sending; tests/accountfriends_tests.cpp checks this file.
//
// Protocol (an addon whisper between the player and themselves, prefix "EFF\t"; the EFT body rules of
// talents/EftProtocol.h: `<ver>;<seq>;<cmd>;<fields>`, printable ASCII, at most 251 bytes, names percent-encoded):
//   client -> server  1;<seq>;HELLO   1;<seq>;LIST          the whole list (HELLO once per world entry, LIST to refresh)
//                     1;<seq>;ADD;<character name>          a request to that character's account
//                     1;<seq>;ACCEPT;<request id>   1;<seq>;DECLINE;<request id>   (DECLINE also withdraws our own)
//                     1;<seq>;DEL;<friend id>       1;<seq>;LABEL;<friend id>;<label>
//   server -> client  1;0;ALL;<friends>;<requests>          a full list follows: forget the old one
//                     1;0;F;<id>;<ev>;<label>;<character>;<level>;<class>;<area>;<seconds since last seen>
//                         ev: - (a list row), on (came online or switched characters), off (went offline), new (accepted)
//                         offline: character empty, level/class/area 0; last seen 0 when unknown
//                     1;0;FGONE;<id>                         no longer friends (either side removed it)
//                     1;0;REQ;<id>;<in|out>;<character>;<seconds since sent>;<ev - | new>
//                     1;0;RGONE;<id>                         a request answered, withdrawn or expired
//                     1;0;OK;<seq>[;<detail>]   1;0;ERR;<seq>;<code>[;<detail>]
// Friend ids are the link's row id (one row per pair, both sides see the same id); request ids the request's row id.
// Account ids, login names and e-mail addresses never go on the wire.

#pragma once

#include "../talents/EftProtocol.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Emberfall::AccountFriends
{
    constexpr std::string_view kPrefix = "EFF\t";

    constexpr uint32_t kMaxFriends = 100;            // accepted account friends per account
    constexpr uint32_t kMaxOutgoing = 20;            // requests an account may have waiting
    constexpr uint32_t kMaxIncoming = 50;            // requests an account may be sent and not have answered
    constexpr uint32_t kAddsPerWindow = 10;          // ADDs (answered or not) per account ...
    constexpr uint64_t kAddWindowMs = 10 * 60 * 1000; // ... in ten minutes
    constexpr uint32_t kListMinGapMs = 2000;         // LIST more often than this answers OK and sends nothing
    constexpr uint32_t kRequestDays = 30;            // an unanswered request goes after this many days
    constexpr uint64_t kOfflineAfterMs = 5000;       // a logout is "offline" only when no character came back in time
    constexpr size_t kMaxLabelChars = 24;
    constexpr size_t kMaxLabelWire = 120;            // a label's encoded size (an accented letter takes 6: 20 of them)

    // ---- bodies --------------------------------------------------------------------------------------------------
    struct FriendRow
    {
        uint32_t id = 0;
        std::string label;
        bool online = false;
        std::string character;
        uint32_t level = 0, cls = 0, area = 0;
        uint32_t seenAgo = 0; // seconds since the last logout (offline), 0 unknown
    };

    inline std::string FriendBody(FriendRow const& r, std::string_view ev = "-")
    {
        if (!r.online)
            return ef::eft::body(0, "F", { std::to_string(r.id), std::string(ev), ef::eft::encodeName(r.label), "", "0", "0", "0",
                                           std::to_string(r.seenAgo) });
        return ef::eft::body(0, "F", { std::to_string(r.id), std::string(ev), ef::eft::encodeName(r.label),
                                       ef::eft::encodeName(r.character), std::to_string(r.level), std::to_string(r.cls),
                                       std::to_string(r.area), "0" });
    }

    inline std::string GoneBody(uint32_t id) { return ef::eft::body(0, "FGONE", { std::to_string(id) }); }

    inline std::string RequestBody(uint32_t id, bool incoming, std::string_view name, uint32_t ago, bool fresh)
    {
        return ef::eft::body(0, "REQ", { std::to_string(id), incoming ? "in" : "out", ef::eft::encodeName(name), std::to_string(ago),
                                         fresh ? "new" : "-" });
    }

    inline std::string RequestGoneBody(uint32_t id) { return ef::eft::body(0, "RGONE", { std::to_string(id) }); }
    inline std::string AllBody(size_t friends, size_t requests)
    {
        return ef::eft::body(0, "ALL", { std::to_string(friends), std::to_string(requests) });
    }
    inline std::string OkBody(uint32_t seq, std::string_view detail = {})
    {
        if (detail.empty())
            return ef::eft::body(0, "OK", { std::to_string(seq) });
        return ef::eft::body(0, "OK", { std::to_string(seq), std::string(detail) });
    }
    inline std::string ErrBody(uint32_t seq, std::string_view code, std::string_view detail = {})
    {
        if (detail.empty())
            return ef::eft::body(0, "ERR", { std::to_string(seq), std::string(code) });
        return ef::eft::body(0, "ERR", { std::to_string(seq), std::string(code), std::string(detail) });
    }

    // ---- labels --------------------------------------------------------------------------------------------------
    // A label the player sets: decoded from the wire, 1-24 characters (at most kMaxLabelWire encoded: fewer when they
    // are accented), no control characters, the leading and trailing spaces cut. Nothing when it does not pass.
    inline std::optional<std::string> ParseLabel(std::string_view wire)
    {
        if (wire.size() > kMaxLabelWire)
            return std::nullopt;
        std::optional<std::string> text = ef::eft::decodeName(wire);
        if (!text)
            return std::nullopt;
        size_t const first = text->find_first_not_of(' ');
        if (first == std::string::npos)
            return std::nullopt;
        size_t const last = text->find_last_not_of(' ');
        std::string out = text->substr(first, last - first + 1);
        if (!ef::eft::validName(out) || ef::eft::encodeName(out).size() > kMaxLabelWire)
            return std::nullopt;
        return out;
    }

    // ---- an ADD ------------------------------------------------------------------------------------------------
    struct AddFacts
    {
        uint32_t self = 0;          // the sender's account
        uint32_t target = 0;        // the named character's account, 0 when no such character
        bool friends = false;       // already linked
        bool outgoing = false;      // the sender asked this account already
        bool reverse = false;       // this account asked the sender: the ADD accepts it
        bool ignored = false;       // one of the target's characters ignores one of the sender's
        bool rateOk = true;         // the sender's ADD rate limit allows it
        uint32_t myFriends = 0, myOutgoing = 0;
        uint32_t theirFriends = 0, theirIncoming = 0;
    };

    enum class AddOutcome
    {
        Request,  // store a request and tell the target
        Link,     // the target asked us before: linked at once
        Silent,   // answered as sent, nothing stored (the target ignores the sender)
        Error,    // `code` says why
    };

    struct AddDecision
    {
        AddOutcome outcome = AddOutcome::Error;
        std::string code; // NOTFOUND SELF ALREADY PENDING RATE LIMIT FULL THEIRFULL
    };

    inline AddDecision DecideAdd(AddFacts const& f)
    {
        auto err = [](char const* code) { return AddDecision{ AddOutcome::Error, code }; };
        if (!f.rateOk)
            return err("RATE");
        if (!f.target)
            return err("NOTFOUND");
        if (f.target == f.self)
            return err("SELF");
        if (f.friends)
            return err("ALREADY");
        if (f.reverse)
        {
            if (f.myFriends >= kMaxFriends)
                return err("FULL");
            if (f.theirFriends >= kMaxFriends)
                return err("THEIRFULL");
            return { AddOutcome::Link, {} };
        }
        if (f.outgoing)
            return err("PENDING");
        if (f.myFriends >= kMaxFriends)
            return err("FULL");
        if (f.myOutgoing >= kMaxOutgoing)
            return err("LIMIT");
        // An ignore is never told: the sender sees the same answer as for a request that was stored.
        if (f.ignored)
            return { AddOutcome::Silent, {} };
        if (f.theirFriends >= kMaxFriends || f.theirIncoming >= kMaxIncoming)
            return err("THEIRFULL");
        return { AddOutcome::Request, {} };
    }

    // ---- the rate limit ------------------------------------------------------------------------------------------
    // At most `max` events in any `windowMs`: Allow() counts the event when it passes.
    struct RateLimit
    {
        std::deque<uint64_t> stamps;
        bool Allow(uint64_t nowMs, uint32_t max = kAddsPerWindow, uint64_t windowMs = kAddWindowMs)
        {
            while (!stamps.empty() && nowMs - stamps.front() >= windowMs)
                stamps.pop_front();
            if (stamps.size() >= max)
                return false;
            stamps.push_back(nowMs);
            return true;
        }
    };

    // ---- going offline -------------------------------------------------------------------------------------------
    // A logout starts the clock; a login of the same account stops it (a character switch reads as "now playing
    // <the new one>", never as offline and online again). Due() hands out the accounts whose time ran out.
    struct OfflineDebounce
    {
        std::map<uint32_t, uint64_t> leaving; // account -> logout ms
        void Logout(uint32_t account, uint64_t nowMs) { leaving[account] = nowMs; }
        bool Login(uint32_t account) { return leaving.erase(account) != 0; }
        std::vector<uint32_t> Due(uint64_t nowMs, uint64_t afterMs = kOfflineAfterMs)
        {
            std::vector<uint32_t> out;
            for (auto it = leaving.begin(); it != leaving.end();)
                if (nowMs - it->second >= afterMs)
                {
                    out.push_back(it->first);
                    it = leaving.erase(it);
                }
                else
                    ++it;
            return out;
        }
    };

    // The stored pair: the smaller account id first. `label_lo` is what account_lo calls account_hi.
    struct Pair
    {
        uint32_t lo = 0, hi = 0;
    };
    inline Pair Ordered(uint32_t a, uint32_t b) { return a < b ? Pair{ a, b } : Pair{ b, a }; }
}
