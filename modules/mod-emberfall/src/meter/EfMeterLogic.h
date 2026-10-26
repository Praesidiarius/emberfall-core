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

// The damage meter without the core (Emberfall docs/design/damage-meter.md): one Meter per group (or solo player)
// tallies what its participants do, fight by fight; the EFM message bodies the clients get. src/meter/EfMeter.cpp
// feeds it from the core's combat log (Emberfall::CombatLog) and sends the bodies; tests/meter_tests.cpp checks it.
//
// Participants: players and Emberfall companions have rows; pets, guardians and totems are attributed to their owner
// (their own pid is the "source" of the owner's spell rows: the per-pet breakdown). Pids are small numbers (1..250)
// a meter hands out once and keeps until it is dropped, so clients learn a name (WHO) once.
//
// Segments: the current fight starts with the first hostile damage a participant deals or takes (a miss counts) and
// ends when no participant has been in combat for kGraceMs (or nothing happened for kIdleMs; a fight nobody was in
// combat in, at a dummy that never enters combat, after kNoCombatGapMs without an event). Its duration runs from that
// first event to the last one. The last kHistory fights are kept, and "overall" (id 0): every fight since the
// last reset, its duration the fights' durations added up. Nothing outside a fight is counted (a heal before the pull,
// falling damage on the road), so overall is the sum of the fights.
//
// Protocol (an addon whisper between the player and themselves, prefix "EFM\t"; numbers: ids, pids, classes, seq in
// decimal, amounts and times in base 36; names have ; | , replaced; a body is at most 251 bytes):
//   client -> server  1;<seq>;HELLO                 1;<seq>;VIEW;<segment>   (<segment>: an id, 0 overall, c current)
//                     1;<seq>;SPELLS;<segment>;<pid>;<metric d|h|t|r|i|p>     1;<seq>;DEATHS;<segment>;<pid>
//                     1;<seq>;RESET
//   server -> client  1;0;ON;<epoch>  1;0;OFF       a new meter (login, a group joined or left, a reset): forget all
//                     1;0;WHO;<pid>;<kind p|c|t>;<class>;<owner pid>;<name>
//                     1;0;LIST;<id>,<id>,...        the fights kept, newest first (the live one included)
//                     1;0;SEG;<id>;<flags 1 live, 2 boss>;<duration ms>;<name>
//                     1;0;SUM;<id>;<row>|<row>...   row: pid,damage,healing,overheal,absorbs,taken,received,
//                                                   interrupts,dispels,deaths,active ms (only rows changed since the
//                                                   client's last SUM of that segment)
//                     1;0;SPL;<seq>;<id>;<pid>;<metric>;<row>|...   row: source pid,spell,hits,crits,misses,total,
//                                                   over,absorbed,max,min
//                     1;0;DTH;<seq>;<id>;<pid>;<n>;<time ms>;<spell>;<killer>
//                     1;0;DEV;<seq>;<n>;<row>|...   row: ms before the death,spell,amount (- damage),health %,
//                                                   flags (1 crit),source
//                     1;0;END;<seq>   1;0;OK;<seq>   1;0;ERR;<seq>;<code>

#ifndef EMBERFALL_METER_LOGIC_H
#define EMBERFALL_METER_LOGIC_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Emberfall::Meter
{
    constexpr std::string_view kPrefix = "EFM\t";
    constexpr std::uint32_t kVersion = 1;
    constexpr std::uint32_t kPushMs = 1000;        // a client gets what changed at most this often
    constexpr std::uint32_t kGraceMs = 3000;       // every participant out of combat this long: the fight is over
    constexpr std::uint32_t kNoCombatGapMs = 8000; // a fight nobody was in combat in (a training dummy that never enters
                                                   // combat): over after this long without an event, not kGraceMs (a slow
                                                   // swing, a long cast or a 3 s DoT tick left alone split it before)
    constexpr std::uint32_t kIdleMs = 20000;       // nothing happened this long: over even if someone is still in combat
    constexpr std::uint32_t kCombatStaleMs = 5000; // a combat report this old does not count (the player left the map)
    constexpr std::uint32_t kActiveGapMs = 5000;   // a pause up to this long between two actions is active time (a slow cast)
    constexpr std::size_t kHistory = 10;           // fights kept besides overall
    constexpr std::size_t kMaxRows = 40;           // players and companions (raid size)
    constexpr std::uint8_t kMaxPid = 250;          // rows and pets
    constexpr std::size_t kRecapEvents = 5;        // a death recap's hits and heals
    constexpr std::uint32_t kRecapWindowMs = 15000;
    constexpr std::size_t kDeathsKept = 10;        // per participant and fight (overall: the last 3 x that)
    constexpr std::size_t kMaxBody = 251;          // 255 with the prefix
    constexpr std::size_t kNameMax = 40;
    constexpr std::uint32_t kEnvSpellBase = 2000000000u; // environmental damage: this + the 3.3.5 EnviromentalDamage type
    constexpr std::uint32_t kRequestsPer10s = 30;

    enum Metric : std::uint8_t
    {
        kDamage,     // d: damage done (to health, overkill included, plus what the target's shields absorbed)
        kHealing,    // h: healing done (effective) and absorbs provided (shield spells)
        kTaken,      // t: damage taken (absorbed included; environmental damage by its type)
        kReceived,   // r: healing received (effective) and absorbs on oneself
        kInterrupts, // i: interrupts, by the interrupting spell
        kDispels,    // p: auras dispelled or stolen, by the dispelling spell
        kMetrics
    };

    inline char MetricLetter(Metric m) { return "dhtrip"[m < kMetrics ? m : 0]; }

    inline std::optional<Metric> ParseMetric(std::string_view s)
    {
        if (s.size() != 1)
            return std::nullopt;
        std::string_view const letters = "dhtrip";
        std::size_t const i = letters.find(s[0]);
        if (i == std::string_view::npos)
            return std::nullopt;
        return Metric(i);
    }

    enum class Kind : char
    {
        Player = 'p',
        Companion = 'c',
        Pet = 't',
    };

    // ---- numbers and names on the wire --------------------------------------------------------------------------------
    inline std::string B36(std::uint64_t v)
    {
        if (v == 0)
            return "0";
        char buf[16];
        int n = 0;
        while (v)
        {
            buf[n++] = "0123456789abcdefghijklmnopqrstuvwxyz"[v % 36];
            v /= 36;
        }
        std::string out;
        out.reserve(std::size_t(n) + 1);
        while (n)
            out.push_back(buf[--n]);
        return out;
    }

    inline std::string SignedB36(std::int64_t v) { return v < 0 ? "-" + B36(std::uint64_t(-v)) : B36(std::uint64_t(v)); }

    inline std::optional<std::uint64_t> ParseB36(std::string_view s)
    {
        if (s.empty() || s.size() > 12)
            return std::nullopt;
        std::uint64_t v = 0;
        for (char c : s)
        {
            int d;
            if (c >= '0' && c <= '9')
                d = c - '0';
            else if (c >= 'a' && c <= 'z')
                d = c - 'a' + 10;
            else
                return std::nullopt;
            v = v * 36 + std::uint64_t(d);
        }
        return v;
    }

    inline std::optional<std::uint64_t> ParseDec(std::string_view s)
    {
        if (s.empty() || s.size() > 10)
            return std::nullopt;
        std::uint64_t v = 0;
        for (char c : s)
        {
            if (c < '0' || c > '9')
                return std::nullopt;
            v = v * 10 + std::uint64_t(c - '0');
        }
        return v;
    }

    // A name fit for the wire: the separators and control characters become spaces, at most kNameMax bytes (cut on a
    // UTF-8 character boundary).
    inline std::string Clean(std::string_view name)
    {
        std::string out;
        out.reserve(std::min(name.size(), kNameMax));
        for (char c : name)
        {
            unsigned char const u = static_cast<unsigned char>(c);
            out.push_back((c == ';' || c == '|' || c == ',' || u < 0x20 || u == 0x7f) ? ' ' : c);
        }
        if (out.size() > kNameMax)
        {
            std::size_t cut = kNameMax;
            while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80)
                --cut;
            out.resize(cut);
        }
        return out;
    }

    // ---- the tallies ----------------------------------------------------------------------------------------------------
    // One spell of one participant (or one of its pets) in one metric.
    struct SpellStats
    {
        std::uint32_t hits = 0, crits = 0, misses = 0;
        std::uint64_t total = 0;     // what counts (damage: to health + absorbed; healing: effective)
        std::uint64_t over = 0;      // overkill, overheal
        std::uint64_t absorbed = 0;  // damage: what the target's shields took; healing: what a heal absorb took
        std::uint32_t max = 0, min = 0;

        void Add(std::uint64_t amount, bool crit, std::uint64_t overAmount = 0, std::uint64_t absorbedAmount = 0)
        {
            ++hits;
            if (crit)
                ++crits;
            total += amount;
            over += overAmount;
            absorbed += absorbedAmount;
            std::uint32_t const a = std::uint32_t(std::min<std::uint64_t>(amount, 0xFFFFFFFFu));
            max = std::max(max, a);
            min = hits == 1 ? a : std::min(min, a);
        }
    };

    struct Totals
    {
        std::uint64_t damage = 0, overkill = 0, absorbedByTarget = 0;
        std::uint64_t healing = 0, overheal = 0, absorbs = 0;
        std::uint64_t taken = 0, environmental = 0, received = 0;
        std::uint32_t interrupts = 0, dispels = 0, deaths = 0;
        std::uint64_t activeMs = 0;
    };

    // One hit or heal before a death, as the recap shows it.
    struct RecapEvent
    {
        std::int64_t atMs = 0;       // the meter's clock (in a Death: ms before the death, <= 0)
        std::uint32_t spell = 0;
        std::int64_t amount = 0;     // negative: damage (absorbed included), positive: effective healing
        std::uint8_t hpPct = 0;      // health after it
        bool crit = false;
        std::string source;
    };

    struct Death
    {
        std::uint64_t timeMs = 0;    // since the fight began
        std::uint32_t spell = 0;     // the killing blow's spell (the last damage in the recap)
        std::string killer;
        std::vector<RecapEvent> events; // oldest first, atMs relative to the death
    };

    using SpellKey = std::uint64_t; // source pid << 32 | spell

    inline SpellKey Key(std::uint8_t source, std::uint32_t spell) { return (SpellKey(source) << 32) | spell; }

    // One participant in one segment.
    struct Row
    {
        Totals t;
        std::array<std::map<SpellKey, SpellStats>, kMetrics> spells;
        std::vector<Death> deaths;
        std::uint64_t lastActiveMs = 0;
        std::uint32_t version = 0;   // the segment's version when it last changed
    };

    struct Segment
    {
        std::uint32_t id = 0;        // 0: overall
        std::uint64_t startMs = 0, lastEventMs = 0;
        bool live = false, boss = false;
        bool sawCombat = false;      // a participant was in combat during the fight (else kNoCombatGapMs ends it)
        std::string name;
        std::map<std::uint8_t, Row> rows;
        std::uint32_t version = 0;
        std::uint64_t closedMs = 0;  // overall: the finished fights' durations

        // A fight: first to last event (at least 1 s); live, up to now. Overall: closedMs plus the live fight's.
        std::uint64_t DurationMs(std::uint64_t nowMs) const
        {
            if (!live)
                return std::max<std::uint64_t>(lastEventMs - startMs, 1000);
            return std::max<std::uint64_t>((nowMs > startMs ? nowMs : startMs) - startMs, 1000);
        }
    };

    struct Participant
    {
        std::uint8_t pid = 0;
        std::uint64_t key = 0;       // the GUID (players, companions) or a pet's owner and entry
        Kind kind = Kind::Player;
        std::uint8_t cls = 0;        // the class (ChrClasses ID; companions: their unit class)
        std::uint8_t owner = 0;      // a pet's owner's pid
        std::string name;
    };

    // The other side of a hostile event: who a fight is named after.
    struct Enemy
    {
        std::string_view name;
        bool boss = false;
    };

    struct Hit
    {
        std::uint32_t spell = 0;     // 0: melee swing
        std::uint32_t amount = 0;    // to health, overkill included
        std::uint32_t overkill = 0;
        std::uint32_t absorbed = 0;
        bool crit = false;
        bool avoided = false;        // a miss, dodge, parry, full block, evade, immune, resist, reflect
    };

    struct Heal
    {
        std::uint32_t spell = 0;
        std::uint32_t amount = 0;    // the heal, overheal included
        std::uint32_t overheal = 0;
        std::uint32_t absorbed = 0;  // what a heal absorb took (not in amount)
        bool crit = false;

        std::uint32_t Effective() const { return amount > overheal ? amount - overheal : 0; }
    };

    // The overheal maths of a heal that met `missing` health: effective min(heal, missing), the rest overheal.
    inline Heal SplitHeal(std::uint32_t spell, std::uint32_t heal, std::uint32_t missing, bool crit = false)
    {
        Heal h;
        h.spell = spell;
        h.amount = heal;
        h.overheal = heal > missing ? heal - missing : 0;
        h.crit = crit;
        return h;
    }

    // A hit's overkill: what went past the target's health (the 3.3.5 combat log's number).
    inline std::uint32_t Overkill(std::uint32_t damage, std::uint32_t health) { return damage > health ? damage - health : 0; }

    inline std::uint8_t Pct(std::uint64_t part, std::uint64_t whole)
    {
        if (!whole)
            return 0;
        return std::uint8_t(std::min<std::uint64_t>(100, (part * 100 + whole / 2) / whole));
    }

    class Meter
    {
    public:
        std::uint32_t epoch = 0;     // set by the owner (unique among meters): clients forget everything when it changes
        std::vector<Participant> roster;
        std::uint32_t rosterVersion = 0;
        std::deque<Segment> fights;  // newest first; fights[0] may be live
        Segment overall;
        std::uint32_t listVersion = 0;
        std::uint64_t lastTouchMs = 0;

        // The pid for a key (a new one the first time; 0 when the meter is full: the participant is not counted).
        std::uint8_t PidFor(std::uint64_t key, Kind kind, std::uint8_t cls, std::uint8_t owner, std::string_view name)
        {
            for (Participant& p : roster)
                if (p.key == key && p.kind == kind)
                {
                    std::string const clean = Clean(name);
                    if (p.name != clean || p.cls != cls)
                    {
                        p.name = clean;
                        p.cls = cls;
                        ++rosterVersion;
                    }
                    return p.pid;
                }
            if (roster.size() >= kMaxPid)
                return 0;
            if (kind != Kind::Pet && Rows() >= kMaxRows)
                return 0;
            Participant p;
            p.pid = std::uint8_t(roster.size() + 1);
            p.key = key;
            p.kind = kind;
            p.cls = cls;
            p.owner = owner;
            p.name = Clean(name);
            roster.push_back(p);
            ++rosterVersion;
            return p.pid;
        }

        Participant const* Who(std::uint8_t pid) const { return pid && pid <= roster.size() ? &roster[pid - 1] : nullptr; }

        std::size_t Rows() const
        {
            return std::size_t(std::count_if(roster.begin(), roster.end(), [](Participant const& p) { return p.kind != Kind::Pet; }));
        }

        // ---- fights ------------------------------------------------------------------------------------------------
        Segment* Live() { return !fights.empty() && fights.front().live ? &fights.front() : nullptr; }
        Segment const* Live() const { return !fights.empty() && fights.front().live ? &fights.front() : nullptr; }
        // The live fight, else the last one (nullptr before the first).
        Segment const* Current() const { return fights.empty() ? nullptr : &fights.front(); }

        Segment const* Find(std::uint32_t id) const
        {
            if (id == 0)
                return &overall;
            for (Segment const& s : fights)
                if (s.id == id)
                    return &s;
            return nullptr;
        }

        std::uint64_t OverallMs(std::uint64_t nowMs) const
        {
            Segment const* live = Live();
            return overall.closedMs + (live ? live->DurationMs(nowMs) : 0);
        }

        // A participant's combat flag, reported by its player's update.
        void SetCombat(std::uint8_t pid, bool inCombat, std::uint64_t nowMs)
        {
            if (!pid)
                return;
            touch(nowMs);
            auto& c = _combat[pid];
            c.inCombat = inCombat;
            c.seenMs = nowMs;
            if (inCombat)
            {
                _lastCombatMs = nowMs;
                if (Segment* live = Live())
                    live->sawCombat = true;
            }
            Tick(nowMs);
        }

        // Leaving (logout, the group): its combat flag no longer holds the fight open.
        void Forget(std::uint8_t pid) { _combat.erase(pid); }

        bool AnyInCombat(std::uint64_t nowMs) const
        {
            for (auto const& [pid, c] : _combat)
                if (c.inCombat && nowMs - std::min(nowMs, c.seenMs) < kCombatStaleMs)
                    return true;
            return false;
        }

        // Ends the live fight when it is over; true when it ended now.
        bool Tick(std::uint64_t nowMs)
        {
            Segment* live = Live();
            if (!live)
                return false;
            std::uint64_t const gap = live->sawCombat ? kGraceMs : kNoCombatGapMs;
            bool const quiet = !AnyInCombat(nowMs) && nowMs >= _lastCombatMs + kGraceMs && nowMs >= live->lastEventMs + gap;
            bool const idle = nowMs >= live->lastEventMs + kIdleMs;
            if (!quiet && !idle)
                return false;
            live->live = false;
            ++live->version;
            overall.closedMs += live->DurationMs(nowMs);
            ++overall.version;
            ++listVersion;
            return true;
        }

        void Reset(std::uint32_t newEpoch)
        {
            fights.clear();
            overall = Segment();
            _recent.clear();
            _combat.clear();
            epoch = newEpoch;
            ++listVersion;
        }

        // ---- events -----------------------------------------------------------------------------------------------
        // `pid` dealt a hit (or missed) to an enemy; `source` is the pet that did it (0: the participant itself).
        void DamageDone(std::uint64_t nowMs, std::uint8_t pid, std::uint8_t source, Hit const& h, Enemy const& target)
        {
            if (!pid || !Begin(nowMs, &target))
                return;
            Each(nowMs, pid, [&](Row& r) {
                SpellStats& s = r.spells[kDamage][Key(source, h.spell)];
                if (h.avoided)
                {
                    ++s.misses;
                    return;
                }
                std::uint64_t const counted = std::uint64_t(h.amount) + h.absorbed;
                s.Add(counted, h.crit, h.overkill, h.absorbed);
                r.t.damage += counted;
                r.t.overkill += h.overkill;
                r.t.absorbedByTarget += h.absorbed;
                Active(r, nowMs);
            });
        }

        // `pid` took a hit. `enemy`: set when it came from an enemy (starts a fight); none for friendly fire and the
        // environment. `environmental`: falling, lava, drowning (h.spell is kEnvSpellBase + the type).
        void DamageTaken(std::uint64_t nowMs, std::uint8_t pid, Hit const& h, std::string_view source, Enemy const* enemy,
                         std::uint8_t hpPctAfter, bool environmental = false)
        {
            if (!pid || !Begin(nowMs, enemy))
                return;
            std::uint64_t const counted = h.avoided ? 0 : std::uint64_t(h.amount) + h.absorbed;
            Each(nowMs, pid, [&](Row& r) {
                SpellStats& s = r.spells[kTaken][Key(0, h.spell)];
                if (h.avoided)
                {
                    ++s.misses;
                    return;
                }
                s.Add(counted, h.crit, h.overkill, h.absorbed);
                r.t.taken += counted;
                if (environmental)
                    r.t.environmental += counted;
            });
            if (!h.avoided)
                Remember(nowMs, pid, h.spell, -std::int64_t(counted), hpPctAfter, h.crit, source);
        }

        void HealDone(std::uint64_t nowMs, std::uint8_t pid, std::uint8_t source, Heal const& h)
        {
            if (!pid || !Begin(nowMs, nullptr))
                return;
            Each(nowMs, pid, [&](Row& r) {
                r.spells[kHealing][Key(source, h.spell)].Add(h.Effective(), h.crit, h.overheal, h.absorbed);
                r.t.healing += h.Effective();
                r.t.overheal += h.overheal;
                Active(r, nowMs);
            });
        }

        void HealReceived(std::uint64_t nowMs, std::uint8_t pid, Heal const& h, std::string_view source, std::uint8_t hpPctAfter)
        {
            if (!pid || !Begin(nowMs, nullptr))
                return;
            Each(nowMs, pid, [&](Row& r) {
                r.spells[kReceived][Key(0, h.spell)].Add(h.Effective(), h.crit, h.overheal, h.absorbed);
                r.t.received += h.Effective();
            });
            if (h.Effective())
                Remember(nowMs, pid, h.spell, std::int64_t(h.Effective()), hpPctAfter, h.crit, source);
        }

        // `pid`'s shield (spell) absorbed `amount` of damage on someone.
        // `enemy`: the attacker whose hit the shield took, when hostile (the absorb comes before the hit's own log, so it
        // may start the fight).
        void AbsorbDone(std::uint64_t nowMs, std::uint8_t pid, std::uint8_t source, std::uint32_t spell, std::uint32_t amount,
                        Enemy const* enemy = nullptr)
        {
            if (!pid || !amount || !Begin(nowMs, enemy))
                return;
            Each(nowMs, pid, [&](Row& r) {
                r.spells[kHealing][Key(source, spell)].Add(amount, false);
                r.t.absorbs += amount;
                Active(r, nowMs);
            });
        }

        void AbsorbReceived(std::uint64_t nowMs, std::uint8_t pid, std::uint32_t spell, std::uint32_t amount, Enemy const* enemy = nullptr)
        {
            if (!pid || !amount || !Begin(nowMs, enemy))
                return;
            Each(nowMs, pid, [&](Row& r) {
                r.spells[kReceived][Key(0, spell)].Add(amount, false);
                r.t.received += amount;
            });
        }

        // `enemy`: set when the target is hostile (an interrupt or a purge on an enemy starts a fight; a cleanse does not).
        void Interrupt(std::uint64_t nowMs, std::uint8_t pid, std::uint8_t source, std::uint32_t spell, Enemy const* enemy = nullptr)
        {
            if (!pid || !Begin(nowMs, enemy))
                return;
            Each(nowMs, pid, [&](Row& r) {
                r.spells[kInterrupts][Key(source, spell)].Add(1, false);
                ++r.t.interrupts;
                Active(r, nowMs);
            });
        }

        void Dispel(std::uint64_t nowMs, std::uint8_t pid, std::uint8_t source, std::uint32_t spell, std::uint32_t count,
                    Enemy const* enemy = nullptr)
        {
            if (!pid || !count || !Begin(nowMs, enemy))
                return;
            Each(nowMs, pid, [&](Row& r) {
                SpellStats& s = r.spells[kDispels][Key(source, spell)];
                for (std::uint32_t i = 0; i < count; ++i)
                    s.Add(1, false);
                r.t.dispels += count;
                Active(r, nowMs);
            });
        }

        // `pid` died: the count and a recap of its last hits and heals (within kRecapWindowMs).
        void Died(std::uint64_t nowMs, std::uint8_t pid, std::string_view killer)
        {
            if (!pid || !Begin(nowMs, nullptr))
                return;
            Death d;
            Segment const* live = Live();
            d.timeMs = live ? nowMs - std::min(nowMs, live->startMs) : 0;
            d.killer = Clean(killer);
            auto it = _recent.find(pid);
            if (it != _recent.end())
            {
                std::vector<RecapEvent> window;
                for (RecapEvent const& e : it->second)
                    if (std::int64_t(nowMs) - e.atMs <= std::int64_t(kRecapWindowMs))
                        window.push_back(e);
                std::size_t const from = window.size() > kRecapEvents ? window.size() - kRecapEvents : 0;
                for (std::size_t i = from; i < window.size(); ++i)
                {
                    RecapEvent e = window[i];
                    e.atMs -= std::int64_t(nowMs);
                    d.events.push_back(e);
                }
                for (auto e = d.events.rbegin(); e != d.events.rend(); ++e)
                    if (e->amount < 0)
                    {
                        d.spell = e->spell;
                        if (d.killer.empty())
                            d.killer = e->source;
                        break;
                    }
                _recent.erase(it);
            }
            Each(nowMs, pid, [&](Row& r) {
                ++r.t.deaths;
                std::size_t const keep = &r == OverallRow(pid) ? kDeathsKept * 3 : kDeathsKept;
                r.deaths.push_back(d);
                if (r.deaths.size() > keep)
                    r.deaths.erase(r.deaths.begin());
            });
        }

    private:
        struct CombatFlag
        {
            bool inCombat = false;
            std::uint64_t seenMs = 0;
        };
        std::map<std::uint8_t, CombatFlag> _combat;
        std::uint64_t _lastCombatMs = 0;
        std::uint32_t _nextId = 1;
        std::map<std::uint8_t, std::deque<RecapEvent>> _recent;

        void touch(std::uint64_t nowMs) { lastTouchMs = nowMs; }

        Row* OverallRow(std::uint8_t pid)
        {
            auto it = overall.rows.find(pid);
            return it == overall.rows.end() ? nullptr : &it->second;
        }

        // The live fight for an event: a hostile one (enemy set) starts one; anything else needs one running. Names
        // the fight after its first enemy, or its first boss.
        bool Begin(std::uint64_t nowMs, Enemy const* enemy)
        {
            touch(nowMs);
            Tick(nowMs);
            Segment* live = Live();
            if (!live)
            {
                if (!enemy)
                    return false;
                Segment s;
                s.id = _nextId++;
                s.startMs = nowMs;
                s.live = true;
                s.sawCombat = AnyInCombat(nowMs);
                fights.push_front(std::move(s));
                while (fights.size() > kHistory)
                    fights.pop_back();
                ++listVersion;
                ++overall.version;
                live = &fights.front();
                _lastCombatMs = nowMs;
            }
            if (enemy)
            {
                _lastCombatMs = nowMs;
                if ((live->name.empty() && !enemy->name.empty()) || (enemy->boss && !live->boss))
                {
                    live->name = Clean(enemy->name);
                    live->boss = live->boss || enemy->boss;
                }
            }
            live->lastEventMs = std::max(live->lastEventMs, nowMs);
            return true;
        }

        template <typename F>
        void Each(std::uint64_t /*nowMs*/, std::uint8_t pid, F&& f)
        {
            Segment* live = Live();
            if (!live)
                return;
            for (Segment* s : { live, &overall })
            {
                ++s->version;
                Row& r = s->rows[pid];
                f(r);
                r.version = s->version;
            }
        }

        static void Active(Row& r, std::uint64_t nowMs)
        {
            if (r.lastActiveMs && nowMs >= r.lastActiveMs && nowMs - r.lastActiveMs <= kActiveGapMs)
                r.t.activeMs += nowMs - r.lastActiveMs;
            r.lastActiveMs = nowMs;
        }

        void Remember(std::uint64_t nowMs, std::uint8_t pid, std::uint32_t spell, std::int64_t amount, std::uint8_t hpPct, bool crit,
                      std::string_view source)
        {
            auto& q = _recent[pid];
            q.push_back({ std::int64_t(nowMs), spell, amount, hpPct, crit, Clean(source) });
            while (q.size() > kRecapEvents * 2)
                q.pop_front();
        }
    };

    // ---- the messages ----------------------------------------------------------------------------------------------------
    inline std::string Head(std::string_view cmd) { return std::to_string(kVersion) + ";0;" + std::string(cmd); }

    inline std::string OnBody(std::uint32_t epoch) { return Head("ON") + ";" + std::to_string(epoch); }
    inline std::string OffBody() { return Head("OFF"); }
    inline std::string OkBody(std::uint32_t seq) { return Head("OK") + ";" + std::to_string(seq); }
    inline std::string EndBody(std::uint32_t seq) { return Head("END") + ";" + std::to_string(seq); }
    inline std::string ErrBody(std::uint32_t seq, std::string_view code) { return Head("ERR") + ";" + std::to_string(seq) + ";" + std::string(code); }

    inline std::string WhoBody(Participant const& p)
    {
        return Head("WHO") + ";" + std::to_string(p.pid) + ";" + char(p.kind) + ";" + std::to_string(p.cls) + ";" + std::to_string(p.owner) + ";"
            + p.name;
    }

    inline std::string ListBody(Meter const& m)
    {
        std::string body = Head("LIST") + ";";
        bool first = true;
        for (Segment const& s : m.fights)
        {
            if (!first)
                body += ",";
            body += std::to_string(s.id);
            first = false;
        }
        return body;
    }

    inline std::string SegBody(Meter const& m, Segment const& s, std::uint64_t nowMs)
    {
        bool const isOverall = s.id == 0;
        bool const live = isOverall ? m.Live() != nullptr : s.live;
        std::uint32_t const flags = (live ? 1u : 0u) | (s.boss ? 2u : 0u);
        std::uint64_t const dur = isOverall ? m.OverallMs(nowMs) : s.DurationMs(nowMs);
        return Head("SEG") + ";" + std::to_string(s.id) + ";" + std::to_string(flags) + ";" + B36(dur) + ";" + s.name;
    }

    inline std::string SumRow(std::uint8_t pid, Row const& r)
    {
        Totals const& t = r.t;
        std::string row = std::to_string(pid);
        for (std::uint64_t v : { t.damage, t.healing, t.overheal, t.absorbs, t.taken, t.received, std::uint64_t(t.interrupts),
                 std::uint64_t(t.dispels), std::uint64_t(t.deaths), t.activeMs })
        {
            row += ",";
            row += B36(v);
        }
        return row;
    }

    // Adds `rows` to bodies `head;row|row...`, a new body whenever the next row would pass kMaxBody.
    inline void Pack(std::vector<std::string>& out, std::string const& head, std::vector<std::string> const& rows)
    {
        std::string body = head;
        bool empty = true;
        for (std::string const& row : rows)
        {
            if (!empty && body.size() + 1 + row.size() > kMaxBody)
            {
                out.push_back(body);
                body = head;
                empty = true;
            }
            body += empty ? ";" : "|";
            body += row;
            empty = false;
        }
        if (!empty)
            out.push_back(body);
    }

    // The SUM bodies of a segment's rows that changed after `sinceVersion` (0: all).
    inline std::vector<std::string> SumBodies(Segment const& s, std::uint32_t sinceVersion)
    {
        std::vector<std::string> rows;
        for (auto const& [pid, r] : s.rows)
            if (r.version > sinceVersion)
                rows.push_back(SumRow(pid, r));
        std::vector<std::string> out;
        Pack(out, Head("SUM") + ";" + std::to_string(s.id), rows);
        return out;
    }

    // A segment's per-spell rows of one participant (its pets' too), biggest first; then END.
    inline std::vector<std::string> SpellBodies(std::uint32_t seq, Segment const& s, std::uint8_t pid, Metric metric)
    {
        std::vector<std::pair<SpellKey, SpellStats>> spells;
        auto it = s.rows.find(pid);
        if (it != s.rows.end())
            spells.assign(it->second.spells[metric].begin(), it->second.spells[metric].end());
        std::stable_sort(spells.begin(), spells.end(), [](auto const& a, auto const& b) { return a.second.total > b.second.total; });
        std::vector<std::string> rows;
        for (auto const& [key, st] : spells)
        {
            std::string row = std::to_string(key >> 32) + "," + std::to_string(std::uint32_t(key));
            for (std::uint64_t v : { std::uint64_t(st.hits), std::uint64_t(st.crits), std::uint64_t(st.misses), st.total, st.over, st.absorbed,
                     std::uint64_t(st.max), std::uint64_t(st.min) })
            {
                row += ",";
                row += B36(v);
            }
            rows.push_back(row);
        }
        std::vector<std::string> out;
        Pack(out, Head("SPL") + ";" + std::to_string(seq) + ";" + std::to_string(s.id) + ";" + std::to_string(pid) + ";" + MetricLetter(metric), rows);
        out.push_back(EndBody(seq));
        return out;
    }

    // A segment's deaths of one participant, each a DTH and its recap (DEV); then END.
    inline std::vector<std::string> DeathBodies(std::uint32_t seq, Segment const& s, std::uint8_t pid)
    {
        std::vector<std::string> out;
        auto it = s.rows.find(pid);
        if (it != s.rows.end())
        {
            std::size_t n = 0;
            for (Death const& d : it->second.deaths)
            {
                ++n;
                out.push_back(Head("DTH") + ";" + std::to_string(seq) + ";" + std::to_string(s.id) + ";" + std::to_string(pid) + ";" + std::to_string(n)
                              + ";" + B36(d.timeMs) + ";" + std::to_string(d.spell) + ";" + d.killer);
                std::vector<std::string> rows;
                for (RecapEvent const& e : d.events)
                    rows.push_back(B36(std::uint64_t(-std::min<std::int64_t>(e.atMs, 0))) + "," + std::to_string(e.spell) + "," + SignedB36(e.amount)
                                   + "," + std::to_string(e.hpPct) + "," + (e.crit ? "1" : "0") + "," + e.source);
                Pack(out, Head("DEV") + ";" + std::to_string(seq) + ";" + std::to_string(n), rows);
            }
        }
        out.push_back(EndBody(seq));
        return out;
    }

    // ---- one client --------------------------------------------------------------------------------------------------
    struct Request
    {
        std::uint32_t seq = 0;
        std::string cmd;
        std::vector<std::string> fields;
    };

    // A client's body `1;<seq>;<CMD>[;field...]` (printable ASCII, at most kMaxBody bytes).
    inline std::optional<Request> ParseRequest(std::string_view body)
    {
        if (body.empty() || body.size() > kMaxBody)
            return std::nullopt;
        for (char c : body)
            if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e)
                return std::nullopt;
        std::vector<std::string> parts;
        std::size_t start = 0;
        while (true)
        {
            std::size_t const semi = body.find(';', start);
            parts.emplace_back(body.substr(start, semi == std::string_view::npos ? std::string_view::npos : semi - start));
            if (semi == std::string_view::npos)
                break;
            start = semi + 1;
        }
        if (parts.size() < 3 || parts[0] != std::to_string(kVersion) || parts[2].empty())
            return std::nullopt;
        auto const seq = ParseDec(parts[1]);
        if (!seq || *seq > 0xFFFFFFFFu)
            return std::nullopt;
        Request r;
        r.seq = std::uint32_t(*seq);
        r.cmd = parts[2];
        r.fields.assign(parts.begin() + 3, parts.end());
        return r;
    }

    // What a client has been told, and what it watches.
    struct Client
    {
        static constexpr std::uint32_t kNever = 0xFFFFFFFFu;
        bool hello = false;
        std::uint32_t epoch = 0;     // the meter it knows (0: none yet)
        std::uint32_t rosterSent = 0; // roster entries sent (the roster only grows)
        std::uint32_t listVersion = kNever;
        std::map<std::uint32_t, std::uint32_t> sent; // segment id -> its version in the last SUM
        std::int64_t view = -1;      // the segment its window shows besides the current one (-1: none, 0 overall)
        std::uint32_t pushMs = 0;    // since the last push
        // the request limit: kRequestsPer10s per 10 s
        std::uint64_t windowStartMs = 0;
        std::uint32_t windowCount = 0;
        bool throttled = false;
        // bandwidth: everything sent, and what was sent while a fight was live
        std::uint64_t bytes = 0, messages = 0, fightBytes = 0, fightMs = 0;

        void Forget()
        {
            epoch = 0;
            rosterSent = 0;
            listVersion = kNever;
            sent.clear();
        }

        // false when over the limit (the first refusal in a window answers THROTTLE: throttled is set then).
        bool Allow(std::uint64_t nowMs)
        {
            if (nowMs - std::min(nowMs, windowStartMs) >= 10000)
            {
                windowStartMs = nowMs;
                windowCount = 0;
                throttled = false;
            }
            return ++windowCount <= kRequestsPer10s;
        }
    };

    // Everything a client should get now: a new meter (ON), new names (WHO), the list of fights (LIST), the current
    // fight and the segment it views (SEG, and SUM of the rows that changed). Updates the client's state.
    inline std::vector<std::string> Push(Meter const& m, Client& c, std::uint64_t nowMs)
    {
        std::vector<std::string> out;
        if (!c.hello)
            return out;
        if (c.epoch != m.epoch)
        {
            c.Forget();
            c.epoch = m.epoch;
            out.push_back(OnBody(m.epoch));
        }
        for (; c.rosterSent < m.roster.size(); ++c.rosterSent)
            out.push_back(WhoBody(m.roster[c.rosterSent]));
        if (c.listVersion != m.listVersion)
        {
            c.listVersion = m.listVersion;
            out.push_back(ListBody(m));
            // forget the versions of fights that are gone
            for (auto it = c.sent.begin(); it != c.sent.end();)
                it = (it->first != 0 && !m.Find(it->first)) ? c.sent.erase(it) : std::next(it);
        }
        std::vector<Segment const*> segments;
        if (Segment const* cur = m.Current())
            segments.push_back(cur);
        if (c.view >= 0)
            if (Segment const* v = m.Find(std::uint32_t(c.view)); v && (segments.empty() || v != segments.front()))
                segments.push_back(v);
        // A fight the client last heard of as live that has ended since: its end (and last rows) once, also when the
        // next fight started in the same moment (else the client kept it live, its duration running on)
        for (Segment const& s : m.fights)
            if (auto it = c.sent.find(s.id); !s.live && it != c.sent.end() && it->second != s.version
                && std::find(segments.begin(), segments.end(), &s) == segments.end())
                segments.push_back(&s);
        for (Segment const* s : segments)
        {
            auto it = c.sent.find(s->id);
            std::uint32_t const since = it == c.sent.end() ? 0 : it->second;
            if (it != c.sent.end() && since == s->version)
                continue;
            out.push_back(SegBody(m, *s, nowMs));
            for (std::string& b : SumBodies(*s, since))
                out.push_back(std::move(b));
            c.sent[s->id] = s->version;
        }
        return out;
    }

    // A segment reference of a request: c (the current fight), 0 (overall) or an id.
    inline Segment const* ResolveSegment(Meter const& m, std::string_view ref)
    {
        if (ref == "c")
            return m.Current();
        auto const id = ParseDec(ref);
        return id && *id <= 0xFFFFFFFFu ? m.Find(std::uint32_t(*id)) : nullptr;
    }
}

#endif
