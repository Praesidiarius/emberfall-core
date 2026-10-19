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

// Emberfall's world editor (Emberfall docs/design/world-editor.md §6): the EFE protocol and the editor's rules as
// plain functions, with no dependency on the core, so tests/editor_tests.cpp runs them standalone (vectors and a decode
// fuzz). The transport is the talents' (EftProtocol.h): an addon whisper to oneself, `EFE\t<ver>;<seq>;<cmd>;...`.
//
//   C->S  HELLO;<data>  MODE;on|off  SCAN;<map>;<x>;<y>;<radius>  OPEN;<cs>  NEW;<title>
//         ADD;c;<entry>;<x>;<y>;<z>;<o>  MOVE;<k>;<id>;<x>;<y>;<z>;<o>  SET;c;<id>;wander|respawn;<value>
//         DEL;c;<id>  UNDO;<cs>  REDO;<cs>  SUBMIT;<cs>;<p>/<t>;<description>  WITHDRAW;<cs>
//   S->C  ROLE;<level>;<zones>;<limits>  MARK;<scan>;<p>/<t>;<entry|...>  DELTA;<entry>  GONE;<k>;<id>
//         CS;<id>;<state>;<ops>;<title>  OPS;<cs>;<p>/<t>;<op|...>  OK;<seq>;<id>  ERR;<seq>;<code>;<detail>
//
// A marker entry: k,id,entry,x,y,z,o,wander,respawn,flags,lowguid (k: c creature spawn, n NPC; flags letters, below).
// Everything here is bounds-checked and never throws: malformed input gives std::nullopt / an error value.

#pragma once

#include "../talents/EftProtocol.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ef::editor
{
    constexpr std::string_view kPrefix = "EFE\t";
    constexpr uint32_t kTempIdBase = 9000001;   // the realm's temporary spawn ids (§5.3), far outside every content range
    constexpr uint32_t kTempIdMax = 9999999;
    constexpr float kMaxScanRadius = 250.0f;
    constexpr float kPresence = 120.0f;         // the editor stands within this of what they edit (§6.3 step 6)
    constexpr float kGroundTolerance = 3.0f;    // a given Z may be this far from the ground (step 8)
    constexpr size_t kMaxTitle = 80, kMinTitle = 3, kMaxDescription = 1000;

    enum class Level : uint8_t { None = 0, Contributor = 1, Reviewer = 2, Maintainer = 3 };

    // The change set states (§5.2), as stored in build_editor.ef_changeset.state.
    namespace state
    {
        constexpr std::string_view Draft = "draft", Submitted = "submitted", InReview = "in_review",
            ChangesRequested = "changes_requested", Accepted = "accepted", Released = "released",
            Rejected = "rejected", Withdrawn = "withdrawn", Conflict = "conflict";
        inline bool open(std::string_view s) { return s == Draft || s == Submitted || s == InReview || s == ChangesRequested || s == Conflict; }
        inline bool editable(std::string_view s) { return s == Draft || s == ChangesRequested || s == Conflict; }
        // replayed on the realm (§7.1): accepted but not released, and every open set
        inline bool replayed(std::string_view s) { return s == Accepted || open(s); }
        inline bool known(std::string_view s)
        {
            return open(s) || s == Accepted || s == Released || s == Rejected || s == Withdrawn;
        }
    }

    // ------------------------------------------------------------------------------------------ numbers

    // A decimal number: optional '-', digits, optional '.digits'; at most 12 characters, finite, |v| <= max.
    inline std::optional<float> decimal(std::string_view text, float max = 100000.0f)
    {
        if (text.empty() || text.size() > 12)
            return std::nullopt;
        size_t i = 0;
        bool negative = false;
        if (text[0] == '-')
        {
            negative = true;
            i = 1;
        }
        double value = 0, scale = 0.1;
        bool digits = false, point = false;
        for (; i < text.size(); ++i)
        {
            char const c = text[i];
            if (c == '.')
            {
                if (point)
                    return std::nullopt;
                point = true;
                continue;
            }
            if (c < '0' || c > '9')
                return std::nullopt;
            digits = true;
            if (!point)
                value = value * 10 + (c - '0');
            else
            {
                value += (c - '0') * scale;
                scale /= 10;
            }
        }
        if (!digits)
            return std::nullopt;
        if (negative)
            value = -value;
        if (!std::isfinite(value) || std::fabs(value) > max)
            return std::nullopt;
        return static_cast<float>(value);
    }

    inline std::string fixed(double value, int digits)
    {
        char buffer[48];
        std::snprintf(buffer, sizeof buffer, "%.*f", digits, value);
        std::string out = buffer;
        if (out == "-0" || out.rfind("-0.", 0) == 0)
        {
            bool zero = true;
            for (char c : out.substr(1))
                if (c != '0' && c != '.')
                    zero = false;
            if (zero)
                out = out.substr(1);
        }
        return out;
    }

    inline float normaliseOrientation(float o)
    {
        constexpr float twoPi = 6.28318530717958647692f;
        o = std::fmod(o, twoPi);
        if (o < 0)
            o += twoPi;
        return o;
    }

    struct Pos
    {
        float x = 0, y = 0, z = 0, o = 0;
    };

    inline float dist2d(Pos const& a, Pos const& b) { return std::hypot(a.x - b.x, a.y - b.y); }
    inline float dist3d(Pos const& a, Pos const& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }

    // ------------------------------------------------------------------------------------------ requests

    enum class Kind : char { Creature = 'c', Npc = 'n' };

    inline std::optional<Kind> kindOf(std::string_view text)
    {
        if (text == "c")
            return Kind::Creature;
        if (text == "n")
            return Kind::Npc;
        return std::nullopt;
    }

    inline std::optional<Pos> position(std::vector<std::string> const& f, size_t at)
    {
        if (f.size() < at + 4)
            return std::nullopt;
        auto x = decimal(f[at], 20000), y = decimal(f[at + 1], 20000), z = decimal(f[at + 2], 5000), o = decimal(f[at + 3], 100);
        if (!x || !y || !z || !o)
            return std::nullopt;
        return Pos{*x, *y, *z, normaliseOrientation(*o)};
    }

    struct Scan { uint32_t map = 0; float x = 0, y = 0, radius = 0; };
    struct Add { Kind kind = Kind::Creature; uint32_t entry = 0; Pos pos; };
    struct Move { Kind kind = Kind::Creature; uint32_t id = 0; Pos pos; };
    enum class SetField : uint8_t { Wander, Respawn };
    struct Set { Kind kind = Kind::Creature; uint32_t id = 0; SetField field = SetField::Wander; float value = 0; };
    struct Del { Kind kind = Kind::Creature; uint32_t id = 0; };

    inline std::optional<Scan> parseScan(std::vector<std::string> const& f)
    {
        if (f.size() != 4)
            return std::nullopt;
        auto map = eft::number(f[0], 10000);
        auto x = decimal(f[1], 20000), y = decimal(f[2], 20000), r = decimal(f[3], 1000);
        if (!map || !x || !y || !r || *r <= 0 || *r > kMaxScanRadius)
            return std::nullopt;
        return Scan{*map, *x, *y, *r};
    }

    inline std::optional<Add> parseAdd(std::vector<std::string> const& f)
    {
        if (f.size() != 6)
            return std::nullopt;
        auto kind = kindOf(f[0]);
        auto entry = eft::number(f[1], 9999999);
        auto pos = position(f, 2);
        if (!kind || !entry || *entry == 0 || !pos)
            return std::nullopt;
        return Add{*kind, *entry, *pos};
    }

    inline std::optional<Move> parseMove(std::vector<std::string> const& f)
    {
        if (f.size() != 6)
            return std::nullopt;
        auto kind = kindOf(f[0]);
        auto id = eft::number(f[1], kTempIdMax);
        auto pos = position(f, 2);
        if (!kind || !id || *id == 0 || !pos)
            return std::nullopt;
        return Move{*kind, *id, *pos};
    }

    inline std::optional<Set> parseSet(std::vector<std::string> const& f)
    {
        if (f.size() != 4)
            return std::nullopt;
        auto kind = kindOf(f[0]);
        auto id = eft::number(f[1], kTempIdMax);
        auto value = decimal(f[3], 100000);
        if (!kind || !id || *id == 0 || !value || *value < 0)
            return std::nullopt;
        SetField field;
        if (f[2] == "wander")
            field = SetField::Wander;
        else if (f[2] == "respawn")
            field = SetField::Respawn;
        else
            return std::nullopt;
        return Set{*kind, *id, field, *value};
    }

    inline std::optional<Del> parseDel(std::vector<std::string> const& f)
    {
        if (f.size() != 2)
            return std::nullopt;
        auto kind = kindOf(f[0]);
        auto id = eft::number(f[1], kTempIdMax);
        if (!kind || !id || *id == 0)
            return std::nullopt;
        return Del{*kind, *id};
    }

    // ------------------------------------------------------------------------------------------ rules (§6.3 step 9)

    struct Limits
    {
        // values
        float wanderMax = 30;
        uint32_t respawnMin = 30, respawnMax = 3600;
        float npcMoveMax = 40;
        // density (step 10)
        uint32_t hostileWithin30 = 12;
        float spacing = 3, serviceClearance = 8, questGiverWarning = 20;
        // rate (step 11): per account
        uint32_t opsPerMinute = 60, opsPerDay = 600, addsPerDay = 40, deletesPerDay = 20;
        uint32_t openSets = 3, opsPerSet = 150, submitsPerHour = 5;

        // reviewers have ten times the contributor's limits, maintainers none (decision 3, §6.3 step 11)
        Limits scaled(Level level) const
        {
            Limits l = *this;
            uint32_t const k = level == Level::Maintainer ? 100000 : level == Level::Reviewer ? 10 : 1;
            l.opsPerMinute *= k, l.opsPerDay *= k, l.addsPerDay *= k, l.deletesPerDay *= k;
            l.openSets *= k, l.opsPerSet *= k, l.submitsPerHour *= k;
            return l;
        }
    };

    enum class Code : uint8_t
    {
        Ok, Access, Decode, Set, Zone, ReadOnly, Lock, Presence, Whitelist, Named, Ground, Water, Value, Distance,
        Density, Spacing, Clearance, Rate, Limit, Unknown, State, Text, Busy, Internal,
    };

    inline char const* codeName(Code c)
    {
        switch (c)
        {
            case Code::Ok: return "OK";
            case Code::Access: return "ACCESS";
            case Code::Decode: return "DECODE";
            case Code::Set: return "SET";
            case Code::Zone: return "ZONE";
            case Code::ReadOnly: return "READONLY";
            case Code::Lock: return "LOCK";
            case Code::Presence: return "PRESENCE";
            case Code::Whitelist: return "WHITELIST";
            case Code::Named: return "NAMED";
            case Code::Ground: return "GROUND";
            case Code::Water: return "WATER";
            case Code::Value: return "VALUE";
            case Code::Distance: return "DISTANCE";
            case Code::Density: return "DENSITY";
            case Code::Spacing: return "SPACING";
            case Code::Clearance: return "CLEARANCE";
            case Code::Rate: return "RATE";
            case Code::Limit: return "LIMIT";
            case Code::Unknown: return "UNKNOWN";
            case Code::State: return "STATE";
            case Code::Text: return "TEXT";
            case Code::Busy: return "BUSY";
            case Code::Internal: return "INTERNAL";
        }
        return "INTERNAL";
    }

    inline Code checkWander(float wander, Limits const& l) { return wander >= 0 && wander <= l.wanderMax ? Code::Ok : Code::Value; }

    inline Code checkRespawn(float seconds, bool named, Level level, Limits const& l)
    {
        if (named && level != Level::Maintainer)
            return Code::Named;
        if (seconds != std::floor(seconds) || seconds < l.respawnMin || seconds > l.respawnMax)
            return level == Level::Maintainer && seconds >= 1 && seconds <= 604800 && seconds == std::floor(seconds) ? Code::Ok : Code::Value;
        return Code::Ok;
    }

    // An NPC moves at most npcMoveMax from where the release has it (reviewers and maintainers: anywhere).
    inline Code checkNpcMove(Pos const& release, Pos const& to, Level level, Limits const& l)
    {
        if (level >= Level::Reviewer)
            return Code::Ok;
        return dist2d(release, to) <= l.npcMoveMax ? Code::Ok : Code::Distance;
    }

    // The ground rule (step 8): the given Z within kGroundTolerance of the ground; the stored Z is the ground's.
    inline Code checkGround(float given, float ground)
    {
        if (!std::isfinite(ground) || ground < -4000.0f)
            return Code::Ground;
        return std::fabs(given - ground) <= kGroundTolerance ? Code::Ok : Code::Ground;
    }

    struct Neighbour
    {
        Pos pos;
        bool hostile = false;
        bool service = false;    // a graveyard, flight master, innkeeper or spirit healer
        bool questGiver = false;
        bool self = false;       // the object being moved (it does not count against itself)
    };

    struct Density
    {
        Code code = Code::Ok;
        bool questGiverNear = false; // a warning, not a refusal
    };

    // Step 10 for a hostile spawn placed at `at`: at most hostileWithin30 hostile spawns within 30 yd (itself
    // included), none closer than `spacing`, nothing within `serviceClearance` of a service; a quest giver within
    // `questGiverWarning` only warns.
    inline Density checkDensity(Pos const& at, std::vector<Neighbour> const& near, Limits const& l)
    {
        Density d;
        uint32_t hostile = 1;
        for (Neighbour const& n : near)
        {
            if (n.self)
                continue;
            float const dd = dist2d(at, n.pos);
            if (n.service && dd < l.serviceClearance)
            {
                d.code = Code::Clearance;
                return d;
            }
            if (dd < l.spacing)
            {
                d.code = Code::Spacing;
                return d;
            }
            if (n.hostile && dd <= 30.0f)
                ++hostile;
            if (n.questGiver && dd <= l.questGiverWarning)
                d.questGiverNear = true;
        }
        if (hostile > l.hostileWithin30)
            d.code = Code::Density;
        return d;
    }

    // ------------------------------------------------------------------------------------------ rate (step 11)

    // Counts in sliding windows; times in ms (the realm's wall clock for the day window, so a restart keeps it when
    // the module seeds it from ef_op.created_at).
    class Window
    {
    public:
        Window(uint32_t limit, uint64_t windowMs) : m_limit(limit), m_window(windowMs) { }
        void prune(uint64_t now)
        {
            size_t keep = 0;
            for (uint64_t t : m_times)
                if (now - t < m_window)
                    m_times[keep++] = t;
            m_times.resize(keep);
        }
        bool full(uint64_t now) { prune(now); return m_times.size() >= m_limit; }
        void add(uint64_t t) { m_times.push_back(t); }
        void setLimit(uint32_t limit) { m_limit = limit; }
        size_t count(uint64_t now) { prune(now); return m_times.size(); }
        uint32_t limit() const { return m_limit; }

    private:
        uint32_t m_limit;
        uint64_t m_window;
        std::vector<uint64_t> m_times;
    };

    enum class OpKind : uint8_t { Add, Move, Set, Delete };

    struct Rate
    {
        Window minute{60, 60000}, day{600, 86400000}, adds{40, 86400000}, deletes{20, 86400000}, submits{5, 3600000};

        void configure(Limits const& l)
        {
            minute.setLimit(l.opsPerMinute), day.setLimit(l.opsPerDay), adds.setLimit(l.addsPerDay);
            deletes.setLimit(l.deletesPerDay), submits.setLimit(l.submitsPerHour);
        }
        // Whether one more op of this kind fits; nothing is counted.
        bool allows(OpKind k, uint64_t now)
        {
            if (minute.full(now) || day.full(now))
                return false;
            if (k == OpKind::Add && adds.full(now))
                return false;
            if (k == OpKind::Delete && deletes.full(now))
                return false;
            return true;
        }
        void record(OpKind k, uint64_t now)
        {
            minute.add(now), day.add(now);
            if (k == OpKind::Add)
                adds.add(now);
            if (k == OpKind::Delete)
                deletes.add(now);
        }
    };

    // ------------------------------------------------------------------------------------------ text (§13.1)

    // A title or description: printable ASCII after decoding (the wire is percent-encoded), within its length, no
    // links, no mentions, no invites. The quest bot's checker runs again on the bot; this is the realm's first line.
    inline Code checkText(std::string_view text, size_t minLen, size_t maxLen)
    {
        if (text.size() < minLen || text.size() > maxLen)
            return Code::Text;
        std::string lower;
        for (char ch : text)
        {
            auto const c = static_cast<unsigned char>(ch);
            if (c < 0x20 || c > 0x7e)
                return Code::Text;
            lower += static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
        }
        for (std::string_view bad : {"http:", "https:", "www.", "discord.gg", "@everyone", "@here", "<@", "://"})
            if (lower.find(bad) != std::string::npos)
                return Code::Text;
        return Code::Ok;
    }

    // ------------------------------------------------------------------------------------------ flat JSON

    // The ops' from/to (build_editor.ef_op.from_json, to_json): a flat object of numbers, null and short strings,
    // which the site and the bot read as JSON. Keys sorted; numbers as given (the caller rounds).
    struct Value
    {
        enum class Type : uint8_t { Null, Number, String } type = Type::Null;
        double number = 0;
        std::string text;
        static Value of(double v) { Value x; x.type = Type::Number; x.number = v; return x; }
        static Value str(std::string v) { Value x; x.type = Type::String; x.text = std::move(v); return x; }
        static Value null() { return Value{}; }
        bool operator==(Value const& o) const
        {
            return type == o.type && (type != Type::Number || number == o.number) && (type != Type::String || text == o.text);
        }
    };
    using Fields = std::map<std::string, Value>;

    inline std::string numberText(double v)
    {
        if (v == std::floor(v) && std::fabs(v) < 1e12)
            return std::to_string(static_cast<long long>(v));
        std::string s = fixed(v, 4);
        while (!s.empty() && s.back() == '0')
            s.pop_back();
        if (!s.empty() && s.back() == '.')
            s.pop_back();
        return s;
    }

    inline std::string toJson(Fields const& o)
    {
        std::string out = "{";
        bool first = true;
        for (auto const& [k, v] : o)
        {
            if (!first)
                out += ",";
            first = false;
            out += "\"" + k + "\":";
            if (v.type == Value::Type::Null)
                out += "null";
            else if (v.type == Value::Type::Number)
                out += numberText(v.number);
            else
            {
                out += "\"";
                for (char c : v.text)
                {
                    if (c == '"' || c == '\\')
                        out += '\\';
                    if (static_cast<unsigned char>(c) >= 0x20)
                        out += c;
                }
                out += "\"";
            }
        }
        return out + "}";
    }

    // Only what toJson writes (and whitespace); anything else is nullopt.
    inline std::optional<Fields> fromJson(std::string_view s)
    {
        Fields out;
        size_t i = 0;
        auto ws = [&] { while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\t' || s[i] == '\r')) ++i; };
        auto str = [&]() -> std::optional<std::string> {
            if (i >= s.size() || s[i] != '"')
                return std::nullopt;
            ++i;
            std::string v;
            while (i < s.size() && s[i] != '"')
            {
                if (s[i] == '\\')
                {
                    if (++i >= s.size())
                        return std::nullopt;
                }
                v += s[i++];
                if (v.size() > 2000)
                    return std::nullopt;
            }
            if (i >= s.size())
                return std::nullopt;
            ++i;
            return v;
        };
        ws();
        if (i >= s.size() || s[i++] != '{')
            return std::nullopt;
        ws();
        if (i < s.size() && s[i] == '}')
            return out;
        while (true)
        {
            ws();
            auto key = str();
            if (!key || key->empty() || key->size() > 32)
                return std::nullopt;
            ws();
            if (i >= s.size() || s[i++] != ':')
                return std::nullopt;
            ws();
            if (s.substr(i, 4) == "null")
            {
                out[*key] = Value::null();
                i += 4;
            }
            else if (i < s.size() && s[i] == '"')
            {
                auto v = str();
                if (!v)
                    return std::nullopt;
                out[*key] = Value::str(*v);
            }
            else
            {
                size_t j = i;
                while (j < s.size() && (s[j] == '-' || s[j] == '.' || (s[j] >= '0' && s[j] <= '9') || s[j] == 'e' || s[j] == 'E' || s[j] == '+'))
                    ++j;
                std::string_view num = s.substr(i, j - i);
                // the plain decimal form (no exponent) through decimal(); an exponent is refused
                auto v = decimal(num, 1e9f);
                if (!v)
                {
                    // an integer longer than decimal() takes (ids): digits only
                    auto n = eft::number(num);
                    if (!n)
                        return std::nullopt;
                    out[*key] = Value::of(*n);
                }
                else
                {
                    // keep integers exact
                    auto n = eft::number(num);
                    out[*key] = n ? Value::of(*n) : Value::of(std::stod(std::string(num)));
                }
                i = j;
            }
            ws();
            if (i < s.size() && s[i] == ',')
            {
                ++i;
                continue;
            }
            if (i < s.size() && s[i] == '}')
            {
                ++i;
                ws();
                return i == s.size() ? std::optional<Fields>(out) : std::nullopt;
            }
            return std::nullopt;
        }
    }

    inline Fields posObject(Pos const& p)
    {
        auto r = [](float v, double k) { return std::round(double(v) * k) / k; };
        return {{"x", Value::of(r(p.x, 100))}, {"y", Value::of(r(p.y, 100))}, {"z", Value::of(r(p.z, 100))},
                {"o", Value::of(r(p.o, 1000))}};
    }

    inline std::optional<Pos> posOf(Fields const& o)
    {
        auto get = [&](char const* k) -> std::optional<double> {
            auto it = o.find(k);
            if (it == o.end() || it->second.type != Value::Type::Number)
                return std::nullopt;
            return it->second.number;
        };
        auto x = get("x"), y = get("y"), z = get("z"), r = get("o");
        if (!x || !y || !z)
            return std::nullopt;
        return Pos{float(*x), float(*y), float(*z), float(r.value_or(0))};
    }

    // ------------------------------------------------------------------------------------------ the wire

    // Marker flags (MARK, DELTA): the viewer's view of a spawn.
    namespace flag
    {
        constexpr char Mine = 'm';      // in the viewer's open set
        constexpr char Pending = 'p';   // in someone's open set (not the viewer's)
        constexpr char Locked = 'l';    // held by another's open set: the viewer cannot edit it
        constexpr char ReadOnly = 'r';  // generated file, outside an editable zone, or an NPC inside a building
        constexpr char Named = 'n';     // a named creature (contributors: no respawn change, no add)
        constexpr char Added = 'a';     // added on the realm (a temporary id)
        constexpr char Dead = 'd';      // no living unit right now
    }

    struct Marker
    {
        Kind kind = Kind::Creature;
        uint32_t id = 0, entry = 0;
        Pos pos;
        float wander = 0;
        uint32_t respawn = 0;
        std::string flags;
        uint32_t lowguid = 0;
    };

    inline std::string encodeMarker(Marker const& m)
    {
        return std::string(1, static_cast<char>(m.kind)) + "," + std::to_string(m.id) + "," + std::to_string(m.entry) + "," +
               fixed(m.pos.x, 2) + "," + fixed(m.pos.y, 2) + "," + fixed(m.pos.z, 2) + "," + fixed(m.pos.o, 3) + "," +
               fixed(m.wander, 1) + "," + std::to_string(m.respawn) + "," + m.flags + "," + std::to_string(m.lowguid);
    }

    inline std::optional<Marker> decodeMarker(std::string_view text)
    {
        auto v = eft::split(text, ',');
        if (v.size() != 11)
            return std::nullopt;
        Marker m;
        auto k = kindOf(v[0]);
        auto id = eft::number(v[1]), entry = eft::number(v[2]), respawn = eft::number(v[8]), low = eft::number(v[10]);
        auto x = decimal(v[3], 20000), y = decimal(v[4], 20000), z = decimal(v[5], 5000), o = decimal(v[6], 100), w = decimal(v[7], 1000);
        if (!k || !id || !entry || !respawn || !low || !x || !y || !z || !o || !w || v[9].size() > 8)
            return std::nullopt;
        m.kind = *k, m.id = *id, m.entry = *entry, m.pos = {*x, *y, *z, *o}, m.wander = *w, m.respawn = *respawn;
        m.flags = std::string(v[9]), m.lowguid = *low;
        return m;
    }

    // Packs entries into `|`-joined payloads of at most `room` bytes each (an entry never splits).
    inline std::vector<std::string> pack(std::vector<std::string> const& entries, size_t room)
    {
        std::vector<std::string> out;
        std::string cur;
        for (std::string const& e : entries)
        {
            if (e.size() > room)
                continue;
            if (!cur.empty() && cur.size() + 1 + e.size() > room)
            {
                out.push_back(cur);
                cur.clear();
            }
            if (!cur.empty())
                cur += '|';
            cur += e;
        }
        if (!cur.empty() || out.empty())
            out.push_back(cur);
        return out;
    }

    // Bodies of a paged list (MARK, OPS): `1;0;<cmd>;<head...>;<p>/<t>;<payload>`. Unlike EFT's chunked payloads, every
    // page is complete in itself (whole entries), and there may be up to `maxPages` (a scan can be long).
    inline std::vector<std::string> pages(std::string_view cmd, std::vector<std::string> const& head,
                                          std::vector<std::string> const& entries, size_t maxPages = 64)
    {
        std::string const fixedPart = eft::body(0, cmd, head) + ";";
        size_t const pageField = 8; // "pp/tt;" with up to 64
        if (fixedPart.size() + pageField >= eft::kMaxBody)
            return {};
        auto parts = pack(entries, eft::kMaxBody - fixedPart.size() - pageField);
        if (parts.size() > maxPages)
            parts.resize(maxPages);
        std::vector<std::string> out;
        for (size_t i = 0; i < parts.size(); ++i)
            out.push_back(fixedPart + std::to_string(i + 1) + "/" + std::to_string(parts.size()) + ";" + parts[i]);
        return out;
    }

    // ROLE's zones field: `key:map:area` joined by ','; limits: `left today,open sets left,ops per set`.
    struct Zone
    {
        std::string key;
        uint32_t map = 0, area = 0;
    };

    // `Emberfall.Editor.Zones`: "elderwood:0:5001 other:1:5401" (spaces or commas between zones).
    inline std::vector<Zone> parseZones(std::string_view text)
    {
        std::vector<Zone> out;
        std::string cur;
        auto flush = [&] {
            if (cur.empty())
                return;
            auto v = eft::split(cur, ':');
            if (v.size() == 3)
                if (auto map = eft::number(v[1], 10000), area = eft::number(v[2]); map && area && !v[0].empty())
                    out.push_back({std::string(v[0]), *map, *area});
            cur.clear();
        };
        for (char c : text)
        {
            if (c == ' ' || c == ',' || c == '\t')
                flush();
            else
                cur += c;
        }
        flush();
        return out;
    }

    // ------------------------------------------------------------------------------------------ ops

    // One stored operation (build_editor.ef_op). `target` is the stable id (a release spawn's GUID, an NPC's id) or the
    // temporary id of an add.
    struct Op
    {
        uint64_t dbId = 0;
        uint32_t seq = 0;
        OpKind op = OpKind::Move;
        Kind kind = Kind::Creature;
        uint32_t target = 0;
        uint32_t tempId = 0, finalId = 0;
        Fields from, to;
        bool undone = false;
        uint64_t createdMs = 0;
    };

    inline char const* opName(OpKind k)
    {
        switch (k)
        {
            case OpKind::Add: return "add";
            case OpKind::Move: return "move";
            case OpKind::Set: return "set";
            case OpKind::Delete: return "delete";
        }
        return "move";
    }

    inline std::optional<OpKind> opKind(std::string_view s)
    {
        if (s == "add") return OpKind::Add;
        if (s == "move") return OpKind::Move;
        if (s == "set") return OpKind::Set;
        if (s == "delete") return OpKind::Delete;
        return std::nullopt;
    }

    // The op that undoes `op` (§8.4): from and to swapped; an add's inverse is a delete of what it added, a delete's an
    // add of what it removed (under the same id).
    inline Op inverse(Op const& op)
    {
        Op inv = op;
        inv.dbId = 0;
        inv.undone = false;
        std::swap(inv.from, inv.to);
        if (op.op == OpKind::Add)
            inv.op = OpKind::Delete;
        else if (op.op == OpKind::Delete)
            inv.op = OpKind::Add;
        return inv;
    }

    // Which ops of a set are undoable now (the newest active) and redoable (the oldest of the undone ops after it).
    inline std::optional<size_t> undoIndex(std::vector<Op> const& ops)
    {
        for (size_t i = ops.size(); i-- > 0;)
            if (!ops[i].undone)
                return i;
        return std::nullopt;
    }

    inline std::optional<size_t> redoIndex(std::vector<Op> const& ops)
    {
        std::optional<size_t> out;
        for (size_t i = ops.size(); i-- > 0;)
        {
            if (!ops[i].undone)
                break;
            out = i;
        }
        return out;
    }

    // The number of objects a set changes (its net ops: §6.3 "150 net ops per set" counts objects touched).
    inline size_t netObjects(std::vector<Op> const& ops)
    {
        std::map<std::pair<char, uint32_t>, int> touched;
        for (Op const& op : ops)
            if (!op.undone)
                touched[{static_cast<char>(op.kind), op.target}] = 1;
        return touched.size();
    }

    // Whether an op is already in effect on a spawn whose current state is `now` (replay, §7.3): a move whose target
    // stands at `to`, a set whose field already has `to`. `now` holds x, y, z, o, wander, respawn.
    inline bool posNear(Fields const& a, Fields const& b)
    {
        auto pa = posOf(a), pb = posOf(b);
        if (!pa || !pb)
            return false;
        if (std::fabs(pa->x - pb->x) > 0.05f || std::fabs(pa->y - pb->y) > 0.05f || std::fabs(pa->z - pb->z) > 0.05f)
            return false;
        if (a.count("o") && b.count("o"))
        {
            float d = std::fabs(normaliseOrientation(pa->o) - normaliseOrientation(pb->o));
            return std::fmin(d, 6.2831853f - d) < 0.01f;
        }
        return true;
    }

    enum class Replay : uint8_t { Apply, AlreadyDone, Conflict };

    inline Replay replayState(Op const& op, Fields const& now)
    {
        if (op.op == OpKind::Move)
        {
            if (posNear(now, op.to))
                return Replay::AlreadyDone;
            return posNear(now, op.from) ? Replay::Apply : Replay::Conflict;
        }
        if (op.op == OpKind::Set)
        {
            for (auto const& [k, v] : op.to)
            {
                auto it = now.find(k);
                if (it != now.end() && it->second == v)
                    return Replay::AlreadyDone;
                auto was = op.from.find(k);
                if (was == op.from.end() || was->second.type == Value::Type::Null)
                    return Replay::Apply; // from the template's value: nothing to compare
                return it != now.end() && it->second == was->second ? Replay::Apply : Replay::Conflict;
            }
        }
        return Replay::Apply;
    }
}
