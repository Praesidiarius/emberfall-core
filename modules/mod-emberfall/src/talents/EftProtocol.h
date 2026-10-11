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

// Emberfall talents (docs/design/talents.md §4): the EFT addon-message protocol as plain functions, with no
// dependency on the core, so tests/ can run them standalone (rules vectors, decode fuzz).
//
//   transport   a whisper to oneself in LANG_ADDON, text `EFT\t<body>`, at most 255 bytes (body 251)
//   body        <ver>;<seq>;<cmd>[;<field>...], printable ASCII only; ver 1; seq 1-9999 (0 from the server)
//   chunking    a field <part>/<total> (1-based, total <= 8) precedes a chunked payload; parts of one seq
//               in order; a set is dropped after 5 s
//   loadouts    on the wire a rank string per tree in `ord` order (TalentRules.h); in the characters DB
//               `nodeID:rank` / `nodeID:option` pairs (option a-c), invested nodes only (§2.8)
//
// Everything here is bounds-checked and never throws: a malformed input gives std::nullopt / an error value.

#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ef::eft
{
    constexpr std::string_view kPrefix = "EFT\t";
    constexpr size_t kMaxText = 255;                       // ChatHandler drops longer texts
    constexpr size_t kMaxBody = kMaxText - kPrefix.size(); // 251
    constexpr uint32_t kVersion = 1;
    constexpr uint32_t kMaxSeq = 9999;
    constexpr uint32_t kMaxParts = 8;
    constexpr uint64_t kChunkTimeoutMs = 5000;
    constexpr size_t kMaxNameLength = 24;

    inline bool isPrintable(char c) { return c >= 0x20 && c <= 0x7e; }

    // Splits on `sep`, keeping empty fields ("a;;b" -> a, "", b).
    inline std::vector<std::string_view> split(std::string_view text, char sep)
    {
        std::vector<std::string_view> out;
        size_t start = 0;
        while (true)
        {
            const size_t pos = text.find(sep, start);
            if (pos == std::string_view::npos)
            {
                out.push_back(text.substr(start));
                return out;
            }
            out.push_back(text.substr(start, pos - start));
            start = pos + 1;
        }
    }

    // A decimal number of 1-10 digits that fits `max`; nothing else (no sign, no spaces).
    inline std::optional<uint32_t> number(std::string_view text, uint32_t max = UINT32_MAX)
    {
        if (text.empty() || text.size() > 10)
            return std::nullopt;
        uint64_t value = 0;
        for (const char c : text)
        {
            if (c < '0' || c > '9')
                return std::nullopt;
            value = value * 10 + static_cast<uint64_t>(c - '0');
        }
        if (value > max)
            return std::nullopt;
        return static_cast<uint32_t>(value);
    }

    struct Request
    {
        uint32_t version = 0;
        uint32_t seq = 0;
        std::string cmd;
        std::vector<std::string> fields;
    };

    enum class ParseError : uint8_t
    {
        None,
        Decode,  // not a body at all (ERR DECODE with the seq if it could be read, else 0)
        Version, // a well-formed body of another protocol version (ERR VERSION)
    };

    struct ParseResult
    {
        ParseError error = ParseError::None;
        uint32_t seq = 0; // what could be read of it, for the error answer
        Request request;
        bool ok() const { return error == ParseError::None; }
    };

    // The body of a client message (the text after `EFT\t`).
    inline ParseResult parse(std::string_view body)
    {
        ParseResult r;
        if (body.empty() || body.size() > kMaxBody)
        {
            r.error = ParseError::Decode;
            return r;
        }
        for (const char c : body)
            if (!isPrintable(c))
            {
                r.error = ParseError::Decode;
                return r;
            }
        const std::vector<std::string_view> parts = split(body, ';');
        if (parts.size() >= 2)
            if (const auto seq = number(parts[1], kMaxSeq))
                r.seq = *seq;
        const auto version = number(parts[0], 999);
        if (!version || parts.size() < 3)
        {
            r.error = ParseError::Decode;
            return r;
        }
        if (*version != kVersion)
        {
            r.error = ParseError::Version;
            return r;
        }
        const auto seq = number(parts[1], kMaxSeq);
        if (!seq || parts[2].empty() || parts[2].size() > 16)
        {
            r.error = ParseError::Decode;
            return r;
        }
        for (const char c : parts[2])
            if (c < 'A' || c > 'Z')
            {
                r.error = ParseError::Decode;
                return r;
            }
        r.request.version = *version;
        r.request.seq = *seq;
        r.request.cmd = std::string(parts[2]);
        for (size_t i = 3; i < parts.size(); ++i)
            r.request.fields.emplace_back(parts[i]);
        return r;
    }

    // `<part>/<total>`, 1 <= part <= total <= kMaxParts.
    inline std::optional<std::pair<uint32_t, uint32_t>> partOf(std::string_view text)
    {
        const std::vector<std::string_view> p = split(text, '/');
        if (p.size() != 2)
            return std::nullopt;
        const auto part = number(p[0], kMaxParts), total = number(p[1], kMaxParts);
        if (!part || !total || *part < 1 || *part > *total)
            return std::nullopt;
        return std::make_pair(*part, *total);
    }

    // A server message body: `1;<seq>;<cmd>;<fields...>`.
    inline std::string body(uint32_t seq, std::string_view cmd, const std::vector<std::string>& fields = {})
    {
        std::string out = std::to_string(kVersion) + ";" + std::to_string(seq) + ";" + std::string(cmd);
        for (const std::string& f : fields)
            out += ";" + f;
        return out;
    }

    // Bodies for a chunked payload: `head` fields, then `<part>/<total>`, then the part of `payload`; every
    // body at most kMaxBody bytes. Empty when it does not fit into kMaxParts messages.
    inline std::vector<std::string> chunked(uint32_t seq, std::string_view cmd, const std::vector<std::string>& head,
                                            std::string_view payload)
    {
        const std::string fixed = body(seq, cmd, head) + ";";
        const size_t partField = 4; // "p/t;" with p, t <= 8
        if (fixed.size() + partField >= kMaxBody)
            return {};
        const size_t room = kMaxBody - fixed.size() - partField;
        const size_t total = payload.empty() ? 1 : (payload.size() + room - 1) / room;
        if (total > kMaxParts)
            return {};
        std::vector<std::string> out;
        for (size_t i = 0; i < total; ++i)
            out.push_back(fixed + std::to_string(i + 1) + "/" + std::to_string(total) + ";" +
                          std::string(payload.substr(std::min(payload.size(), i * room), room)));
        return out;
    }

    // Names on the wire: `;`, `,`, `%`, control characters and every non-ASCII byte as %XX.
    inline std::string encodeName(std::string_view name)
    {
        static constexpr char hex[] = "0123456789ABCDEF";
        std::string out;
        for (const char ch : name)
        {
            const auto c = static_cast<unsigned char>(ch);
            if (c < 0x20 || c > 0x7e || c == ';' || c == ',' || c == '%')
            {
                out += '%';
                out += hex[c >> 4];
                out += hex[c & 15];
            }
            else
                out += ch;
        }
        return out;
    }

    inline std::optional<std::string> decodeName(std::string_view text)
    {
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        std::string out;
        for (size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] != '%')
            {
                out += text[i];
                continue;
            }
            if (i + 2 >= text.size())
                return std::nullopt;
            const int hi = nibble(text[i + 1]), lo = nibble(text[i + 2]);
            if (hi < 0 || lo < 0)
                return std::nullopt;
            out += static_cast<char>(hi * 16 + lo);
            i += 2;
        }
        return out;
    }

    // A loadout name: 1-24 characters (UTF-8 code points), none of them a control character.
    inline bool validName(std::string_view name)
    {
        size_t chars = 0;
        for (const char ch : name)
        {
            const auto c = static_cast<unsigned char>(ch);
            if (c < 0x20 || c == 0x7f)
                return false;
            if ((c & 0xC0) != 0x80)
                ++chars;
        }
        return chars >= 1 && chars <= kMaxNameLength;
    }

    // A stored loadout: node ID -> (rank, option); option 1-3 for a choice node (rank then 1).
    struct Pair
    {
        uint8_t rank = 0;
        uint8_t option = 0;
        bool operator==(const Pair&) const = default;
    };
    using Pairs = std::map<uint32_t, Pair>;

    // "50012:2,50020:b" (sorted by node ID; nodes with nothing invested left out).
    inline std::string encodePairs(const Pairs& pairs)
    {
        std::string out;
        for (const auto& [node, p] : pairs)
        {
            if (p.rank == 0 && p.option == 0)
                continue;
            if (!out.empty())
                out += ',';
            out += std::to_string(node) + ':';
            if (p.option >= 1 && p.option <= 3)
                out += static_cast<char>('a' + p.option - 1);
            else
                out += std::to_string(p.rank);
        }
        return out;
    }

    inline std::optional<Pairs> decodePairs(std::string_view text)
    {
        Pairs out;
        if (text.empty())
            return out;
        for (const std::string_view item : split(text, ','))
        {
            const std::vector<std::string_view> kv = split(item, ':');
            if (kv.size() != 2 || kv[1].size() != 1)
                return std::nullopt;
            const auto node = number(kv[0]);
            if (!node || *node == 0 || out.count(*node))
                return std::nullopt;
            const char v = kv[1][0];
            if (v >= '1' && v <= '9')
                out[*node] = Pair{static_cast<uint8_t>(v - '0'), 0};
            else if (v >= 'a' && v <= 'c')
                out[*node] = Pair{1, static_cast<uint8_t>(v - 'a' + 1)};
            else
                return std::nullopt;
        }
        return out;
    }

    // Reassembles the chunked payload of one request at a time (talents.md §4.3): parts of one seq, in
    // order, the same total; a set older than kChunkTimeoutMs is dropped. At most kMaxParts x kMaxBody bytes.
    class Reassembly
    {
    public:
        enum class Status : uint8_t
        {
            Complete,   // `payload()` holds the whole payload
            Incomplete, // wait for the next part, no answer
            Error,      // ERR CHUNK; the set is dropped
        };

        Status feed(uint32_t seq, uint32_t part, uint32_t total, std::string_view data, uint64_t nowMs)
        {
            if (m_active && nowMs - m_started > kChunkTimeoutMs)
                m_active = false; // timed out: a later part of it is out of order now
            if (total < 1 || total > kMaxParts || part < 1 || part > total || data.size() > kMaxBody)
            {
                m_active = false;
                return Status::Error;
            }
            if (part == 1)
            {
                m_active = true;
                m_seq = seq;
                m_total = total;
                m_next = 1;
                m_started = nowMs;
                m_payload.clear();
            }
            else if (!m_active || seq != m_seq || total != m_total || part != m_next)
            {
                m_active = false;
                return Status::Error;
            }
            m_payload.append(data);
            ++m_next;
            if (part == total)
            {
                m_active = false;
                return Status::Complete;
            }
            return Status::Incomplete;
        }

        const std::string& payload() const { return m_payload; }
        bool pending() const { return m_active; }

    private:
        bool m_active = false;
        uint32_t m_seq = 0, m_total = 0, m_next = 0;
        uint64_t m_started = 0;
        std::string m_payload;
    };

    // At most `limit` events per `windowMs` (a sliding window over the event times).
    class RateLimit
    {
    public:
        RateLimit(uint32_t limit, uint64_t windowMs) : m_limit(limit), m_window(windowMs) { }

        bool allow(uint64_t nowMs)
        {
            m_times.erase(std::remove_if(m_times.begin(), m_times.end(), [&](uint64_t t) { return nowMs - t >= m_window; }),
                          m_times.end());
            if (m_times.size() >= m_limit)
                return false;
            m_times.push_back(nowMs);
            return true;
        }

    private:
        uint32_t m_limit;
        uint64_t m_window;
        std::vector<uint64_t> m_times;
    };
}
