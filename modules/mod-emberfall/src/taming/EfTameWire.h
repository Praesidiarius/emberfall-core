// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 the Emberfall authors.
//
// EfTameWire.h: the EFN kind `T` messages, hunter taming (docs/design/hunter-taming.md §9, §14), on EfEncounterWire.h's
// frame. Taming is server-driven: mod-emberfall keeps the beast's state and sends the bars (BARS) and the slots
// (SLOTS); the client asks to open a sleeping beast, puts items in (PUT) and takes them back (TAKE). Header-only,
// standard library only.
//
// This one file is shared: the client (client/src/data/) and mod-emberfall (src/taming/EfTameWire.h, a verbatim copy;
// tools/py/tests/test_taming.py checks they are equal).
//
//   s -> c  BARS    (EfEncounterWire.h) for a beast being drowsed or asleep, to its claimant and the claimant's group:
//                   id 0 (no frame open) or the claimant's open encounter; phase (Phase below); left ms (Out Cold: the
//                   sleep left); flags (kMine: the receiver holds the claim); bars: Drowsing [drowse], Out Cold and
//                   the waits [sleep (ms left / the sleep's length), trust]
//   c -> s  OPEN    <beast guid hex>                          the claimant right-clicked the sleeping beast
//   s -> c  OPEN    <id>;<beast guid hex>;<family>;<entry>;<level>;<rank>;<slots>
//   s -> c  SLOTS   (EfEncounterWire.h) after every change
//   c -> s  PUT, TAKE (EfEncounterWire.h)                     validated: the claim, 5 yd, the item in the bags, the
//                                                             slot's group (tame_food); ERR CLAIM, RANGE, GROUP, BOUNDS
//   s -> c  EAT     <id>;<item>;<taste>;<amount>              the beast ate or drank from a slot: taste C cake,
//                                                             F favourite, L liked, P plain, R refused (sniffed and
//                                                             left), D a draught drunk; amount the Trust gained (D: the
//                                                             sleep's ms added)
//   s -> c  RESULT  <id>;<beast guid hex>;<outcome>           T tamed, W woke, D died, G gone (evaded, despawned),
//                                                             F the tame failed (the core refused); id 0 when no frame
//                                                             was open
//   s -> c  CLOSE   <id>;<reason>                             RANGE (more than 5 yd), GONE, DIED (the hunter)
//   c -> s  CLOSE   <id>                                      the hunter closed the frame (the beast sleeps on)

#ifndef EMBERFALL_TAME_WIRE_H
#define EMBERFALL_TAME_WIRE_H

#include "EfEncounterWire.h"

#include <optional>
#include <string>

namespace eftame::wire
{
    // BARS phases.
    enum Phase : std::uint32_t
    {
        kOver = 0,          // the state is gone: drop the beast's bars
        kDrowsing = 1,      // bars: [drowse]
        kOutCold = 2,       // bars: [sleep, trust]
        kWaitStable = 3,    // Trust is full; the tame waits for a free stable slot (bars as Out Cold)
        kWaitHunter = 4,    // Trust is full; the tame waits for the hunter (alive, within 40 yd)
    };

    // BARS flags.
    constexpr std::uint32_t kMine = 1;  // the receiver holds the claim

    constexpr std::uint32_t kSlots = 6;

    // EAT tastes.
    constexpr char kCake = 'C';
    constexpr char kFavourite = 'F';
    constexpr char kLiked = 'L';
    constexpr char kPlain = 'P';
    constexpr char kRefused = 'R';
    constexpr char kDrank = 'D';

    // RESULT outcomes.
    constexpr char kTamed = 'T';
    constexpr char kWoke = 'W';
    constexpr char kDied = 'D';
    constexpr char kGone = 'G';
    constexpr char kFailed = 'F';

    struct Open
    {
        std::uint32_t id = 0;
        std::uint64_t beast = 0;
        std::uint32_t family = 0;         // CreatureFamily (1 hound, 2 lynx, 5 boar, 7 hawk, 11 lizard)
        std::uint32_t entry = 0;
        std::uint32_t level = 0;
        std::uint32_t rank = 0;           // creature_template.rank (0 normal, 1 elite, 2 rare elite, 4 rare)
        std::uint32_t slots = kSlots;
    };

    struct Eat
    {
        std::uint32_t id = 0;
        std::uint32_t item = 0;
        char taste = 0;
        std::uint32_t amount = 0;
    };

    struct Result
    {
        std::uint32_t id = 0;
        std::uint64_t beast = 0;
        char outcome = 0;
    };

    struct Close
    {
        std::uint32_t id = 0;
        std::string reason;               // empty from the client
    };

    inline efenc::Message Msg(std::uint32_t seq, char const* cmd) { return efenc::MakeMessage(seq, efenc::kTaming, cmd); }

    inline bool IsTaste(char c)
    {
        return c == kCake || c == kFavourite || c == kLiked || c == kPlain || c == kRefused || c == kDrank;
    }

    inline bool IsOutcome(char c) { return c == kTamed || c == kWoke || c == kDied || c == kGone || c == kFailed; }

    // c -> s: open the sleeping beast's frame.
    inline std::string EncodeOpenRequest(std::uint32_t seq, std::uint64_t beast)
    {
        efenc::Message m = Msg(seq, "OPEN");
        m.fields = {efenc::Hex(beast)};
        return efenc::Encode(m);
    }

    inline std::optional<std::uint64_t> DecodeOpenRequest(efenc::Message const& m)
    {
        if (m.kind != efenc::kTaming || m.cmd != "OPEN" || m.fields.size() != 1)
            return std::nullopt;
        return efenc::ParseUint<std::uint64_t>(m.fields[0], 16);
    }

    inline std::string EncodeOpen(std::uint32_t seq, Open const& o)
    {
        efenc::Message m = Msg(seq, "OPEN");
        m.fields = {std::to_string(o.id), efenc::Hex(o.beast), std::to_string(o.family), std::to_string(o.entry),
                    std::to_string(o.level), std::to_string(o.rank), std::to_string(o.slots)};
        return efenc::Encode(m);
    }

    inline std::optional<Open> DecodeOpen(efenc::Message const& m)
    {
        if (m.kind != efenc::kTaming || m.cmd != "OPEN" || m.fields.size() != 7)
            return std::nullopt;
        auto const id = efenc::ParseUint<std::uint32_t>(m.fields[0]);
        auto const beast = efenc::ParseUint<std::uint64_t>(m.fields[1], 16);
        auto const family = efenc::ParseUint<std::uint32_t>(m.fields[2]);
        auto const entry = efenc::ParseUint<std::uint32_t>(m.fields[3]);
        auto const level = efenc::ParseUint<std::uint32_t>(m.fields[4]);
        auto const rank = efenc::ParseUint<std::uint32_t>(m.fields[5]);
        auto const slots = efenc::ParseUint<std::uint32_t>(m.fields[6]);
        if (!id || !beast || !family || !entry || !level || !rank || !slots || *slots > efenc::kMaxSlots)
            return std::nullopt;
        return Open{*id, *beast, *family, *entry, *level, *rank, *slots};
    }

    inline std::string EncodeEat(std::uint32_t seq, Eat const& e)
    {
        if (!IsTaste(e.taste))
            return {};
        efenc::Message m = Msg(seq, "EAT");
        m.fields = {std::to_string(e.id), std::to_string(e.item), std::string(1, e.taste), std::to_string(e.amount)};
        return efenc::Encode(m);
    }

    inline std::optional<Eat> DecodeEat(efenc::Message const& m)
    {
        if (m.kind != efenc::kTaming || m.cmd != "EAT" || m.fields.size() != 4 || m.fields[2].size() != 1 ||
            !IsTaste(m.fields[2][0]))
            return std::nullopt;
        auto const id = efenc::ParseUint<std::uint32_t>(m.fields[0]);
        auto const item = efenc::ParseUint<std::uint32_t>(m.fields[1]);
        auto const amount = efenc::ParseUint<std::uint32_t>(m.fields[3]);
        if (!id || !item || !amount)
            return std::nullopt;
        return Eat{*id, *item, m.fields[2][0], *amount};
    }

    inline std::string EncodeResult(std::uint32_t seq, Result const& r)
    {
        if (!IsOutcome(r.outcome))
            return {};
        efenc::Message m = Msg(seq, "RESULT");
        m.fields = {std::to_string(r.id), efenc::Hex(r.beast), std::string(1, r.outcome)};
        return efenc::Encode(m);
    }

    inline std::optional<Result> DecodeResult(efenc::Message const& m)
    {
        if (m.kind != efenc::kTaming || m.cmd != "RESULT" || m.fields.size() != 3 || m.fields[2].size() != 1 ||
            !IsOutcome(m.fields[2][0]))
            return std::nullopt;
        auto const id = efenc::ParseUint<std::uint32_t>(m.fields[0]);
        auto const beast = efenc::ParseUint<std::uint64_t>(m.fields[1], 16);
        if (!id || !beast)
            return std::nullopt;
        return Result{*id, *beast, m.fields[2][0]};
    }

    // s -> c with a reason; c -> s without.
    inline std::string EncodeClose(std::uint32_t seq, Close const& c)
    {
        efenc::Message m = Msg(seq, "CLOSE");
        m.fields = {std::to_string(c.id)};
        if (!c.reason.empty())
            m.fields.push_back(c.reason);
        return efenc::Encode(m);
    }

    inline std::optional<Close> DecodeClose(efenc::Message const& m)
    {
        if (m.kind != efenc::kTaming || m.cmd != "CLOSE" || m.fields.empty() || m.fields.size() > 2)
            return std::nullopt;
        auto const id = efenc::ParseUint<std::uint32_t>(m.fields[0]);
        if (!id)
            return std::nullopt;
        return Close{*id, m.fields.size() == 2 ? m.fields[1] : std::string()};
    }
}

#endif
