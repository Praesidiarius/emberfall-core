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

// Account friends (Emberfall docs/design/account-friends.md): friends by account, next to the stock character friends
// (character_social, untouched). A request goes to a character's account; once accepted, each side sees which
// character the other plays, wherever they log in with it. src/accountfriends/EfAccountFriendsLogic.h holds the EFF
// bodies and the rules; this file keeps the links of the accounts in the world, answers the client and tells online
// friends when an account comes, switches characters or goes.
//
// Storage: the auth database (data/sql/db-auth/mod_emberfall_2026_10_22_00_account_friends.sql): ef_account_friend
// (one row per pair), ef_account_friend_request, ef_account_seen. Presence is this worldserver's: a friend playing on
// another realm shows as offline.
//
// Threads: the hooks used here (login, logout, chat whispers, the world update) run on the world thread; one mutex
// guards the state anyway.
//
// Config: Emberfall.AccountFriends.Enable (1; AC_EMBERFALL_ACCOUNTFRIENDS_ENABLE). Off: HELLO answers ERR OFF.

#include "CharacterCache.h"
#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "SocialMgr.h"
#include "StringFormat.h"
#include "WorldPacket.h"
#include "WorldScript.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include "accountfriends/EfAccountFriendsLogic.h"

#include <ctime>
#include <mutex>
#include <unordered_map>

namespace AF = Emberfall::AccountFriends;

namespace
{
    bool s_enable = true;

    // One side of a link: the other account and what this side calls it.
    struct Link
    {
        uint32 id = 0;
        uint32 other = 0;
        std::string label;
    };

    std::mutex s_lock;
    std::unordered_map<uint32, std::vector<Link>> s_links; // account in the world -> its links
    std::unordered_map<uint32, uint32> s_seen;             // account -> unix time of its last logout
    std::unordered_map<uint32, AF::RateLimit> s_adds;      // account -> its ADDs
    std::unordered_map<uint32, uint64> s_lastList;         // account -> ms of its last LIST
    AF::OfflineDebounce s_offline;
    uint32 s_tickMs = 0;

    uint64 NowMs() { return uint64(GameTime::GetGameTimeMS().count()); }
    uint32 NowUnix() { return uint32(GameTime::GetGameTime().count()); }

    std::string Escaped(std::string text)
    {
        LoginDatabase.EscapeString(text);
        return text;
    }

    void Send(Player* player, std::string const& body)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, std::string(AF::kPrefix) + body);
        player->SendDirectMessage(&data);
    }

    // The account's character in this world (logging in or out, or between characters: none).
    Player* Playing(uint32 account)
    {
        WorldSession* s = sWorldSessionMgr->FindSession(account);
        if (!s || s->PlayerLoading() || s->PlayerLogout())
            return nullptr;
        return s->GetPlayer();
    }

    // ---- the links ------------------------------------------------------------------------------------------------
    std::vector<Link> LoadLinks(uint32 account)
    {
        std::vector<Link> out;
        if (QueryResult r = LoginDatabase.Query("SELECT `id`, `account_lo`, `account_hi`, `label_lo`, `label_hi` FROM `ef_account_friend` "
                                                "WHERE `account_lo` = {} OR `account_hi` = {}", account, account))
            do
            {
                Field* f = r->Fetch();
                Link l;
                l.id = f[0].Get<uint32>();
                bool const lo = f[1].Get<uint32>() == account;
                l.other = lo ? f[2].Get<uint32>() : f[1].Get<uint32>();
                l.label = lo ? f[3].Get<std::string>() : f[4].Get<std::string>();
                out.push_back(std::move(l));
            } while (r->NextRow());
        return out;
    }

    std::vector<Link>& LinksOf(uint32 account)
    {
        auto it = s_links.find(account);
        if (it == s_links.end())
            it = s_links.emplace(account, LoadLinks(account)).first;
        return it->second;
    }

    Link const* FindLink(uint32 account, uint32 id)
    {
        for (Link const& l : LinksOf(account))
            if (l.id == id)
                return &l;
        return nullptr;
    }

    // An account whose links are cached (in the world): the change; otherwise the next login loads it.
    void CacheAdd(uint32 account, Link const& l)
    {
        if (auto it = s_links.find(account); it != s_links.end())
        {
            std::erase_if(it->second, [&](Link const& o) { return o.id == l.id; });
            it->second.push_back(l);
        }
    }
    void CacheRemove(uint32 account, uint32 id)
    {
        if (auto it = s_links.find(account); it != s_links.end())
            std::erase_if(it->second, [&](Link const& o) { return o.id == id; });
    }

    uint32 SeenAgo(uint32 account)
    {
        auto it = s_seen.find(account);
        uint32 const now = NowUnix();
        return it == s_seen.end() || !it->second || it->second > now ? 0 : now - it->second;
    }

    AF::FriendRow RowFor(Link const& l)
    {
        AF::FriendRow row;
        row.id = l.id;
        row.label = l.label;
        if (Player* p = Playing(l.other))
        {
            row.online = true;
            row.character = p->GetName();
            row.level = p->GetLevel();
            row.cls = p->getClass();
            row.area = p->GetZoneId();
        }
        else
            row.seenAgo = SeenAgo(l.other);
        return row;
    }

    // Tells every friend of `account` who is in the world its row (ev on, off).
    void TellFriends(uint32 account, std::string_view ev)
    {
        for (Link const& l : LinksOf(account))
        {
            Player* watcher = Playing(l.other);
            if (!watcher)
                continue;
            // The watcher's own side of the link (its label for us).
            if (Link const* mine = FindLink(l.other, l.id))
                Send(watcher, AF::FriendBody(RowFor(Link{ mine->id, account, mine->label }), ev));
        }
    }

    // ---- requests -------------------------------------------------------------------------------------------------
    struct Request
    {
        uint32 id = 0, from = 0, to = 0;
        std::string fromName, toName;
        uint32 age = 0;
    };

    std::vector<Request> LoadRequests(std::string const& where)
    {
        std::vector<Request> out;
        if (QueryResult r = LoginDatabase.Query("SELECT `id`, `from_account`, `to_account`, `from_name`, `to_name`, "
                                                "CAST(GREATEST(0, UNIX_TIMESTAMP() - UNIX_TIMESTAMP(`created_at`)) AS UNSIGNED) FROM `ef_account_friend_request` WHERE " + where))
            do
            {
                Field* f = r->Fetch();
                out.push_back({ f[0].Get<uint32>(), f[1].Get<uint32>(), f[2].Get<uint32>(), f[3].Get<std::string>(), f[4].Get<std::string>(),
                                uint32(f[5].Get<uint64>()) });
            } while (r->NextRow());
        return out;
    }

    uint32 Count(std::string const& sql)
    {
        if (QueryResult r = LoginDatabase.Query(sql))
            return uint32(r->Fetch()[0].Get<uint64>());
        return 0;
    }

    // Does any character of `by` ignore any character of `whom` (on this realm)?
    bool Ignores(uint32 by, uint32 whom)
    {
        return bool(CharacterDatabase.Query("SELECT 1 FROM `character_social` s JOIN `characters` a ON a.`guid` = s.`guid` "
                                            "JOIN `characters` b ON b.`guid` = s.`friend` WHERE (s.`flags` & {}) <> 0 AND a.`account` = {} "
                                            "AND b.`account` = {} LIMIT 1", uint32(SOCIAL_FLAG_IGNORED), by, whom));
    }

    // Links two accounts (the request, if any, gone): each side's label, the new id; 0 when it failed.
    uint32 MakeLink(uint32 a, std::string const& aCallsB, uint32 b, std::string const& bCallsA)
    {
        AF::Pair const p = AF::Ordered(a, b);
        std::string const labelLo = p.lo == a ? aCallsB : bCallsA, labelHi = p.lo == a ? bCallsA : aCallsB;
        LoginDatabase.DirectExecute("INSERT IGNORE INTO `ef_account_friend` (`account_lo`, `account_hi`, `label_lo`, `label_hi`) VALUES ({}, {}, '{}', '{}')",
                                    p.lo, p.hi, Escaped(labelLo), Escaped(labelHi));
        LoginDatabase.DirectExecute("DELETE FROM `ef_account_friend_request` WHERE (`from_account` = {} AND `to_account` = {}) OR "
                                    "(`from_account` = {} AND `to_account` = {})", a, b, b, a);
        QueryResult r = LoginDatabase.Query("SELECT `id` FROM `ef_account_friend` WHERE `account_lo` = {} AND `account_hi` = {}", p.lo, p.hi);
        if (!r)
            return 0;
        uint32 const id = r->Fetch()[0].Get<uint32>();
        CacheAdd(a, { id, b, aCallsB });
        CacheAdd(b, { id, a, bCallsA });
        return id;
    }

    // A new link: both sides' rows (ev new) and the request gone, to whoever is in the world.
    void TellLinked(uint32 linkId, uint32 requestId, uint32 a, uint32 b)
    {
        for (uint32 const acc : { a, b })
            if (Player* p = Playing(acc))
            {
                if (requestId)
                    Send(p, AF::RequestGoneBody(requestId));
                if (Link const* l = FindLink(acc, linkId))
                    Send(p, AF::FriendBody(RowFor(*l), "new"));
            }
    }

    // ---- the client -----------------------------------------------------------------------------------------------
    void SendAll(Player* player, uint32 me)
    {
        s_links[me] = LoadLinks(me);
        std::vector<Link> const& links = s_links[me];
        if (!links.empty())
        {
            std::string ids;
            for (Link const& l : links)
                ids += (ids.empty() ? "" : ",") + std::to_string(l.other);
            if (QueryResult r = LoginDatabase.Query("SELECT `account_id`, `seen_at` FROM `ef_account_seen` WHERE `account_id` IN (" + ids + ")"))
                do
                    s_seen[r->Fetch()[0].Get<uint32>()] = r->Fetch()[1].Get<uint32>();
                while (r->NextRow());
        }
        std::vector<Request> const reqs = LoadRequests(Acore::StringFormat("`from_account` = {} OR `to_account` = {}", me, me));
        Send(player, AF::AllBody(links.size(), reqs.size()));
        for (Link const& l : links)
            Send(player, AF::FriendBody(RowFor(l)));
        for (Request const& q : reqs)
        {
            bool const incoming = q.to == me;
            Send(player, AF::RequestBody(q.id, incoming, incoming ? q.fromName : q.toName, q.age, false));
        }
    }

    void HandleAdd(Player* player, uint32 me, uint32 seq, std::vector<std::string> const& fields)
    {
        std::optional<std::string> name = fields.empty() ? std::nullopt : ef::eft::decodeName(fields[0]);
        if (!name || name->empty() || name->size() > 48)
            return Send(player, AF::ErrBody(seq, "DECODE"));
        std::string typed = *name;
        AF::AddFacts f;
        f.self = me;
        f.rateOk = s_adds[me].Allow(NowMs());
        if (f.rateOk && normalizePlayerName(typed))
            if (ObjectGuid const guid = sCharacterCache->GetCharacterGuidByName(typed))
            {
                f.target = sCharacterCache->GetCharacterAccountIdByGuid(guid);
                if (CharacterCacheEntry const* e = sCharacterCache->GetCharacterCacheByGuid(guid))
                    typed = e->Name;
            }
        uint32 reverseId = 0;
        std::string reverseToName;
        if (f.rateOk && f.target && f.target != me)
        {
            AF::Pair const p = AF::Ordered(me, f.target);
            f.friends = Count(Acore::StringFormat("SELECT COUNT(*) FROM `ef_account_friend` WHERE `account_lo` = {} AND `account_hi` = {}", p.lo, p.hi)) > 0;
            for (Request const& q : LoadRequests(Acore::StringFormat("(`from_account` = {} AND `to_account` = {}) OR (`from_account` = {} AND `to_account` = {})",
                                                                     me, f.target, f.target, me)))
            {
                if (q.from == me)
                    f.outgoing = true;
                else
                {
                    f.reverse = true;
                    reverseId = q.id;
                    reverseToName = q.toName;
                }
            }
            f.ignored = Ignores(f.target, me);
            f.myFriends = Count(Acore::StringFormat("SELECT COUNT(*) FROM `ef_account_friend` WHERE `account_lo` = {} OR `account_hi` = {}", me, me));
            f.theirFriends = Count(Acore::StringFormat("SELECT COUNT(*) FROM `ef_account_friend` WHERE `account_lo` = {} OR `account_hi` = {}", f.target, f.target));
            f.myOutgoing = Count(Acore::StringFormat("SELECT COUNT(*) FROM `ef_account_friend_request` WHERE `from_account` = {}", me));
            f.theirIncoming = Count(Acore::StringFormat("SELECT COUNT(*) FROM `ef_account_friend_request` WHERE `to_account` = {}", f.target));
        }
        AF::AddDecision const d = AF::DecideAdd(f);
        std::string const wireName = ef::eft::encodeName(typed);
        switch (d.outcome)
        {
            case AF::AddOutcome::Error:
                return Send(player, AF::ErrBody(seq, d.code, wireName));
            case AF::AddOutcome::Silent:
                return Send(player, AF::OkBody(seq, "SENT"));
            case AF::AddOutcome::Link:
            {
                // They asked us first: their label for us is the character they asked, ours the one we named.
                uint32 const id = MakeLink(me, typed, f.target, reverseToName.empty() ? player->GetName() : reverseToName);
                if (!id)
                    return Send(player, AF::ErrBody(seq, "DB"));
                TellLinked(id, reverseId, me, f.target);
                return Send(player, AF::OkBody(seq, "LINKED"));
            }
            case AF::AddOutcome::Request:
            {
                LoginDatabase.DirectExecute("INSERT IGNORE INTO `ef_account_friend_request` (`from_account`, `to_account`, `from_name`, `to_name`) VALUES "
                                            "({}, {}, '{}', '{}')", me, f.target, Escaped(player->GetName()), Escaped(typed));
                std::vector<Request> const q = LoadRequests(Acore::StringFormat("`from_account` = {} AND `to_account` = {}", me, f.target));
                if (q.empty())
                    return Send(player, AF::ErrBody(seq, "DB"));
                Send(player, AF::RequestBody(q[0].id, false, typed, 0, false));
                if (Player* them = Playing(f.target))
                    Send(them, AF::RequestBody(q[0].id, true, player->GetName(), 0, true));
                return Send(player, AF::OkBody(seq, "SENT"));
            }
        }
    }

    void HandleAnswer(Player* player, uint32 me, uint32 seq, std::string const& cmd, std::vector<std::string> const& fields)
    {
        std::optional<uint32> const id = fields.empty() ? std::nullopt : ef::eft::number(fields[0]);
        if (!id)
            return Send(player, AF::ErrBody(seq, "DECODE"));
        std::vector<Request> const q = LoadRequests(Acore::StringFormat("`id` = {} AND (`to_account` = {} OR `from_account` = {})", *id, me, me));
        if (q.empty())
            return Send(player, AF::ErrBody(seq, "ID"));
        Request const& r = q[0];
        if (cmd == "ACCEPT")
        {
            if (r.to != me)
                return Send(player, AF::ErrBody(seq, "ID"));
            if (Count(Acore::StringFormat("SELECT COUNT(*) FROM `ef_account_friend` WHERE `account_lo` = {} OR `account_hi` = {}", me, me)) >= AF::kMaxFriends)
                return Send(player, AF::ErrBody(seq, "FULL"));
            if (Count(Acore::StringFormat("SELECT COUNT(*) FROM `ef_account_friend` WHERE `account_lo` = {} OR `account_hi` = {}", r.from, r.from)) >= AF::kMaxFriends)
                return Send(player, AF::ErrBody(seq, "THEIRFULL"));
            // The receiver calls the sender by the character it came from; the sender keeps the name it typed.
            uint32 const link = MakeLink(me, r.fromName, r.from, r.toName);
            if (!link)
                return Send(player, AF::ErrBody(seq, "DB"));
            TellLinked(link, r.id, me, r.from);
            return Send(player, AF::OkBody(seq, ef::eft::encodeName(r.fromName)));
        }
        // DECLINE: an incoming one declined, or our own withdrawn. The other side's list just loses it.
        LoginDatabase.DirectExecute("DELETE FROM `ef_account_friend_request` WHERE `id` = {}", r.id);
        Send(player, AF::RequestGoneBody(r.id));
        if (Player* other = Playing(r.to == me ? r.from : r.to))
            Send(other, AF::RequestGoneBody(r.id));
        Send(player, AF::OkBody(seq));
    }

    void HandleRequest(Player* player, std::string_view text)
    {
        ef::eft::ParseResult const parsed = ef::eft::parse(text);
        if (!parsed.ok())
            return Send(player, AF::ErrBody(parsed.seq, parsed.error == ef::eft::ParseError::Version ? "VERSION" : "DECODE"));
        ef::eft::Request const& rq = parsed.request;
        if (!s_enable)
            return Send(player, AF::ErrBody(rq.seq, "OFF"));
        uint32 const me = player->GetSession()->GetAccountId();
        std::lock_guard<std::mutex> guard(s_lock);
        if (rq.cmd == "HELLO" || rq.cmd == "LIST")
        {
            uint64 const now = NowMs();
            uint64& last = s_lastList[me];
            if (rq.cmd == "LIST" && last && now - last < AF::kListMinGapMs)
                return Send(player, AF::OkBody(rq.seq));
            last = now;
            SendAll(player, me);
            return Send(player, AF::OkBody(rq.seq));
        }
        if (rq.cmd == "ADD")
            return HandleAdd(player, me, rq.seq, rq.fields);
        if (rq.cmd == "ACCEPT" || rq.cmd == "DECLINE")
            return HandleAnswer(player, me, rq.seq, rq.cmd, rq.fields);
        if (rq.cmd == "DEL" || rq.cmd == "LABEL")
        {
            std::optional<uint32> const id = rq.fields.empty() ? std::nullopt : ef::eft::number(rq.fields[0]);
            Link const* l = id ? FindLink(me, *id) : nullptr;
            if (!l)
                return Send(player, AF::ErrBody(rq.seq, "ID"));
            if (rq.cmd == "DEL")
            {
                uint32 const other = l->other, linkId = l->id;
                LoginDatabase.DirectExecute("DELETE FROM `ef_account_friend` WHERE `id` = {}", linkId);
                CacheRemove(me, linkId);
                CacheRemove(other, linkId);
                Send(player, AF::GoneBody(linkId));
                if (Player* them = Playing(other))
                    Send(them, AF::GoneBody(linkId));
                return Send(player, AF::OkBody(rq.seq));
            }
            std::optional<std::string> const label = rq.fields.size() < 2 ? std::nullopt : AF::ParseLabel(rq.fields[1]);
            if (!label)
                return Send(player, AF::ErrBody(rq.seq, "LABEL"));
            AF::Pair const p = AF::Ordered(me, l->other);
            LoginDatabase.DirectExecute("UPDATE `ef_account_friend` SET `{}` = '{}' WHERE `id` = {}", p.lo == me ? "label_lo" : "label_hi",
                                        Escaped(*label), l->id);
            Link const updated{ l->id, l->other, *label };
            CacheAdd(me, updated);
            Send(player, AF::FriendBody(RowFor(updated)));
            return Send(player, AF::OkBody(rq.seq));
        }
        Send(player, AF::ErrBody(rq.seq, "CMD"));
    }

    // ---- scripts ----------------------------------------------------------------------------------------------------
    class emberfall_accountfriends_world : public WorldScript
    {
    public:
        emberfall_accountfriends_world()
            : WorldScript("emberfall_accountfriends_world", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED, WORLDHOOK_ON_UPDATE }) { }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            s_enable = sConfigMgr->GetOption<bool>("Emberfall.AccountFriends.Enable", true, false);
        }

        void OnBeforeWorldInitialized() override
        {
            if (!s_enable)
            {
                LOG_INFO("server.loading", ">> Emberfall account friends: off (Emberfall.AccountFriends.Enable = 0)");
                return;
            }
            LoginDatabase.DirectExecute("DELETE FROM `ef_account_friend_request` WHERE `created_at` < NOW() - INTERVAL {} DAY", AF::kRequestDays);
            LOG_INFO("server.loading", ">> Emberfall account friends: on (EFF; {} pairs, {} open requests)",
                     Count("SELECT COUNT(*) FROM `ef_account_friend`"), Count("SELECT COUNT(*) FROM `ef_account_friend_request`"));
        }

        // Accounts that logged out and did not come back with another character: offline for their friends.
        void OnUpdate(uint32 diff) override
        {
            if (!s_enable)
                return;
            s_tickMs += diff;
            if (s_tickMs < 1000)
                return;
            s_tickMs = 0;
            std::lock_guard<std::mutex> guard(s_lock);
            for (uint32 const account : s_offline.Due(NowMs()))
            {
                if (Playing(account))
                    continue;
                TellFriends(account, "off");
                s_links.erase(account);
                s_lastList.erase(account);
            }
        }
    };

    class emberfall_accountfriends_player : public PlayerScript
    {
    public:
        emberfall_accountfriends_player()
            : PlayerScript("emberfall_accountfriends_player", { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT }) { }

        // In the world: its friends in the world see it ("now online playing ...", also after a character switch).
        void OnPlayerLogin(Player* player) override
        {
            if (!s_enable)
                return;
            uint32 const me = player->GetSession()->GetAccountId();
            std::lock_guard<std::mutex> guard(s_lock);
            s_offline.Login(me);
            s_links[me] = LoadLinks(me);
            TellFriends(me, "on");
        }

        void OnPlayerLogout(Player* player) override
        {
            if (!s_enable)
                return;
            uint32 const me = player->GetSession()->GetAccountId();
            uint32 const now = NowUnix();
            std::lock_guard<std::mutex> guard(s_lock);
            s_seen[me] = now;
            LoginDatabase.Execute("REPLACE INTO `ef_account_seen` (`account_id`, `seen_at`) VALUES ({}, {})", me, now);
            s_offline.Logout(me, NowMs());
        }

        // The EFF protocol: every `EFF\t` addon whisper is consumed, only the player's own (to itself) answered.
        bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg, Player* receiver) override
        {
            if (type != CHAT_MSG_WHISPER || language != uint32(LANG_ADDON) || msg.compare(0, AF::kPrefix.size(), AF::kPrefix) != 0)
                return true;
            if (receiver == player)
                HandleRequest(player, std::string_view(msg).substr(AF::kPrefix.size()));
            return false;
        }
    };
}

void AddEmberfallAccountFriendsScripts()
{
    new emberfall_accountfriends_world();
    new emberfall_accountfriends_player();
}
