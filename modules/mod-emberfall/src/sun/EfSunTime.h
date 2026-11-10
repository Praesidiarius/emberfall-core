// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 the Emberfall authors.
//
// EfSunTime.h: the sun's hours and the "sun clock" (docs/design/atmosphere.md "The sun's hours"). The server works out
// each day's sunrise and sunset in its own local time (mod-emberfall src/emberfall_sun.cpp: the real ones for its place
// and date, softened ones or the fixed 06:00 / 19:45) and tells every client (the EFSUN addon whisper below). Everything
// that goes by the time of day reads it on the sun clock: the real clock mapped so that the day's sunrise falls on the
// nominal 06:00 and its sunset on the nominal 19:45, where the lights' time keys (lights.yaml), the sun's path
// (world/Lighting.h celestialAt) and fishing's windows (fishing.md §4.2) were written. The clock shown to the player
// stays the real one.
//
// This one file is shared: the client (client/src/data/) lights the world and draws the water clues with it, and
// mod-emberfall carries a verbatim copy (src/sun/EfSunTime.h) for the bite roll and the daylight event.
// tools/py/tests/test_sun_hours.py checks that the copies are equal; both test suites run the same vectors. Change both
// together, as EfFishRules.h. Header-only, standard library only.

#ifndef EMBERFALL_SUN_TIME_H
#define EMBERFALL_SUN_TIME_H

#include <charconv>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace efsun
{
    // The nominal hours: where the data has its sunrise and its sunset (lights.yaml's keys: sunrise 06:00, dusk 19:30
    // with the sun 3 degrees up, late dusk 21:00), and the sun's hours while no server said otherwise.
    constexpr double kNominalSunrise = 6.0;
    constexpr double kNominalSunset = 19.75;

    // A day's sunrise and sunset in hours of the server's local clock (0..24).
    struct SunHours
    {
        double sunrise = kNominalSunrise;
        double sunset = kNominalSunset;
    };
    constexpr bool IsNominal(SunHours h) { return h.sunrise == kNominalSunrise && h.sunset == kNominalSunset; }

    // An hour into 0..24.
    inline double Wrap24(double hour)
    {
        if (!std::isfinite(hour))
            return 12.0;
        const double w = hour - 24.0 * std::floor(hour / 24.0);
        return w >= 24.0 ? 0.0 : w;
    }

    // Hours the mapping can work with: both inside the day, an hour of day and an hour of night at the least (a polar
    // day or night, a place whose sun sets past its clock's midnight). Anything not a number: the nominal hours.
    constexpr double kShortestSpan = 1.0;
    inline SunHours Sanitised(SunHours h)
    {
        if (!std::isfinite(h.sunrise) || !std::isfinite(h.sunset))
            return {};
        h.sunrise = h.sunrise < 0.0 ? 0.0 : h.sunrise > 24.0 - kShortestSpan ? 24.0 - kShortestSpan : h.sunrise;
        if (h.sunset < h.sunrise + kShortestSpan)
            h.sunset = h.sunrise + kShortestSpan;
        if (h.sunset > 24.0)
            h.sunset = 24.0;
        if (h.sunset - h.sunrise > 24.0 - kShortestSpan)
            h.sunset = h.sunrise + 24.0 - kShortestSpan;
        return h;
    }

    // The sun clock (0..24) at an hour of the real clock: two straight pieces, the day from sunrise to sunset onto
    // 06:00-19:45 and the night from sunset to the next sunrise onto 19:45-06:00. The day's middle (solar noon) falls
    // on 12:52:30, the night's on 00:52:30. With the nominal hours it is the real clock, bit for bit.
    inline double SunClock(double realHour, SunHours hours)
    {
        const double t = Wrap24(realHour);
        if (IsNominal(hours))
            return t;
        const SunHours h = Sanitised(hours);
        const double day = h.sunset - h.sunrise;
        const double nominalDay = kNominalSunset - kNominalSunrise;
        if (t >= h.sunrise && t <= h.sunset)
            return Wrap24(kNominalSunrise + (t - h.sunrise) / day * nominalDay);
        const double sinceSunset = t > h.sunset ? t - h.sunset : t + 24.0 - h.sunset;
        return Wrap24(kNominalSunset + sinceSunset / (24.0 - day) * (24.0 - nominalDay));
    }

    // The other way: the hour of the real clock at which the sun clock reads `sunHour`.
    inline double RealOfSunClock(double sunHour, SunHours hours)
    {
        const double s = Wrap24(sunHour);
        if (IsNominal(hours))
            return s;
        const SunHours h = Sanitised(hours);
        const double day = h.sunset - h.sunrise;
        const double nominalDay = kNominalSunset - kNominalSunrise;
        if (s >= kNominalSunrise && s <= kNominalSunset)
            return Wrap24(h.sunrise + (s - kNominalSunrise) / nominalDay * day);
        const double sinceSunset = s > kNominalSunset ? s - kNominalSunset : s + 24.0 - kNominalSunset;
        return Wrap24(h.sunset + sinceSunset / (24.0 - nominalDay) * (24.0 - day));
    }

    // --- the parts of the day, read on the sun clock (fishing.md §4.2's windows) ------------------------------------
    enum class Part : std::uint8_t { Dawn = 0, Day = 1, Dusk = 2, Night = 3 };
    constexpr double kDawnFrom = 5.0, kDayFrom = 8.0, kDuskFrom = 18.0, kNightFrom = 21.0;

    inline Part PartOfSunHour(double sunHour)
    {
        const double s = Wrap24(sunHour);
        if (s >= kDawnFrom && s < kDayFrom)
            return Part::Dawn;
        if (s >= kDayFrom && s < kDuskFrom)
            return Part::Day;
        if (s >= kDuskFrom && s < kNightFrom)
            return Part::Dusk;
        return Part::Night;
    }
    // Which part of the day it is at an hour of the real clock.
    inline Part PartAt(double realHour, SunHours hours) { return PartOfSunHour(SunClock(realHour, hours)); }
    constexpr const char* PartName(Part p)
    {
        return p == Part::Dawn ? "dawn" : p == Part::Day ? "day" : p == Part::Dusk ? "dusk" : "night";
    }

    // --- the modes (the server's setting Emberfall.Sun.Mode) --------------------------------------------------------
    enum class Mode : std::uint8_t { Real = 0, Softened = 1, Fixed = 2 };
    constexpr std::string_view ModeName(Mode m)
    {
        return m == Mode::Real ? "real" : m == Mode::Softened ? "softened" : "fixed";
    }
    inline std::optional<Mode> ModeOf(std::string_view name)
    {
        for (Mode m : {Mode::Real, Mode::Softened, Mode::Fixed})
            if (name == ModeName(m))
                return m;
        return std::nullopt;
    }

    // --- the wire: EFSUN (an addon whisper from the server in the player's own name, as EFU) -------------------------
    //
    //   server -> client   EFSUN\t<ver>;<mode>;<sunrise>;<sunset>     the day's hours in whole minutes after the
    //                                                                 server's local midnight, e.g. 1;real;442;1018
    //
    // Sent after a login, when the server's day changes and when the mode or the place changes. Nothing goes from the
    // client to the server. A client that does not know the prefix ignores it (an EF prefix is reserved); a client on a
    // server that never sends it keeps the nominal hours. A later version may add fields after these four: a reader
    // takes the four it knows from any version from 1 on.
    constexpr std::string_view kPrefix = "EFSUN";
    constexpr int kWireVersion = 1;

    // Hours as the wire carries them: whole minutes. The server rounds its own values the same way before it uses
    // them, so both sides read the same sun clock.
    inline int MinutesOf(double hour) { return static_cast<int>(std::lround(hour * 60.0)); }
    inline SunHours RoundedToMinutes(SunHours h) { return {MinutesOf(h.sunrise) / 60.0, MinutesOf(h.sunset) / 60.0}; }

    struct Message
    {
        std::string mode; // "real", "softened", "fixed" (a later server may name another)
        SunHours hours;
    };

    inline std::string Encode(Mode mode, SunHours hours)
    {
        return std::to_string(kWireVersion) + ";" + std::string(ModeName(mode)) + ";" + std::to_string(MinutesOf(hours.sunrise)) +
               ";" + std::to_string(MinutesOf(hours.sunset));
    }

    // One EFSUN body (the text after "EFSUN\t"); nullopt for anything malformed or out of range.
    inline std::optional<Message> Decode(std::string_view body)
    {
        std::string_view field[4];
        for (int i = 0; i < 4; ++i)
        {
            const std::size_t end = body.find(';');
            field[i] = body.substr(0, end);
            if (end == std::string_view::npos)
            {
                if (i < 3)
                    return std::nullopt;
                body = {};
            }
            else
                body.remove_prefix(end + 1);
        }
        const auto number = [](std::string_view text) -> std::optional<int> {
            int value = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (text.empty() || error != std::errc() || end != text.data() + text.size())
                return std::nullopt;
            return value;
        };
        const auto version = number(field[0]);
        const auto sunrise = number(field[2]);
        const auto sunset = number(field[3]);
        if (!version || *version < 1 || field[1].empty() || field[1].size() > 16 || !sunrise || !sunset)
            return std::nullopt;
        if (*sunrise < 0 || *sunset > 1440 || *sunset - *sunrise < 60 || *sunset - *sunrise > 1380)
            return std::nullopt;
        return Message{std::string(field[1]), SunHours{*sunrise / 60.0, *sunset / 60.0}};
    }
}

#endif
