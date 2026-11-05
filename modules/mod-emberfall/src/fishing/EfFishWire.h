// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 the Emberfall authors.
//
// EfFishWire.h: the EFN kind `F` messages (docs/design/fishing.md §6.4) on EfEncounterWire.h's frame, and the fight
// parameters they carry. Header-only, standard library only.
//
// This one file is shared: the client (client/src/data/) and mod-emberfall (src/fishing/EfFishWire.h, a verbatim copy;
// tools/py/tests/test_fishing.py checks they are equal).
//
//   c -> s  HELLO   <data hash, 16 hex>                                  once per world entry
//   s -> c  OK                                                           the data matches
//   s -> c  ERR     DATA;<server hash>                                   it does not (the fight still works; the
//                                                                        client warns once)
//   s -> c  OPEN    <fight>;<bobber guid hex>;<species>;<size mm>;<seed hex>;<size factor>;<speed>;<stamina>;
//                   <safe lo>;<safe hi>;<break>;<max line myd>;<start line myd>;<max ms>;<passive 0/1>;
//                   <phases: style:until/style:until/...>
//   c -> s  END     <fight>;<outcome L S R N X>;<duration ms>;<peak>;<over 900 ms>;<input seconds>;<phase ends a/b/c>
//   s -> c  RESULT  <fight>;OK;<item>;<size mm>;<grams>;<best 0/1>   or   <fight>;REJ;<code>
//   s -> c  CLOSE   <fight>;<reason MOVED DIED TIMEOUT CHANNEL GONE>

#ifndef EMBERFALL_FISH_WIRE_H
#define EMBERFALL_FISH_WIRE_H

#include "EfEncounterWire.h"
#include "EfFishFight.h"

#include <optional>
#include <string>

namespace effish::wire
{
    struct Open
    {
        std::uint32_t fight = 0;
        std::uint64_t bobber = 0;
        std::uint32_t species = 0;
        std::uint32_t sizeMm = 0;
        FightParams params;               // the styles hold only their ids: the receiver fills them from its rows
        std::array<std::uint32_t, 3> styleIds{};
    };

    struct End
    {
        std::uint32_t fight = 0;
        EndReport report;
    };

    struct Result
    {
        std::uint32_t fight = 0;
        bool ok = false;
        std::string code;                 // REJ: a Verdict code or NOENC
        std::uint32_t item = 0;
        std::uint32_t sizeMm = 0;
        std::uint32_t grams = 0;
        bool best = false;
    };

    struct Close
    {
        std::uint32_t fight = 0;
        std::string reason;
    };

    inline efenc::Message Msg(std::uint32_t seq, char const* cmd)
    {
        efenc::Message m;
        m.seq = seq;
        m.kind = efenc::kFishing;
        m.cmd = cmd;
        return m;
    }

    inline std::string EncodeHello(std::uint32_t seq, std::uint64_t hash)
    {
        efenc::Message m = Msg(seq, "HELLO");
        m.fields = {efenc::Hex(hash)};
        return efenc::Encode(m);
    }

    inline std::string EncodeOpen(std::uint32_t seq, Open const& o)
    {
        efenc::Message m = Msg(seq, "OPEN");
        FightParams const& p = o.params;
        std::string phases;
        for (std::uint32_t k = 0; k < p.phaseCount && k < 3; ++k)
        {
            if (k)
                phases += "/";
            phases += std::to_string(o.styleIds[k]) + ":" + std::to_string(p.untilPermil[k]);
        }
        m.fields = {std::to_string(o.fight), efenc::Hex(o.bobber), std::to_string(o.species), std::to_string(o.sizeMm),
                    efenc::Hex(p.seed), std::to_string(p.sizeFactorPermil), std::to_string(p.speedPermil),
                    std::to_string(p.staminaPermil), std::to_string(p.safeLo), std::to_string(p.safeHi),
                    std::to_string(p.breakAt), std::to_string(p.maxLineMyd), std::to_string(p.startLineMyd),
                    std::to_string(p.maxMs), p.passiveRecovers ? "1" : "0", phases};
        return efenc::Encode(m);
    }

    inline std::optional<Open> DecodeOpen(efenc::Message const& m)
    {
        if (m.kind != efenc::kFishing || m.cmd != "OPEN" || m.fields.size() != 16)
            return std::nullopt;
        auto const& f = m.fields;
        Open o;
        auto u32 = [](std::string const& s) { return efenc::ParseUint<std::uint32_t>(s); };
        auto fight = u32(f[0]);
        auto bobber = efenc::ParseUint<std::uint64_t>(f[1], 16);
        auto species = u32(f[2]);
        auto size = u32(f[3]);
        auto seed = efenc::ParseUint<std::uint64_t>(f[4], 16);
        auto sf = u32(f[5]), speed = u32(f[6]), stamina = u32(f[7]), lo = u32(f[8]), hi = u32(f[9]), brk = u32(f[10]),
             maxLine = u32(f[11]), start = u32(f[12]), maxMs = u32(f[13]);
        if (!fight || !bobber || !species || !size || !seed || !sf || !speed || !stamina || !lo || !hi || !brk || !maxLine ||
            !start || !maxMs || (f[14] != "0" && f[14] != "1"))
            return std::nullopt;
        o.fight = *fight;
        o.bobber = *bobber;
        o.species = *species;
        o.sizeMm = *size;
        FightParams& p = o.params;
        p.seed = *seed;
        p.sizeFactorPermil = *sf;
        p.speedPermil = *speed;
        p.staminaPermil = *stamina;
        p.safeLo = *lo;
        p.safeHi = *hi;
        p.breakAt = *brk;
        p.maxLineMyd = *maxLine;
        p.startLineMyd = *start;
        p.maxMs = *maxMs;
        p.passiveRecovers = f[14] == "1";
        auto const phases = efenc::Split(f[15], '/');
        if (phases.empty() || phases.size() > 3)
            return std::nullopt;
        p.phaseCount = static_cast<std::uint32_t>(phases.size());
        for (std::size_t k = 0; k < phases.size(); ++k)
        {
            auto const pair = efenc::Split(phases[k], ':');
            if (pair.size() != 2)
                return std::nullopt;
            auto style = efenc::ParseUint<std::uint32_t>(pair[0]);
            auto until = efenc::ParseUint<std::uint32_t>(pair[1]);
            if (!style || !until || *until > 1000)
                return std::nullopt;
            o.styleIds[k] = *style;
            p.styles[k].id = *style;
            p.untilPermil[k] = *until;
        }
        return o;
    }

    inline std::string EncodeEnd(std::uint32_t seq, End const& e)
    {
        efenc::Message m = Msg(seq, "END");
        EndReport const& r = e.report;
        std::string phases = std::to_string(r.phaseEndMs[0]) + "/" + std::to_string(r.phaseEndMs[1]) + "/" +
                             std::to_string(r.phaseEndMs[2]);
        m.fields = {std::to_string(e.fight), std::string(1, static_cast<char>(r.outcome)), std::to_string(r.durationMs),
                    std::to_string(r.peakTension), std::to_string(r.over900Ms), std::to_string(r.inputSeconds), phases};
        return efenc::Encode(m);
    }

    inline std::optional<End> DecodeEnd(efenc::Message const& m)
    {
        if (m.kind != efenc::kFishing || m.cmd != "END" || m.fields.size() != 7)
            return std::nullopt;
        auto const& f = m.fields;
        auto u32 = [](std::string_view s) { return efenc::ParseUint<std::uint32_t>(s); };
        auto fight = u32(f[0]);
        auto dur = u32(f[2]), peak = u32(f[3]), over = u32(f[4]), inputs = u32(f[5]);
        if (!fight || f[1].size() != 1 || !IsOutcome(f[1][0]) || !dur || !peak || !over || !inputs)
            return std::nullopt;
        auto const phases = efenc::Split(f[6], '/');
        if (phases.size() != 3)
            return std::nullopt;
        End e;
        e.fight = *fight;
        e.report.outcome = static_cast<Outcome>(f[1][0]);
        e.report.durationMs = *dur;
        e.report.peakTension = *peak;
        e.report.over900Ms = *over;
        e.report.inputSeconds = *inputs;
        for (std::size_t k = 0; k < 3; ++k)
        {
            auto v = u32(phases[k]);
            if (!v)
                return std::nullopt;
            e.report.phaseEndMs[k] = *v;
        }
        return e;
    }

    inline std::string EncodeResult(std::uint32_t seq, Result const& r)
    {
        efenc::Message m = Msg(seq, "RESULT");
        if (r.ok)
            m.fields = {std::to_string(r.fight), "OK", std::to_string(r.item), std::to_string(r.sizeMm),
                        std::to_string(r.grams), r.best ? "1" : "0"};
        else
            m.fields = {std::to_string(r.fight), "REJ", r.code};
        return efenc::Encode(m);
    }

    inline std::optional<Result> DecodeResult(efenc::Message const& m)
    {
        if (m.kind != efenc::kFishing || m.cmd != "RESULT" || m.fields.size() < 3)
            return std::nullopt;
        auto const& f = m.fields;
        auto fight = efenc::ParseUint<std::uint32_t>(f[0]);
        if (!fight)
            return std::nullopt;
        Result r;
        r.fight = *fight;
        if (f[1] == "REJ" && f.size() == 3)
        {
            r.code = f[2];
            return r;
        }
        if (f[1] != "OK" || f.size() != 6)
            return std::nullopt;
        auto item = efenc::ParseUint<std::uint32_t>(f[2]), size = efenc::ParseUint<std::uint32_t>(f[3]),
             grams = efenc::ParseUint<std::uint32_t>(f[4]);
        if (!item || !size || !grams || (f[5] != "0" && f[5] != "1"))
            return std::nullopt;
        r.ok = true;
        r.item = *item;
        r.sizeMm = *size;
        r.grams = *grams;
        r.best = f[5] == "1";
        return r;
    }

    inline std::string EncodeClose(std::uint32_t seq, Close const& c)
    {
        efenc::Message m = Msg(seq, "CLOSE");
        m.fields = {std::to_string(c.fight), c.reason};
        return efenc::Encode(m);
    }

    inline std::optional<Close> DecodeClose(efenc::Message const& m)
    {
        if (m.kind != efenc::kFishing || m.cmd != "CLOSE" || m.fields.size() != 2)
            return std::nullopt;
        auto fight = efenc::ParseUint<std::uint32_t>(m.fields[0]);
        if (!fight || m.fields[1].empty())
            return std::nullopt;
        return Close{*fight, m.fields[1]};
    }
}

#endif
