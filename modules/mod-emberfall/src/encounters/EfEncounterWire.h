// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 the Emberfall authors.
//
// EfEncounterWire.h: the EFN protocol's frame (docs/design/mini-encounters.md §5): mod-emberfall and our client talk
// about a mini-encounter (a fish on the line; later a beast to tame) by addon whispers to oneself, prefix `EFN`, a tab,
// then `<ver>;<seq>;<kind>;<cmd>;<fields...>`, at most 255 bytes. Header-only, standard library only.
//
// This one file is shared: the client (client/src/data/) and mod-emberfall (src/encounters/EfEncounterWire.h, a verbatim
// copy; tools/py/tests/test_fishing.py checks they are equal). A kind's own messages build on it (EfFishWire.h for
// kind `F`, EfTameWire.h for kind `T`). Fields never contain `;` (numbers, codes, hex GUIDs; a list inside a field uses
// `/` and `:`).
//
// The server-driven kinds' shared messages (taming first; docs/design/hunter-taming.md §14) are here too:
//   s -> c  BARS   <id>;<subject hex>;<phase>;<left ms>;<flags>;<value/max>...   (at most kMaxBars bars)
//   s -> c  SLOTS  <id>;<item:count>...                                           (at most kMaxSlots slots)
//   c -> s  PUT    <id>;<slot>;<bag>;<bag slot>;<count>     slot 0-based; bag 0 the backpack, 1-4 the bags; bag slot
//                                                           1-based (the Lua API's numbering)
//   c -> s  TAKE   <id>;<slot>

#ifndef EMBERFALL_ENCOUNTER_WIRE_H
#define EMBERFALL_ENCOUNTER_WIRE_H

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace efenc
{
    constexpr std::string_view kPrefix = "EFN";
    constexpr std::uint32_t kVersion = 1;
    constexpr std::size_t kMaxBytes = 255;   // the whole addon message: prefix, tab and body

    // The kinds (one letter): fishing (client-driven), taming (server-driven, later).
    constexpr char kFishing = 'F';
    constexpr char kTaming = 'T';

    // ERR codes (mini-encounters.md §5).
    constexpr std::string_view kErrDecode = "DECODE";
    constexpr std::string_view kErrData = "DATA";
    constexpr std::string_view kErrNoEnc = "NOENC";
    constexpr std::string_view kErrRange = "RANGE";
    constexpr std::string_view kErrGroup = "GROUP";
    constexpr std::string_view kErrClaim = "CLAIM";
    constexpr std::string_view kErrThrottle = "THROTTLE";
    constexpr std::string_view kErrBounds = "BOUNDS";

    struct Message
    {
        std::uint32_t seq = 0;
        char kind = 0;
        std::string cmd;
        std::vector<std::string> fields;
    };

    inline std::vector<std::string_view> Split(std::string_view s, char sep)
    {
        std::vector<std::string_view> out;
        std::size_t start = 0;
        for (;;)
        {
            std::size_t const at = s.find(sep, start);
            if (at == std::string_view::npos)
            {
                out.push_back(s.substr(start));
                return out;
            }
            out.push_back(s.substr(start, at - start));
            start = at + 1;
        }
    }

    template <typename T>
    inline std::optional<T> ParseUint(std::string_view s, int base = 10)
    {
        if (s.empty())
            return std::nullopt;
        T v{};
        auto const [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v, base);
        if (ec != std::errc() || p != s.data() + s.size())
            return std::nullopt;
        return v;
    }

    inline std::string Hex(std::uint64_t v)
    {
        char buf[17];
        auto const r = std::to_chars(buf, buf + sizeof(buf), v, 16);
        return std::string(buf, r.ptr);
    }

    // The body (what follows `EFN\t`); nullopt when it is not a version-1 EFN body.
    inline std::optional<Message> Decode(std::string_view body)
    {
        if (body.size() + kPrefix.size() + 1 > kMaxBytes)
            return std::nullopt;
        std::vector<std::string_view> const parts = Split(body, ';');
        if (parts.size() < 4)
            return std::nullopt;
        auto const ver = ParseUint<std::uint32_t>(parts[0]);
        auto const seq = ParseUint<std::uint32_t>(parts[1]);
        if (!ver || *ver != kVersion || !seq || parts[2].size() != 1 || parts[3].empty())
            return std::nullopt;
        Message m;
        m.seq = *seq;
        m.kind = parts[2][0];
        m.cmd = std::string(parts[3]);
        for (std::size_t i = 4; i < parts.size(); ++i)
            m.fields.emplace_back(parts[i]);
        return m;
    }

    // The body for a message; empty when the whole addon message would pass 255 bytes or a field holds a `;`.
    inline std::string Encode(Message const& m)
    {
        std::string out = std::to_string(kVersion) + ";" + std::to_string(m.seq) + ";" + std::string(1, m.kind) + ";" + m.cmd;
        for (std::string const& f : m.fields)
        {
            if (f.find(';') != std::string::npos)
                return {};
            out += ";";
            out += f;
        }
        if (out.size() + kPrefix.size() + 1 > kMaxBytes)
            return {};
        return out;
    }

    // --- the server-driven kinds' shared messages ---------------------------------------------------------------------

    constexpr std::size_t kMaxBars = 3;
    constexpr std::size_t kMaxSlots = 6;

    struct BarValue
    {
        std::uint32_t value = 0;
        std::uint32_t max = 0;
    };

    // A subject's bars. `id` 0: no encounter is open for the receiver; the bars are for the target frame and the
    // nameplate (a beast being drowsed). Phase 0: over (the receiver drops the subject's bars).
    struct Bars
    {
        std::uint32_t id = 0;
        std::uint64_t subject = 0;
        std::uint32_t phase = 0;
        std::uint32_t leftMs = 0;
        std::uint32_t flags = 0;          // the kind's own bits (taming: eftame::wire::kMine)
        std::vector<BarValue> bars;
    };

    struct SlotValue
    {
        std::uint32_t item = 0;           // 0: empty
        std::uint32_t count = 0;
    };

    struct Slots
    {
        std::uint32_t id = 0;
        std::vector<SlotValue> slots;
    };

    struct Put
    {
        std::uint32_t id = 0;
        std::uint32_t slot = 0;           // 0-based
        std::uint32_t bag = 0;            // 0 the backpack, 1-4 the bags
        std::uint32_t bagSlot = 0;        // 1-based
        std::uint32_t count = 0;
    };

    struct Take
    {
        std::uint32_t id = 0;
        std::uint32_t slot = 0;           // 0-based
    };

    inline Message MakeMessage(std::uint32_t seq, char kind, char const* cmd)
    {
        Message m;
        m.seq = seq;
        m.kind = kind;
        m.cmd = cmd;
        return m;
    }

    // `a/b` and `a:b` pairs of unsigned numbers.
    inline std::optional<std::pair<std::uint32_t, std::uint32_t>> ParsePair(std::string_view s, char sep)
    {
        std::size_t const at = s.find(sep);
        if (at == std::string_view::npos)
            return std::nullopt;
        auto const a = ParseUint<std::uint32_t>(s.substr(0, at));
        auto const b = ParseUint<std::uint32_t>(s.substr(at + 1));
        if (!a || !b)
            return std::nullopt;
        return std::make_pair(*a, *b);
    }

    inline std::string EncodeBars(std::uint32_t seq, char kind, Bars const& b)
    {
        if (b.bars.size() > kMaxBars)
            return {};
        Message m = MakeMessage(seq, kind, "BARS");
        m.fields = {std::to_string(b.id), Hex(b.subject), std::to_string(b.phase), std::to_string(b.leftMs), std::to_string(b.flags)};
        for (BarValue const& v : b.bars)
            m.fields.push_back(std::to_string(v.value) + "/" + std::to_string(v.max));
        return Encode(m);
    }

    inline std::optional<Bars> DecodeBars(Message const& m)
    {
        if (m.cmd != "BARS" || m.fields.size() < 5 || m.fields.size() > 5 + kMaxBars)
            return std::nullopt;
        auto const id = ParseUint<std::uint32_t>(m.fields[0]);
        auto const subject = ParseUint<std::uint64_t>(m.fields[1], 16);
        auto const phase = ParseUint<std::uint32_t>(m.fields[2]);
        auto const left = ParseUint<std::uint32_t>(m.fields[3]);
        auto const flags = ParseUint<std::uint32_t>(m.fields[4]);
        if (!id || !subject || !phase || !left || !flags)
            return std::nullopt;
        Bars b{*id, *subject, *phase, *left, *flags, {}};
        for (std::size_t i = 5; i < m.fields.size(); ++i)
        {
            auto const p = ParsePair(m.fields[i], '/');
            if (!p)
                return std::nullopt;
            b.bars.push_back({p->first, p->second});
        }
        return b;
    }

    inline std::string EncodeSlots(std::uint32_t seq, char kind, Slots const& s)
    {
        if (s.slots.size() > kMaxSlots)
            return {};
        Message m = MakeMessage(seq, kind, "SLOTS");
        m.fields = {std::to_string(s.id)};
        for (SlotValue const& v : s.slots)
            m.fields.push_back(std::to_string(v.item) + ":" + std::to_string(v.count));
        return Encode(m);
    }

    inline std::optional<Slots> DecodeSlots(Message const& m)
    {
        if (m.cmd != "SLOTS" || m.fields.empty() || m.fields.size() > 1 + kMaxSlots)
            return std::nullopt;
        auto const id = ParseUint<std::uint32_t>(m.fields[0]);
        if (!id)
            return std::nullopt;
        Slots s{*id, {}};
        for (std::size_t i = 1; i < m.fields.size(); ++i)
        {
            auto const p = ParsePair(m.fields[i], ':');
            if (!p)
                return std::nullopt;
            s.slots.push_back({p->first, p->second});
        }
        return s;
    }

    inline std::string EncodePut(std::uint32_t seq, char kind, Put const& p)
    {
        Message m = MakeMessage(seq, kind, "PUT");
        m.fields = {std::to_string(p.id), std::to_string(p.slot), std::to_string(p.bag), std::to_string(p.bagSlot), std::to_string(p.count)};
        return Encode(m);
    }

    inline std::optional<Put> DecodePut(Message const& m)
    {
        if (m.cmd != "PUT" || m.fields.size() != 5)
            return std::nullopt;
        auto const id = ParseUint<std::uint32_t>(m.fields[0]);
        auto const slot = ParseUint<std::uint32_t>(m.fields[1]);
        auto const bag = ParseUint<std::uint32_t>(m.fields[2]);
        auto const bagSlot = ParseUint<std::uint32_t>(m.fields[3]);
        auto const count = ParseUint<std::uint32_t>(m.fields[4]);
        if (!id || !slot || !bag || !bagSlot || !count)
            return std::nullopt;
        return Put{*id, *slot, *bag, *bagSlot, *count};
    }

    inline std::string EncodeTake(std::uint32_t seq, char kind, Take const& t)
    {
        Message m = MakeMessage(seq, kind, "TAKE");
        m.fields = {std::to_string(t.id), std::to_string(t.slot)};
        return Encode(m);
    }

    inline std::optional<Take> DecodeTake(Message const& m)
    {
        if (m.cmd != "TAKE" || m.fields.size() != 2)
            return std::nullopt;
        auto const id = ParseUint<std::uint32_t>(m.fields[0]);
        auto const slot = ParseUint<std::uint32_t>(m.fields[1]);
        if (!id || !slot)
            return std::nullopt;
        return Take{*id, *slot};
    }

    // At most `perSecond` messages a second from one sender (the server drops the rest: ERR;THROTTLE).
    struct Throttle
    {
        std::uint32_t windowStartMs = 0;
        std::uint32_t count = 0;
        bool Allow(std::uint32_t nowMs, std::uint32_t perSecond = 20)
        {
            if (nowMs - windowStartMs >= 1000)
            {
                windowStartMs = nowMs;
                count = 0;
            }
            return ++count <= perSecond;
        }
    };
}

#endif
