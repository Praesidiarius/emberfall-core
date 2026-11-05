// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 the Emberfall authors.
//
// EfEncounterWire.h: the EFN protocol's frame (docs/design/mini-encounters.md §5): mod-emberfall and our client talk
// about a mini-encounter (a fish on the line; later a beast to tame) by addon whispers to oneself, prefix `EFN`, a tab,
// then `<ver>;<seq>;<kind>;<cmd>;<fields...>`, at most 255 bytes. Header-only, standard library only.
//
// This one file is shared: the client (client/src/data/) and mod-emberfall (src/encounters/EfEncounterWire.h, a verbatim
// copy; tools/py/tests/test_fishing.py checks they are equal). A kind's own messages build on it (EfFishWire.h for
// kind `F`). Fields never contain `;` (numbers, codes, hex GUIDs; a list inside a field uses `/` and `:`).

#ifndef EMBERFALL_ENCOUNTER_WIRE_H
#define EMBERFALL_ENCOUNTER_WIRE_H

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
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
