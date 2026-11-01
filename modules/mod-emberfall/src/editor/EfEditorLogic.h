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
//   C->S  HELLO;<data>  MODE;on|off  VIEW;on|off  SCAN;<map>;<x>;<y>;<radius>  OPEN;<cs>  NEW;<title>
//         ADD;c;<entry>;<x>;<y>;<z>;<o>  MOVE;<k>;<id>;<x>;<y>;<z>;<o>  SET;c;<id>;wander|respawn|count;<value>
//         DEL;c;<id>  UNDO;<cs>  REDO;<cs>  SUBMIT;<cs>;<p>/<t>;<description>  WITHDRAW;<cs>
//   S->C  ROLE;<level>;<zones>;<limits>  RZONE;<p>/<t>;<zone|...> (every zone, paged)  MARK;<scan>;<p>/<t>;<entry|...>
//         DELTA;<entry>  GONE;<k>;<id>
//         CS;<id>;<state>;<ops>;<title>[;1]  OPS;<cs>;<p>/<t>;<op|...>  OK;<seq>;<id>  ERR;<seq>;<code>;<detail>
//         (CS's sixth field 1: the editor's active set, e.g. one the realm opened by itself for a change without one)
//
// A marker entry: k,id,entry,x,y,z,o,wander,respawn,flags,lowguid (k: c creature spawn, n NPC; flags letters, below),
// and for a spawn with a draft (flag g) four more fields, gx,gy,gz,go: where the original stands for everyone else;
// last, for a spawn of several creatures (count 2-8), `<count>[:<lowguid>...]`: the count and the other creatures'
// living units (12 or 16 fields; a client from before the count drops only such markers).
//
// On the normal realm (world-editor.md §19): the role is the account's GM level for the realm (1 contributor, 2 and up
// maintainer). A contributor's open sets are drafts in one shared phase (kDraftPhase) that only editors with the
// editor open see (VIEW on); a maintainer's ops apply for everyone at once. The later steps are client overlays with
// kinds of their own: ground strokes l, r, h (step 3, §21: PAINT, ROAD, SCULPT, GND) and props p and kit buildings b
// (step 2, §22: PADD, PMOVE, PDEL, PLACE, PMOD), each section below.
// Everything here is bounds-checked and never throws: malformed input gives std::nullopt / an error value.

#pragma once

#include "../talents/EftProtocol.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
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
    // The draft copies of release spawns that a contributor's open set changes (§19): runtime only, never stored, never
    // sent as a marker id (the marker names the release spawn).
    constexpr uint32_t kCopyIdBase = 8000001;
    constexpr uint32_t kCopyIdMax = 8999999;
    // The editor phase: phase 32 (bit 31). Spawns there are the drafts; an editor sees them while the editor is open.
    // The memories use phases 2-31 (Emberfall quests/memories.py refuses 32); the present is phase 1.
    constexpr uint32_t kDraftPhase = 0x80000000u;

    enum class Level : uint8_t { None = 0, Contributor = 1, Reviewer = 2, Maintainer = 3 };

    // The account's GM level for the realm (account_access) as the editor's role: 0 none, 1 (SEC_MODERATOR) the
    // contributor (no GM command; plays as a player everywhere else), 2 and up the maintainer.
    inline Level levelOfSecurity(uint32_t gmlevel)
    {
        if (gmlevel == 0)
            return Level::None;
        return gmlevel == 1 ? Level::Contributor : Level::Maintainer;
    }

    // Whether a set of this owner applies for everyone at once (a maintainer's) or stays a draft in the editor phase
    // until it is accepted (a contributor's).
    inline bool appliesLive(Level level) { return level == Level::Maintainer; }

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

    // c creature spawn, n NPC; step 3 (world-editor.md §21): l a ground layer stroke, r a road, h a height stamp (the
    // ground kinds are no objects: no lock, no marker, nothing the server applies; ground below); step 2 (§22): p a prop,
    // b a kit building (objects with a lock, drawn by the clients only: places below).
    enum class Kind : char { Creature = 'c', Npc = 'n', Ground = 'l', Road = 'r', Height = 'h', Prop = 'p', Building = 'b' };

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
    enum class SetField : uint8_t { Wander, Respawn, Count };
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
        else if (f[2] == "count")
            field = SetField::Count;
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
        Density, Spacing, Clearance, Rate, Limit, Unknown, State, Text, Busy, Internal, Mode,
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
            case Code::Mode: return "MODE";
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

    // ------------------------------------------------------------------------------------------ count
    //
    // A spawn of several creatures (`count: N` on its spawn row, 1-kMaxCount): the spawn itself and N-1 others on a
    // ring of kCountRing yards round its point, each one a creature spawn of its own under a GUID derived from the
    // spawn's (extraId), so neither the realm nor the release compile allocates anything and both make the same
    // creatures (Emberfall tools/py/emberfall_content/sql/compile.py `count_slots` is the same arithmetic). They copy
    // the spawn's entry, wander radius and respawn, follow it when it moves, and go with it.
    //
    // GUIDs: a content spawn (GUID 1-999,999) has its others at GUID + slot x 1,000,000 (1,000,001-7,999,999: ids.yaml
    // `creature_guid_count`, world.sql's owned range); a spawn the realm made (a draft copy 8,000,001+, an add's
    // temporary id 9,000,001+) at id + slot x 10,000,000 (18,000,001-79,999,999, never stored). No two spawns share one.
    constexpr uint32_t kMaxCount = 8;                // [proposal] within the density rules: 12 hostile in 30 yd
    constexpr float kCountRing = 4.0f;               // yards: two neighbours on the ring stand 3.47 yd apart (spacing 3)
    constexpr uint32_t kContentGuidMax = 999999;
    constexpr uint32_t kCountStepContent = 1000000, kCountStepRealm = 10000000;

    // The GUID of a spawn's other creature `slot` (1 .. kMaxCount - 1); 0 for an id that has none.
    inline uint32_t extraId(uint32_t id, uint32_t slot)
    {
        if (id == 0 || slot == 0 || slot >= kMaxCount)
            return 0;
        if (id <= kContentGuidMax)
            return id + slot * kCountStepContent;
        if (id >= kCopyIdBase && id <= kTempIdMax)
            return id + slot * kCountStepRealm;
        return 0;
    }

    struct ExtraOf
    {
        uint32_t base = 0, slot = 0;
    };

    // Which spawn an id is the other creature of, or nothing for a spawn of its own.
    inline std::optional<ExtraOf> extraOf(uint32_t id)
    {
        if (id > kContentGuidMax && id < kCopyIdBase)
        {
            uint32_t const slot = id / kCountStepContent, base = id % kCountStepContent;
            if (base != 0 && slot >= 1 && slot < kMaxCount)
                return ExtraOf{base, slot};
            return std::nullopt;
        }
        if (id > kTempIdMax)
        {
            uint32_t const slot = id / kCountStepRealm, base = id % kCountStepRealm;
            if (base >= kCopyIdBase && base <= kTempIdMax && slot >= 1 && slot < kMaxCount)
                return ExtraOf{base, slot};
        }
        return std::nullopt;
    }

    inline uint32_t baseOf(uint32_t id)
    {
        auto e = extraOf(id);
        return e ? e->base : id;
    }

    // Where the other creature `slot` stands: on the ring round the spawn's point, the slots spread so any count is
    // even round it and a growing count leaves the others where they stood. Z is the spawn's (the realm puts each on
    // its ground when its unit loads); the facing is the spawn's turned by the golden angle per slot.
    inline Pos slotPos(Pos const& at, uint32_t slot)
    {
        static constexpr uint8_t kOrder[kMaxCount - 1] = {0, 4, 2, 6, 1, 5, 3};
        if (slot == 0 || slot >= kMaxCount)
            return at;
        double const a = kOrder[slot - 1] * (2.0 * 3.14159265358979323846 / 7.0);
        return Pos{float(at.x + kCountRing * std::cos(a)), float(at.y + kCountRing * std::sin(a)), at.z,
                   normaliseOrientation(float(at.o + slot * 2.399963))};
    }

    // Every creature of a spawn of `count`: the spawn's point first, then the slots.
    inline std::vector<Pos> groupPositions(Pos const& at, uint32_t count)
    {
        std::vector<Pos> out{at};
        for (uint32_t slot = 1; slot < std::min(count, kMaxCount); ++slot)
            out.push_back(slotPos(at, slot));
        return out;
    }

    inline Code checkCount(float count, bool named, Level level)
    {
        if (count != std::floor(count) || count < 1 || count > float(kMaxCount))
            return Code::Value;
        if (named && count > 1 && level != Level::Maintainer)
            return Code::Named;
        return Code::Ok;
    }

    // Step 10 for every creature of a spawn of several (`members`, groupPositions): each one by checkDensity, the
    // others of the group counted as hostile neighbours. `member`: which one failed (0 the spawn itself).
    struct GroupDensity
    {
        Density density;
        uint32_t member = 0;
    };

    inline GroupDensity checkGroupDensity(std::vector<Pos> const& members, std::vector<Neighbour> const& near, Limits const& l)
    {
        GroupDensity out;
        for (size_t i = 0; i < members.size(); ++i)
        {
            std::vector<Neighbour> list = near;
            for (size_t j = 0; j < members.size(); ++j)
                if (j != i)
                {
                    Neighbour n;
                    n.pos = members[j];
                    n.hostile = true;
                    list.push_back(n);
                }
            Density const d = checkDensity(members[i], list, l);
            out.density.questGiverNear = out.density.questGiverNear || d.questGiverNear;
            if (d.code != Code::Ok)
            {
                out.density.code = d.code;
                out.member = uint32_t(i);
                return out;
            }
        }
        return out;
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
        // A count raised by `n` places n creatures: they count against the day's adds.
        bool allowsAdds(uint32_t n, uint64_t now) { return n == 0 || adds.count(now) + n <= adds.limit(); }
        void recordAdds(uint32_t n, uint64_t now)
        {
            for (uint32_t i = 0; i < n; ++i)
                adds.add(now);
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
        constexpr char Ghost = 'g';     // a draft moved or changed it: the marker is the draft, gx..go the original
        constexpr char Removed = 'x';   // a draft removes it (editors with the editor open no longer see its unit)
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
        std::optional<Pos> ghost;   // flag g: where the original stands
        uint32_t count = 1;         // the spawn's creatures
        std::vector<uint32_t> others; // the other creatures' living units (lowguids)
    };

    // A marker stays well inside one DELTA (eft::kMaxBody with its head) and one MARK page: the others' lowguids that
    // would not fit are left off (the count stays).
    constexpr size_t kMaxMarker = 200;

    inline std::string encodeMarker(Marker const& m)
    {
        std::string out = std::string(1, static_cast<char>(m.kind)) + "," + std::to_string(m.id) + "," + std::to_string(m.entry) + "," +
               fixed(m.pos.x, 2) + "," + fixed(m.pos.y, 2) + "," + fixed(m.pos.z, 2) + "," + fixed(m.pos.o, 3) + "," +
               fixed(m.wander, 1) + "," + std::to_string(m.respawn) + "," + m.flags + "," + std::to_string(m.lowguid);
        if (m.ghost)
            out += "," + fixed(m.ghost->x, 2) + "," + fixed(m.ghost->y, 2) + "," + fixed(m.ghost->z, 2) + "," + fixed(m.ghost->o, 3);
        if (m.count > 1)
        {
            out += "," + std::to_string(std::min(m.count, kMaxCount));
            for (size_t i = 0; i < m.others.size() && i + 1 < kMaxCount; ++i)
            {
                std::string const low = ":" + std::to_string(m.others[i]);
                if (out.size() + low.size() > kMaxMarker)
                    break;
                out += low;
            }
        }
        return out;
    }

    inline std::optional<Marker> decodeMarker(std::string_view text)
    {
        auto v = eft::split(text, ',');
        if (v.size() != 11 && v.size() != 12 && v.size() != 15 && v.size() != 16)
            return std::nullopt;
        Marker m;
        auto k = kindOf(v[0]);
        auto id = eft::number(v[1]), entry = eft::number(v[2]), respawn = eft::number(v[8]), low = eft::number(v[10]);
        auto x = decimal(v[3], 20000), y = decimal(v[4], 20000), z = decimal(v[5], 5000), o = decimal(v[6], 100), w = decimal(v[7], 1000);
        if (!k || !id || !entry || !respawn || !low || !x || !y || !z || !o || !w || v[9].size() > 8)
            return std::nullopt;
        m.kind = *k, m.id = *id, m.entry = *entry, m.pos = {*x, *y, *z, *o}, m.wander = *w, m.respawn = *respawn;
        m.flags = std::string(v[9]), m.lowguid = *low;
        if (v.size() >= 15)
        {
            auto gx = decimal(v[11], 20000), gy = decimal(v[12], 20000), gz = decimal(v[13], 5000), go = decimal(v[14], 100);
            if (!gx || !gy || !gz || !go)
                return std::nullopt;
            m.ghost = Pos{*gx, *gy, *gz, *go};
        }
        if (v.size() == 12 || v.size() == 16)
        {
            auto c = eft::split(v.back(), ':');
            auto n = eft::number(c[0], kMaxCount);
            if (!n || *n < 2 || c.size() > kMaxCount)
                return std::nullopt;
            m.count = *n;
            for (size_t i = 1; i < c.size(); ++i)
            {
                auto low = eft::number(c[i]);
                if (!low)
                    return std::nullopt;
                m.others.push_back(*low);
            }
        }
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

    // ROLE;<level>;<zones>;<limits>;<local|realm> - ONE message: the client reads one ROLE (a second one replaces it)
    // and drops a body over kMaxBody unread, so the editor never appears. The limits shown are capped at
    // kRoleLimitShown (a maintainer's are 100000x a contributor's: 24 bytes that pushed 12 zones over 251, alpha1
    // 0.4.19); the server enforces the real ones. Zones that would still not fit are left off from the end
    // (`dropped`; the caller logs it) - the client only uses them to pick the palette of its map.
    constexpr uint32_t kRoleLimitShown = 9999;
    inline std::string roleBody(uint32_t level, std::vector<Zone> const& zones, uint32_t opsLeft, uint32_t setsLeft,
                                uint32_t opsPerSet, bool local, size_t* dropped = nullptr)
    {
        auto shown = [](uint32_t v) { return std::to_string(std::min(v, kRoleLimitShown)); };
        std::string const limits = shown(opsLeft) + "," + shown(setsLeft) + "," + shown(opsPerSet);
        std::vector<std::string> list;
        for (Zone const& z : zones)
            list.push_back(z.key + ":" + std::to_string(z.map) + ":" + std::to_string(z.area));
        for (size_t n = list.size();; --n)
        {
            std::string joined;
            for (size_t i = 0; i < n; ++i)
                joined += (i ? "," : "") + list[i];
            std::string body = eft::body(0, "ROLE", {std::to_string(level), joined, limits, local ? "local" : "realm"});
            if (body.size() <= eft::kMaxBody || n == 0)
            {
                if (dropped)
                    *dropped = list.size() - n;
                return body;
            }
        }
    }

    // RZONE;<p>/<t>;<key:map:area|...> - every zone, paged (any number of them; whole entries per page), sent right after
    // ROLE. ROLE keeps the zones that fit one message (a client from before RZONE reads only those); a client that knows
    // RZONE replaces ROLE's list with the pages' once the last page is there.
    inline std::vector<std::string> roleZonePages(std::vector<Zone> const& zones)
    {
        std::vector<std::string> list;
        for (Zone const& z : zones)
            list.push_back(z.key + ":" + std::to_string(z.map) + ":" + std::to_string(z.area));
        return pages("RZONE", {}, list, 64);
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
            if (!op.undone && op.kind != Kind::Ground && op.kind != Kind::Road && op.kind != Kind::Height &&
                op.kind != Kind::Prop && op.kind != Kind::Building)  // strokes: groundCount; props, buildings: placeCount
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

    // ------------------------------------------------------------------------------------------ drafts (§19)

    // What a contributor's open set makes of one object in the editor phase: its active ops folded onto the release
    // state (`release`: x, y, z, o, wander, respawn, entry; nothing for a spawn the set adds), by the replay's rules
    // (§7.3): an op already in effect is skipped (it shipped), one that meets neither its from nor its to is a conflict
    // and changes nothing.
    struct Draft
    {
        bool present = false;   // the object exists in the draft
        bool changed = false;   // ... and differs from the release (a draft copy is needed), or the set adds it
        bool conflict = false;
        Fields state;
    };

    inline bool sameState(Fields const& a, Fields const& b)
    {
        if (!posNear(a, b))
            return false;
        for (char const* k : {"wander", "respawn", "entry", "count"})
        {
            auto x = a.find(k), y = b.find(k);
            if ((x == a.end()) != (y == b.end()) || (x != a.end() && !(x->second == y->second)))
                return false;
        }
        return true;
    }

    inline Draft foldDraft(std::optional<Fields> const& release, std::vector<Op> const& ops)
    {
        Draft d;
        if (release)
            d.present = true, d.state = *release;
        for (Op const& op : ops)
        {
            if (op.undone)
                continue;
            switch (op.op)
            {
                case OpKind::Add:
                    if (!d.present)
                    {
                        d.present = true;
                        d.state = op.to;
                        d.state.emplace("count", Value::of(1));   // an add places one; a count op may raise it
                    }
                    break;
                case OpKind::Delete:
                    d.present = false;
                    break;
                case OpKind::Move:
                case OpKind::Set:
                {
                    if (!d.present)
                    {
                        d.conflict = true;
                        break;
                    }
                    Replay const how = replayState(op, d.state);
                    if (how == Replay::Conflict)
                        d.conflict = true;
                    else if (how == Replay::Apply)
                        for (auto const& [k, v] : op.to)
                            d.state[k] = v;
                    break;
                }
            }
        }
        d.changed = !release ? d.present : (!d.present || !sameState(d.state, *release));
        return d;
    }

    // ------------------------------------------------------------------------------------------ editor mode (§19)

    // Flight, speed and safety are an explicit toggle (default off; the panel works without it). Not in combat, dead,
    // on a flight path or in a dungeon or battleground.
    inline Code checkModeOn(bool inCombat, bool alive, bool onTaxi, bool instance)
    {
        return inCombat || !alive || onTaxi || instance ? Code::Mode : Code::Ok;
    }

    // Turning editor mode off takes a contributor back to where they turned it on (flight is no travel shortcut); a
    // maintainer stays where they are.
    inline bool returnsOnModeOff(Level level) { return level != Level::Maintainer; }

    // The title of a set the realm opens by itself for a change made without one: "<Zone> edits <YYYY-MM-DD>".
    inline std::string autoTitle(std::string_view zoneName, std::string_view date)
    {
        return std::string(zoneName.substr(0, kMaxTitle - 18)) + " edits " + std::string(date.substr(0, 10));
    }

    // ------------------------------------------------------------------------------------------ ground (step 3, §20)

    // Ground layer strokes (texture), roads and height stamps: ops of kind l, r, h with op `add`, each the record of one
    // stroke (a click, or a line of up to kMaxGroundPoints points). Nothing on the server changes: the realm validates,
    // stores and forwards them (GND), the client paints them over its own copy of the ground (a ghost for heights), and
    // the release bakes them (Emberfall tools/py emberfall_content/editor/ground.py). Collision, paths and line of sight
    // stay the release's. The rules are written down in docs/design/world-editor.md §21.
    //
    //   C->S  PAINT;<layer>;<radius>;<strength>;<hardness>;<group>;<path>
    //         ROAD;<layer>;<class>;<group>;<path>
    //         SCULPT;raise|lower|smooth|flatten;<radius>;<strength>;<height|->;<group>;<path>
    //   S->C  GND;<cs>;<seq>;<flags>;<map>;l;<tileset/layer>;<radius>;<strength>;<hardness>;<path>
    //         GND;<cs>;<seq>;<flags>;<map>;r;<tileset/layer>;<class>;<path>
    //         GND;<cs>;<seq>;<flags>;<map>;h;<mode>;<radius>;<strength>;<height|->;<path>
    //         GNDX;<cs>;<seq|*>   (GNDX;*;* first at login: forget every stroke)   GLAY;<zone>;<p>/<t>;<layer|...>
    // `path`: `x0,y0|dx,dy|...`, x0 and y0 world yards with one decimal, each further point in whole decimetres from the
    // one before. `group`: the client's number for one line split over several ops (UNDO and REDO take the whole group;
    // 0: none). GND flags: d a contributor's draft (only editors with the editor open get those), m the viewer's own.

    constexpr float kTileYards = 1600.0f / 3.0f;
    constexpr size_t kMaxGroundPoints = 40;
    constexpr size_t kMaxLayerName = 24;
    // The client keeps a PAINT, ROAD or SCULPT body at most this long, so its GND (a longer head, `<tileset>/` before
    // the layer, at most 10 letters of tileset) fits one message; it splits a longer line into several ops of a group.
    constexpr size_t kMaxGroundRequest = 214;

    inline bool isGround(Kind k) { return k == Kind::Ground || k == Kind::Road || k == Kind::Height; }
    inline bool isGround(char k) { return k == 'l' || k == 'r' || k == 'h'; }

    struct GroundLimits
    {
        float paintRadiusMin = 0.5f, paintRadiusMax = 16.0f;
        float sculptRadiusMin = 2.0f, sculptRadiusMax = 40.0f;
        float strengthMin = 0.05f;
        float flattenReach = 40.0f;      // a flatten height within this of the ground at the stroke's first point
        float maxSegment = 60.0f, maxLength = 250.0f;
        uint32_t perSet = 400;           // strokes per set, for everyone (every client paints them all: performance)
        uint32_t perMinute = 90, perDay = 1500;   // per account (contributors; reviewers x10, maintainers no limit)
        uint32_t realmCap = 6000;        // strokes waiting for a release on the realm (every set)
    };

    // roads.py CLASSES: the half width of each road class (painted full to hw - 0.5, nothing past hw + 2).
    inline std::optional<float> roadHalfWidth(std::string_view cls)
    {
        if (cls == "path") return 1.75f;
        if (cls == "track") return 2.5f;
        if (cls == "road") return 3.5f;
        if (cls == "main") return 4.5f;
        return std::nullopt;
    }

    enum class Sculpt : uint8_t { Raise, Lower, Smooth, Flatten };

    inline char const* sculptName(Sculpt m)
    {
        switch (m)
        {
            case Sculpt::Raise: return "raise";
            case Sculpt::Lower: return "lower";
            case Sculpt::Smooth: return "smooth";
            case Sculpt::Flatten: return "flatten";
        }
        return "raise";
    }

    inline std::optional<Sculpt> sculptOf(std::string_view s)
    {
        if (s == "raise") return Sculpt::Raise;
        if (s == "lower") return Sculpt::Lower;
        if (s == "smooth") return Sculpt::Smooth;
        if (s == "flatten") return Sculpt::Flatten;
        return std::nullopt;
    }

    struct Point { float x = 0, y = 0; };

    // A signed whole number of at most 6 characters.
    inline std::optional<int32_t> integer(std::string_view text)
    {
        bool const negative = !text.empty() && text[0] == '-';
        if (text.size() > 6)
            return std::nullopt;
        auto n = eft::number(negative ? text.substr(1) : text, 99999);
        if (!n)
            return std::nullopt;
        return negative ? -int32_t(*n) : int32_t(*n);
    }

    inline std::optional<std::vector<Point>> decodePath(std::string_view text, size_t maxPoints = kMaxGroundPoints)
    {
        auto parts = eft::split(text, '|');
        if (text.empty() || parts.size() > maxPoints)
            return std::nullopt;
        std::vector<Point> out;
        int64_t x = 0, y = 0;   // decimetres
        for (size_t i = 0; i < parts.size(); ++i)
        {
            auto xy = eft::split(parts[i], ',');
            if (xy.size() != 2)
                return std::nullopt;
            if (i == 0)
            {
                auto fx = decimal(xy[0], 20000), fy = decimal(xy[1], 20000);
                if (!fx || !fy)
                    return std::nullopt;
                x = std::llround(double(*fx) * 10), y = std::llround(double(*fy) * 10);
            }
            else
            {
                auto dx = integer(xy[0]), dy = integer(xy[1]);
                if (!dx || !dy || std::abs(*dx) > 600 || std::abs(*dy) > 600)
                    return std::nullopt;
                x += *dx, y += *dy;
                if (std::llabs(x) > 200000 || std::llabs(y) > 200000)
                    return std::nullopt;
            }
            out.push_back({float(double(x) / 10), float(double(y) / 10)});
        }
        return out;
    }

    // The wire form of a path (positions on the decimetre grid; decodePath gives exactly those back).
    inline std::string encodePath(std::vector<Point> const& pts)
    {
        std::string out;
        int64_t px = 0, py = 0;
        for (size_t i = 0; i < pts.size(); ++i)
        {
            int64_t const x = std::llround(double(pts[i].x) * 10), y = std::llround(double(pts[i].y) * 10);
            if (i == 0)
                out = fixed(double(x) / 10, 1) + "," + fixed(double(y) / 10, 1);
            else
                out += "|" + std::to_string(x - px) + "," + std::to_string(y - py);
            px = x, py = y;
        }
        return out;
    }

    struct Stroke
    {
        Kind kind = Kind::Ground;
        std::string layer;      // the zone's layer name (l, r)
        std::string cls;        // a road's class (r)
        Sculpt mode = Sculpt::Raise;
        float radius = 0, strength = 1, hardness = 0.5f;
        std::optional<float> height;   // flatten's target (h)
        uint32_t group = 0;
        std::vector<Point> pts;
    };

    inline bool layerName(std::string_view s)
    {
        if (s.empty() || s.size() > kMaxLayerName)
            return false;
        for (char c : s)
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
                return false;
        return true;
    }

    inline std::optional<Stroke> parsePaint(std::vector<std::string> const& f)
    {
        if (f.size() != 6 || !layerName(f[0]))
            return std::nullopt;
        auto r = decimal(f[1], 1000), s = decimal(f[2], 10), h = decimal(f[3], 10);
        auto g = eft::number(f[4], 9999);
        auto pts = decodePath(f[5]);
        if (!r || !s || !h || !g || !pts)
            return std::nullopt;
        Stroke st;
        st.kind = Kind::Ground, st.layer = f[0], st.radius = *r, st.strength = *s, st.hardness = *h, st.group = *g, st.pts = *pts;
        return st;
    }

    inline std::optional<Stroke> parseRoad(std::vector<std::string> const& f)
    {
        if (f.size() != 4 || !layerName(f[0]) || !roadHalfWidth(f[1]))
            return std::nullopt;
        auto g = eft::number(f[2], 9999);
        auto pts = decodePath(f[3]);
        if (!g || !pts)
            return std::nullopt;
        Stroke st;
        st.kind = Kind::Road, st.layer = f[0], st.cls = f[1], st.group = *g, st.pts = *pts;
        st.radius = *roadHalfWidth(f[1]), st.strength = 1, st.hardness = 1;
        return st;
    }

    inline std::optional<Stroke> parseSculpt(std::vector<std::string> const& f)
    {
        if (f.size() != 6)
            return std::nullopt;
        auto mode = sculptOf(f[0]);
        auto r = decimal(f[1], 1000), s = decimal(f[2], 10);
        auto g = eft::number(f[4], 9999);
        auto pts = decodePath(f[5]);
        if (!mode || !r || !s || !g || !pts)
            return std::nullopt;
        Stroke st;
        st.kind = Kind::Height, st.mode = *mode, st.radius = *r, st.strength = *s, st.group = *g, st.pts = *pts;
        if (f[3] != "-")
        {
            auto z = decimal(f[3], 5000);
            if (!z)
                return std::nullopt;
            st.height = *z;
        }
        if (*mode == Sculpt::Flatten && !st.height)
            return std::nullopt;
        if (*mode != Sculpt::Flatten)
            st.height.reset();
        return st;
    }

    inline float pathLength(std::vector<Point> const& pts, float* longest = nullptr)
    {
        float total = 0, most = 0;
        for (size_t i = 1; i < pts.size(); ++i)
        {
            float const d = std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
            total += d, most = std::max(most, d);
        }
        if (longest)
            *longest = most;
        return total;
    }

    // Step 9 for a stroke (the zone, the layer, presence and rate are the caller's): ranges, the line's shape.
    inline Code checkStroke(Stroke const& s, GroundLimits const& l)
    {
        if (s.pts.empty() || (s.kind == Kind::Road && s.pts.size() < 2))
            return Code::Value;
        float longest = 0;
        if (pathLength(s.pts, &longest) > l.maxLength || longest > l.maxSegment)
            return Code::Distance;
        if (s.kind == Kind::Ground)
        {
            if (s.radius < l.paintRadiusMin || s.radius > l.paintRadiusMax || s.strength < l.strengthMin || s.strength > 1 ||
                s.hardness < 0 || s.hardness > 1)
                return Code::Value;
        }
        else if (s.kind == Kind::Height)
        {
            if (s.radius < l.sculptRadiusMin || s.radius > l.sculptRadiusMax || s.strength < l.strengthMin || s.strength > 1)
                return Code::Value;
        }
        return Code::Ok;
    }

    // The op's to_json (flat): the stroke as ef_op stores it and the bake reads it.
    inline Fields strokeFields(Stroke const& s, uint32_t map)
    {
        auto r = [](float v, double k) { return std::round(double(v) * k) / k; };
        Fields o;
        o["map"] = Value::of(map);
        o["g"] = Value::of(s.group);
        o["pts"] = Value::str(encodePath(s.pts));
        if (s.kind == Kind::Ground || s.kind == Kind::Road)
            o["layer"] = Value::str(s.layer);
        if (s.kind == Kind::Road)
            o["class"] = Value::str(s.cls);
        if (s.kind == Kind::Ground || s.kind == Kind::Height)
            o["r"] = Value::of(r(s.radius, 10)), o["s"] = Value::of(r(s.strength, 100));
        if (s.kind == Kind::Ground)
            o["h"] = Value::of(r(s.hardness, 100));
        if (s.kind == Kind::Height)
        {
            o["mode"] = Value::str(sculptName(s.mode));
            if (s.height)
                o["z"] = Value::of(r(*s.height, 10));
        }
        return o;
    }

    inline std::optional<Stroke> strokeOf(Kind kind, Fields const& to)
    {
        if (!isGround(kind))
            return std::nullopt;
        auto num = [&](char const* k) -> std::optional<double> {
            auto it = to.find(k);
            return it != to.end() && it->second.type == Value::Type::Number ? std::optional<double>(it->second.number) : std::nullopt;
        };
        auto str = [&](char const* k) -> std::string {
            auto it = to.find(k);
            return it != to.end() && it->second.type == Value::Type::String ? it->second.text : std::string();
        };
        Stroke s;
        s.kind = kind;
        auto pts = decodePath(str("pts"));
        if (!pts)
            return std::nullopt;
        s.pts = *pts;
        s.group = uint32_t(num("g").value_or(0));
        s.layer = str("layer"), s.cls = str("class");
        s.radius = float(num("r").value_or(0)), s.strength = float(num("s").value_or(1)), s.hardness = float(num("h").value_or(0.5));
        if (kind == Kind::Road)
        {
            auto hw = roadHalfWidth(s.cls);
            if (!hw)
                return std::nullopt;
            s.radius = *hw, s.strength = 1, s.hardness = 1;
        }
        if (kind == Kind::Height)
        {
            auto m = sculptOf(str("mode"));
            if (!m)
                return std::nullopt;
            s.mode = *m;
            if (auto z = num("z"))
                s.height = float(*z);
        }
        else if (!layerName(s.layer))
            return std::nullopt;
        return s;
    }

    // The fields of GND after <map>: the kind and the stroke. `layerToken` is `<tileset>/<layer>` (the client's texture
    // is Tileset\<tileset>\<layer>.blp).
    inline std::vector<std::string> groundFields(Stroke const& s, std::string const& layerToken)
    {
        std::vector<std::string> f{std::string(1, static_cast<char>(s.kind))};
        if (s.kind == Kind::Ground)
            f.insert(f.end(), {layerToken, fixed(s.radius, 1), fixed(s.strength, 2), fixed(s.hardness, 2)});
        else if (s.kind == Kind::Road)
            f.insert(f.end(), {layerToken, s.cls});
        else
            f.insert(f.end(), {sculptName(s.mode), fixed(s.radius, 1), fixed(s.strength, 2), s.height ? fixed(*s.height, 1) : std::string("-")});
        f.push_back(encodePath(s.pts));
        return f;
    }

    // GND: one message per stroke; empty when it would not fit one (the request is refused then).
    inline std::string groundBody(uint32_t cs, uint32_t seq, std::string_view flags, uint32_t map, Stroke const& s,
                                  std::string const& layerToken)
    {
        std::vector<std::string> f{std::to_string(cs), std::to_string(seq), std::string(flags), std::to_string(map)};
        for (std::string& x : groundFields(s, layerToken))
            f.push_back(std::move(x));
        std::string body = eft::body(0, "GND", f);
        return body.size() <= eft::kMaxBody ? body : std::string();
    }

    // The ADT tile of a world position (x north, y west): (east index, south index), as the client's data::tileAt.
    inline std::pair<int32_t, int32_t> tileOf(float x, float y)
    {
        auto clampTile = [](double v) { return int32_t(std::clamp(std::floor(v), 0.0, 63.0)); };
        return {clampTile(32.0 - double(y) / kTileYards), clampTile(32.0 - double(x) / kTileYards)};
    }

    inline uint32_t groupOf(Op const& op)
    {
        auto it = op.to.find("g");
        return isGround(op.kind) && it != op.to.end() && it->second.type == Value::Type::Number ? uint32_t(it->second.number) : 0;
    }

    // UNDO and REDO of a ground line take its whole group: [first, last] of the ops to undo (the newest active op and
    // the active ops of its group directly before it) or to redo (the oldest undone op after the last active one and
    // the undone ops of its group directly after it).
    inline std::optional<std::pair<size_t, size_t>> undoRange(std::vector<Op> const& ops)
    {
        auto last = undoIndex(ops);
        if (!last)
            return std::nullopt;
        size_t first = *last;
        uint32_t const g = groupOf(ops[*last]);
        while (g && first > 0 && !ops[first - 1].undone && ops[first - 1].kind == ops[*last].kind && groupOf(ops[first - 1]) == g)
            --first;
        return std::make_pair(first, *last);
    }

    inline std::optional<std::pair<size_t, size_t>> redoRange(std::vector<Op> const& ops)
    {
        auto first = redoIndex(ops);
        if (!first)
            return std::nullopt;
        size_t last = *first;
        uint32_t const g = groupOf(ops[*first]);
        while (g && last + 1 < ops.size() && ops[last + 1].undone && ops[last + 1].kind == ops[*first].kind && groupOf(ops[last + 1]) == g)
            ++last;
        return std::make_pair(*first, last);
    }

    // The ground strokes a set holds (its active ops of kinds l, r, h).
    inline size_t groundCount(std::vector<Op> const& ops)
    {
        size_t n = 0;
        for (Op const& op : ops)
            n += !op.undone && isGround(op.kind);
        return n;
    }

    // ------------------------------------------------------------------------------------------ places (step 2, §22)

    // Props (p: the trees, rocks and clutter of a zone's placements.yaml `trees` and `clutter`) and kit buildings (b: its
    // `buildings`, kit models at scale 1): placed, moved, turned, scaled (props only) and removed. Like the ground,
    // nothing on the server changes: the realm validates and stores the ops, streams what each set makes of each object
    // (PLACE) by tile, and the clients draw it over the release (a prop as an M2, a building as a WMO, the release's own
    // instance hidden by its position) until a release bakes it into placements.yaml (Emberfall tools/py
    // emberfall_content/editor/placements.py). The server's collision, paths and line of sight stay the release's. The
    // rules are written down in docs/design/world-editor.md §22.
    //
    //   C->S  PADD;<k>;<model>;<x>;<y>;<z>;<yaw>;<size>   PMOVE;<k>;<ref>;<x>;<y>;<z>;<yaw>;<size>   PDEL;<k>;<ref>
    //   S->C  PLACE;<cs>;<k>;<id>;<flags>;<map>;<path|->;<x>;<y>;<z>;<o>;<size>;<hx|->;<hy|->
    //         PLACEX;<cs>;<k>;<id>   (PLACEX;*;*;* first at login and on a new map: forget every place)
    //         PMOD;<p>/<t>;<k>,<model>,<path|->,<variant>|...   (the models an editor may place, after ROLE)
    // `model`: a whitelist key (doodads.yaml's `<category>/<name>` for a prop, the kit model for a building). `ref`: the
    // object, `<id>` as PLACE names it (an object a set added, or a release object a set changed), or `@<x>,<y>`: the
    // release object the client draws there (its MDDF/MODF position; the realm takes the row within kMatchReach). `yaw`
    // in degrees as placements.yaml has it (a prop's: its glTF +Z from south towards east; a building's: its door
    // clockwise from north); `size` a prop's scale (a building's is 1). PLACE's `path`: the model's game path with '/'
    // and no extension (.m2 for p, .wmo for b; `-`: nothing shown, a set removes the object), x, y, z and `o`
    // (efterrain's orientation, degrees counter-clockwise from north) where it stands, hx and hy the release instance the
    // clients hide (`-`: none, a new object). Flags: d a contributor's draft (only editors with the editor open get
    // those), m the viewer's own. A client shows, of the records of one object (k, id), the one of the highest set.
    // PMOD's `path` is `-` for a prop at World/Doodads/<model>; `variant` the one to preview (0: the model has none).

    constexpr size_t kMaxModelKey = 48;
    constexpr float kMatchReach = 0.6f;

    inline bool isPlace(Kind k) { return k == Kind::Prop || k == Kind::Building; }
    inline bool isPlace(char k) { return k == 'p' || k == 'b'; }
    // The kinds the server's world never holds (ground strokes, props, buildings): nothing applied, no marker, no copy.
    inline bool isOverlay(Kind k) { return isGround(k) || isPlace(k); }

    inline std::optional<Kind> placeKindOf(std::string_view s)
    {
        if (s == "p")
            return Kind::Prop;
        if (s == "b")
            return Kind::Building;
        return std::nullopt;
    }

    // A whitelist key: lower-case letters, digits, '-' and '_', at most one '/' between two names.
    inline bool modelKey(std::string_view s)
    {
        if (s.empty() || s.size() > kMaxModelKey || s.front() == '/' || s.back() == '/')
            return false;
        size_t slashes = 0;
        for (char c : s)
        {
            if (c == '/')
                ++slashes;
            else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
                return false;
        }
        return slashes <= 1;
    }

    struct PlaceRef
    {
        uint32_t id = 0;          // an object PLACE named
        bool at = false;          // or the release object at (x, y)
        float x = 0, y = 0;
    };

    inline std::optional<PlaceRef> parseRef(std::string_view s)
    {
        PlaceRef r;
        if (!s.empty() && s[0] == '@')
        {
            auto xy = eft::split(s.substr(1), ',');
            if (xy.size() != 2)
                return std::nullopt;
            auto x = decimal(xy[0], 20000), y = decimal(xy[1], 20000);
            if (!x || !y)
                return std::nullopt;
            r.at = true, r.x = *x, r.y = *y;
            return r;
        }
        auto id = eft::number(s, kTempIdMax);
        if (!id || *id == 0)
            return std::nullopt;
        r.id = *id;
        return r;
    }

    struct PlaceRequest
    {
        Kind kind = Kind::Prop;
        std::string model;        // PADD
        PlaceRef ref;             // PMOVE, PDEL
        Pos pos;                  // x, y, z (o unused)
        float yaw = 0, size = 1;
    };

    // Degrees in [0, 360) to one decimal (placements.yaml's precision).
    inline float normaliseYaw(float yaw)
    {
        double y = std::fmod(double(yaw), 360.0);
        if (y < 0)
            y += 360.0;
        y = std::round(y * 10.0) / 10.0;
        return y >= 360.0 ? 0.0f : float(y);
    }

    // x, y, z, yaw, size at f[at .. at + 4]
    inline bool placeWhere(std::vector<std::string> const& f, size_t at, PlaceRequest& r)
    {
        auto x = decimal(f[at], 20000), y = decimal(f[at + 1], 20000), z = decimal(f[at + 2], 5000);
        auto yaw = decimal(f[at + 3], 100000), size = decimal(f[at + 4], 10);
        if (!x || !y || !z || !yaw || !size || *size <= 0)
            return false;
        r.pos = Pos{*x, *y, *z, 0};
        r.yaw = normaliseYaw(*yaw);
        r.size = std::round(*size * 100.0f) / 100.0f;
        return r.size > 0;
    }

    inline std::optional<PlaceRequest> parsePlaceAdd(std::vector<std::string> const& f)
    {
        if (f.size() != 7)
            return std::nullopt;
        auto k = placeKindOf(f[0]);
        if (!k || !modelKey(f[1]))
            return std::nullopt;
        PlaceRequest r;
        r.kind = *k, r.model = f[1];
        if (!placeWhere(f, 2, r))
            return std::nullopt;
        return r;
    }

    inline std::optional<PlaceRequest> parsePlaceMove(std::vector<std::string> const& f)
    {
        if (f.size() != 7)
            return std::nullopt;
        auto k = placeKindOf(f[0]);
        auto ref = parseRef(f[1]);
        if (!k || !ref)
            return std::nullopt;
        PlaceRequest r;
        r.kind = *k, r.ref = *ref;
        if (!placeWhere(f, 2, r))
            return std::nullopt;
        return r;
    }

    inline std::optional<PlaceRequest> parsePlaceDel(std::vector<std::string> const& f)
    {
        if (f.size() != 2)
            return std::nullopt;
        auto k = placeKindOf(f[0]);
        auto ref = parseRef(f[1]);
        if (!k || !ref)
            return std::nullopt;
        PlaceRequest r;
        r.kind = *k, r.ref = *ref;
        return r;
    }

    struct PlaceLimits
    {
        uint32_t propsPerSet = 300, buildingsPerSet = 3;   // objects a set touches (§9.1)
        float sizeMin = 0.5f, sizeMax = 2.0f;              // a contributor's prop scale; a maintainer's below
        float sizeMinMaintainer = 0.1f, sizeMaxMaintainer = 5.0f;
        uint32_t perMinute = 60, perDay = 1000;            // per account (reviewers x10, maintainers none)
        uint32_t realmCap = 20000;                         // objects the realm shows waiting for a release, every set
    };

    // A prop's scale within its range; a kit building stands at scale 1 only.
    inline Code checkPlaceSize(Kind k, float size, Level level, PlaceLimits const& l)
    {
        if (!std::isfinite(size))
            return Code::Value;
        if (k == Kind::Building)
            return std::fabs(size - 1.0f) < 0.001f ? Code::Ok : Code::Value;
        bool const m = level == Level::Maintainer;
        return size >= (m ? l.sizeMinMaintainer : l.sizeMin) - 1e-4f && size <= (m ? l.sizeMaxMaintainer : l.sizeMax) + 1e-4f
                   ? Code::Ok : Code::Value;
    }

    // A zone's design frame (zone.yaml `frame.origin`, `adt_size`): design (u east, v south, yards) <-> world (x north,
    // y west) as doodads.py design_to_world. A prop stands at its tile's centre (u + 0.5, v + 0.5); a building's u, v is
    // its footprint's centre.
    struct Frame
    {
        double ou = 0, ov = 0, tile = 1600.0 / 3.0;
    };

    inline double round2(double v) { return std::round(v * 100.0) / 100.0; }

    inline std::pair<double, double> worldOf(Frame const& f, double u, double v, Kind k)
    {
        double const c = k == Kind::Prop ? 0.5 : 0.0;
        return {f.tile - (v + c + f.ov), f.tile - (u + c + f.ou)};
    }

    inline std::pair<double, double> designOf(Frame const& f, double x, double y, Kind k)
    {
        double const c = k == Kind::Prop ? 0.5 : 0.0;
        return {round2(f.tile - y - f.ou - c), round2(f.tile - x - f.ov - c)};
    }

    // efterrain's orientation (degrees counter-clockwise from north) of a placements.yaml yaw: a prop's is yaw + 180
    // (doodads.py prop_orientation), a building's -yaw (wmo/__main__.py).
    inline double orientationOf(Kind k, double yaw)
    {
        double o = std::fmod(k == Kind::Prop ? yaw + 180.0 : -yaw, 360.0);
        if (o < 0)
            o += 360.0;
        return o;
    }

    // zlib's CRC-32 (doodads.py variant_of hashes the position with it).
    inline uint32_t crc32(std::string_view s)
    {
        uint32_t c = 0xFFFFFFFFu;
        for (char ch : s)
        {
            c ^= static_cast<unsigned char>(ch);
            for (int k = 0; k < 8; ++k)
                c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
        return ~c;
    }

    // Which of a model's `count` variants a prop at design (u, v) stands as (1..count; 0 without variants): the release's
    // rule (doodads.py variant_of), so the preview is what the bake builds.
    inline uint32_t variantOf(double u, double v, uint32_t count)
    {
        if (count == 0)
            return 0;
        char buffer[64];
        std::snprintf(buffer, sizeof buffer, "%.3f,%.3f", u, v);
        return crc32(buffer) % count + 1;
    }

    // The variant the clients can draw: the hashed one when their data has it (`built`: bit k set for variant k, the
    // variants some release placement stands as), else the lowest built one (the bake builds the hashed one).
    inline uint32_t shownVariant(double u, double v, uint32_t count, uint32_t built)
    {
        uint32_t const want = variantOf(u, v, count);
        if (want == 0 || want > 31 || (built >> want) & 1u)
            return want;
        for (uint32_t k = 1; k <= count && k <= 31; ++k)
            if ((built >> k) & 1u)
                return k;
        return want;
    }

    inline std::string variantPath(std::string const& base, uint32_t variant)
    {
        return variant ? base + "-v" + std::to_string(variant) : base;
    }

    // An object as placements.yaml has it (model, u, v, yaw, size; a building's name), and where the realm stood it (z,
    // its map).
    struct PlaceState
    {
        bool present = false;
        std::string model, name;
        double u = 0, v = 0, yaw = 0, size = 1;
        double z = 0;
        uint32_t map = 0;
    };

    inline Fields placeFields(Kind k, PlaceState const& s)
    {
        Fields o;
        o["model"] = Value::str(s.model);
        o["u"] = Value::of(round2(s.u));
        o["v"] = Value::of(round2(s.v));
        o["yaw"] = Value::of(std::round(s.yaw * 10.0) / 10.0);
        o["z"] = Value::of(round2(s.z));
        o["map"] = Value::of(s.map);
        if (k == Kind::Prop)
            o["size"] = Value::of(round2(s.size));
        else if (!s.name.empty())
            o["name"] = Value::str(s.name);
        return o;
    }

    inline std::optional<PlaceState> placeStateOf(Fields const& f)
    {
        auto num = [&](char const* k) -> std::optional<double> {
            auto it = f.find(k);
            return it != f.end() && it->second.type == Value::Type::Number ? std::optional<double>(it->second.number) : std::nullopt;
        };
        auto model = f.find("model");
        auto u = num("u"), v = num("v");
        if (model == f.end() || model->second.type != Value::Type::String || !modelKey(model->second.text) || !u || !v)
            return std::nullopt;
        PlaceState s;
        s.present = true;
        s.model = model->second.text;
        s.u = *u, s.v = *v;
        s.yaw = num("yaw").value_or(0), s.size = num("size").value_or(1), s.z = num("z").value_or(0);
        s.map = uint32_t(num("map").value_or(0));
        if (auto n = f.find("name"); n != f.end() && n->second.type == Value::Type::String)
            s.name = n->second.text;
        return s;
    }

    // The same object in the same place (the files' precisions: u, v 2 decimals, yaw 1, size 2).
    inline bool samePlace(PlaceState const& a, PlaceState const& b)
    {
        if (a.present != b.present)
            return false;
        if (!a.present)
            return true;
        double const turn = std::fabs(std::fmod(a.yaw - b.yaw + 540.0, 360.0) - 180.0);
        return a.model == b.model && std::fabs(a.u - b.u) < 0.006 && std::fabs(a.v - b.v) < 0.006 && turn < 0.06 &&
               std::fabs(a.size - b.size) < 0.006;
    }

    // What a set's ops make of one object (its ops in seq order): from the first active op's `from` (nothing before an
    // add), each active op's `to` over it (a delete: gone).
    struct PlaceFold
    {
        bool touched = false;   // an active op
        PlaceState before, after;
    };

    inline PlaceFold foldPlace(std::vector<Op> const& ops)
    {
        PlaceFold out;
        for (Op const& op : ops)
        {
            if (op.undone)
                continue;
            if (!out.touched)
            {
                out.touched = true;
                if (op.op != OpKind::Add)
                    if (auto b = placeStateOf(op.from))
                        out.before = *b;
                out.after = out.before;
            }
            if (op.op == OpKind::Delete)
                out.after.present = false;
            else if (auto t = placeStateOf(op.to))
                out.after = *t;
        }
        return out;
    }

    // The objects of kind k a set touches (its caps, §9.1).
    inline size_t placeCount(std::vector<Op> const& ops, Kind k)
    {
        std::set<uint32_t> ids;
        for (Op const& op : ops)
            if (!op.undone && op.kind == k)
                ids.insert(op.target);
        return ids.size();
    }

    struct PlaceShow
    {
        uint32_t cs = 0, id = 0, map = 0;
        Kind kind = Kind::Prop;
        std::string flags = "-", path;   // path empty: nothing shown
        float x = 0, y = 0, z = 0, o = 0, size = 1;
        std::optional<std::pair<float, float>> hide;
    };

    // PLACE: one message per object; empty when it would not fit one.
    inline std::string placeBody(PlaceShow const& s)
    {
        std::vector<std::string> f{std::to_string(s.cs), std::string(1, static_cast<char>(s.kind)), std::to_string(s.id),
                                   s.flags.empty() ? std::string("-") : s.flags, std::to_string(s.map),
                                   s.path.empty() ? std::string("-") : s.path, fixed(s.x, 2), fixed(s.y, 2), fixed(s.z, 2),
                                   fixed(s.o, 1), fixed(s.size, 2), s.hide ? fixed(s.hide->first, 2) : std::string("-"),
                                   s.hide ? fixed(s.hide->second, 2) : std::string("-")};
        std::string body = eft::body(0, "PLACE", f);
        return body.size() <= eft::kMaxBody ? body : std::string();
    }

    // PLACE's fields after its cmd (the client reads them the same way; here for the tests and the fuzz).
    inline std::optional<PlaceShow> decodePlace(std::vector<std::string> const& f)
    {
        if (f.size() != 13)
            return std::nullopt;
        PlaceShow s;
        auto cs = eft::number(f[0]), id = eft::number(f[2]), map = eft::number(f[4], 10000);
        auto k = placeKindOf(f[1]);
        auto x = decimal(f[6], 20000), y = decimal(f[7], 20000), z = decimal(f[8], 5000), o = decimal(f[9], 360), size = decimal(f[10], 10);
        if (!cs || !id || !map || !k || !x || !y || !z || !o || !size || f[3].size() > 4 || f[5].size() > 96)
            return std::nullopt;
        s.cs = *cs, s.id = *id, s.map = *map, s.kind = *k, s.flags = f[3], s.x = *x, s.y = *y, s.z = *z, s.o = *o, s.size = *size;
        if (f[5] != "-")
        {
            for (char c : f[5])
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == '-' || c == '_'))
                    return std::nullopt;
            s.path = f[5];
        }
        if (f[11] != "-" || f[12] != "-")
        {
            auto hx = decimal(f[11], 20000), hy = decimal(f[12], 20000);
            if (!hx || !hy)
                return std::nullopt;
            s.hide = std::make_pair(*hx, *hy);
        }
        return s;
    }

    // The ADT tile a place is streamed by: where it stands, or (removed) where the release instance stood.
    inline std::pair<int32_t, int32_t> placeTile(PlaceShow const& s)
    {
        if (s.path.empty() && s.hide)
            return tileOf(s.hide->first, s.hide->second);
        return tileOf(s.x, s.y);
    }
}
