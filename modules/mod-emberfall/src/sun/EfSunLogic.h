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

#ifndef EMBERFALL_SUN_LOGIC_H
#define EMBERFALL_SUN_LOGIC_H

// The sun's hours (Emberfall docs/design/atmosphere.md "The sun's hours"), without the core: a day's sunrise and sunset
// for a place and a date (the NOAA sunrise equation), the three modes (real, softened, fixed), and the daily window of
// a game event that follows the sun (the daylight event). The sun clock itself, the parts of the day and the EFSUN
// message are EfSunTime.h, a verbatim copy of the client's. The script (emberfall_sun.cpp) feeds this the server's
// clock and its config; tests/sun_tests.cpp checks it standalone.

#include "EfSunTime.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace Emberfall::Sun
{
    // ---- dates (the proleptic Gregorian calendar; days since 1970-01-01) ------------------------------------------------
    struct Date
    {
        int year = 1970;
        int month = 1;   // 1-12
        int day = 1;     // 1-31
        constexpr bool operator==(Date const& o) const { return year == o.year && month == o.month && day == o.day; }
    };

    constexpr int64_t DaysFromCivil(Date d)
    {
        int64_t const y = d.month <= 2 ? d.year - 1 : d.year;
        int64_t const era = (y >= 0 ? y : y - 399) / 400;
        int64_t const yoe = y - era * 400;
        int64_t const doy = (153 * (d.month > 2 ? d.month - 3 : d.month + 9) + 2) / 5 + d.day - 1;
        int64_t const doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + doe - 719468;
    }

    constexpr Date CivilFromDays(int64_t z)
    {
        z += 719468;
        int64_t const era = (z >= 0 ? z : z - 146096) / 146097;
        int64_t const doe = z - era * 146097;
        int64_t const yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        int64_t const doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        int64_t const mp = (5 * doy + 2) / 153;
        int const day = int(doy - (153 * mp + 2) / 5 + 1);
        int const month = int(mp < 10 ? mp + 3 : mp - 9);
        return { int(yoe + era * 400 + (month <= 2 ? 1 : 0)), month, day };
    }

    constexpr Date AddDays(Date d, int days) { return CivilFromDays(DaysFromCivil(d) + days); }
    constexpr bool IsLeapYear(int year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }
    constexpr int DayOfYear(Date d) { return int(DaysFromCivil(d) - DaysFromCivil({ d.year, 1, 1 })) + 1; }

    // ---- the sun ---------------------------------------------------------------------------------------------------------
    struct Place
    {
        double latitude = 47.37;    // degrees north (Zurich)
        double longitude = 8.54;    // degrees east
    };

    // A place the equation can take: the latitude within the poles, the longitude once round.
    inline Place Sanitised(Place p)
    {
        if (!std::isfinite(p.latitude) || !std::isfinite(p.longitude))
            return {};
        p.latitude = p.latitude < -89.0 ? -89.0 : p.latitude > 89.0 ? 89.0 : p.latitude;
        p.longitude = p.longitude - 360.0 * std::floor((p.longitude + 180.0) / 360.0);
        return p;
    }

    enum class Sky : uint8_t { RisesAndSets = 0, NeverSets = 1, NeverRises = 2 };

    struct Solar
    {
        double sunrise = 0.0;   // hours of the local clock (utcOffset applied); may lie outside 0..24 far from its meridian
        double sunset = 0.0;
        Sky sky = Sky::RisesAndSets;
    };

    // The NOAA sunrise equation (NOAA Global Monitoring Laboratory, "General Solar Position Calculations": the
    // fractional year, the equation of time and the sun's declination as Spencer's Fourier series, the hour angle at a
    // zenith of 90.833 degrees: the sun's upper limb on the horizon with the usual refraction). Good to a minute or two
    // between the polar circles. `utcOffsetHours`: the local clock's offset from UTC on that date (daylight saving in).
    // A place where the sun does not rise or set that day: both hours at solar noon, and `sky` says which.
    inline Solar SolarHours(Date date, Place place, double utcOffsetHours)
    {
        constexpr double kPi = 3.14159265358979323846;
        constexpr double kRad = kPi / 180.0;
        place = Sanitised(place);
        double const days = IsLeapYear(date.year) ? 366.0 : 365.0;
        int const dayOfYear = DayOfYear(date);

        struct At
        {
            double equationOfTime = 0.0;   // minutes
            double cosHourAngle = 0.0;
        };
        // The sun at an hour (UTC) of that day.
        auto const at = [&](double hourUtc) {
            double const g = 2.0 * kPi / days * (dayOfYear - 1 + (hourUtc - 12.0) / 24.0);
            At a;
            a.equationOfTime = 229.18 * (0.000075 + 0.001868 * std::cos(g) - 0.032077 * std::sin(g) - 0.014615 * std::cos(2.0 * g) -
                                         0.040849 * std::sin(2.0 * g));
            double const declination = 0.006918 - 0.399912 * std::cos(g) + 0.070257 * std::sin(g) - 0.006758 * std::cos(2.0 * g) +
                                       0.000907 * std::sin(2.0 * g) - 0.002697 * std::cos(3.0 * g) + 0.00148 * std::sin(3.0 * g);
            double const latitude = place.latitude * kRad;
            a.cosHourAngle = std::cos(90.833 * kRad) / (std::cos(latitude) * std::cos(declination)) - std::tan(latitude) * std::tan(declination);
            return a;
        };
        // Minutes after midnight UTC of the rise (sign -1) or the set (+1) for the sun as it stands at `hourUtc`.
        auto const event = [&](At const& a, double sign) {
            double const hourAngle = std::acos(a.cosHourAngle < -1.0 ? -1.0 : a.cosHourAngle > 1.0 ? 1.0 : a.cosHourAngle) / kRad;
            return 720.0 - 4.0 * (place.longitude - sign * hourAngle) - a.equationOfTime;
        };

        Solar out;
        At const noon = at(12.0);
        if (noon.cosHourAngle > 1.0 || noon.cosHourAngle < -1.0)
        {
            out.sky = noon.cosHourAngle > 1.0 ? Sky::NeverRises : Sky::NeverSets;
            out.sunrise = out.sunset = (720.0 - 4.0 * place.longitude - noon.equationOfTime) / 60.0 + utcOffsetHours;
            return out;
        }
        // Once more with the sun as it stands at the rise and at the set themselves (it moves a little over the day).
        double const rise = event(at(event(noon, -1.0) / 60.0), -1.0);
        double const set = event(at(event(noon, 1.0) / 60.0), 1.0);
        out.sunrise = rise / 60.0 + utcOffsetHours;
        out.sunset = set / 60.0 + utcOffsetHours;
        return out;
    }

    // ---- the modes -------------------------------------------------------------------------------------------------------
    struct Settings
    {
        efsun::Mode mode = efsun::Mode::Real;
        Place place;
        efsun::SunHours fixed;   // the fixed hours (06:00, 19:45)
    };

    // The day's hours for a mode, as the server uses and sends them: a day the sun clock can take (EfSunTime.h Sanitised:
    // a polar day or night keeps an hour of night or day), in whole minutes.
    //   real      the place's own sunrise and sunset that day;
    //   softened  half way between the fixed hours and the real ones: the seasons with half the swing;
    //   fixed     the fixed hours.
    inline efsun::SunHours HoursFor(Settings const& s, Date date, double utcOffsetHours)
    {
        efsun::SunHours const fixed = efsun::Sanitised(s.fixed);
        if (s.mode == efsun::Mode::Fixed)
            return efsun::RoundedToMinutes(fixed);
        Solar const solar = SolarHours(date, s.place, utcOffsetHours);
        efsun::SunHours real{ solar.sunrise, solar.sunset };
        if (solar.sky == Sky::NeverSets)
            real = { solar.sunrise - 12.0, solar.sunset + 12.0 };
        else if (solar.sky == Sky::NeverRises)
            real = { solar.sunrise - efsun::kShortestSpan * 0.5, solar.sunset + efsun::kShortestSpan * 0.5 };
        real = efsun::Sanitised(real);
        if (s.mode == efsun::Mode::Softened)
            real = { (real.sunrise + fixed.sunrise) * 0.5, (real.sunset + fixed.sunset) * 0.5 };
        return efsun::Sanitised(efsun::RoundedToMinutes(real));
    }

    // ---- a game event that follows the sun (the daylight event: NPCs with hours, quest markers) ----------------------------
    // The event's data says when it runs on the nominal clock (game_event.start_time's time of day, its length: 06:00
    // for 840 minutes); it runs while the sun clock is inside that. One day's window on the real clock:
    struct Window
    {
        int64_t start = 0;       // Unix seconds
        uint32_t minutes = 0;    // its length
        constexpr int64_t end() const { return start + int64_t(minutes) * 60; }
        constexpr bool operator==(Window const& o) const { return start == o.start && minutes == o.minutes; }
    };

    // `midnight`: the Unix time of that day's local 00:00 (by the day's own UTC offset); `nominalStart`: hours on the sun
    // clock; `nominalMinutes`: the event's length there (under a day); `hours`: that day's sun.
    inline Window DayWindow(int64_t midnight, double nominalStart, uint32_t nominalMinutes, efsun::SunHours hours)
    {
        double const from = efsun::RealOfSunClock(nominalStart, hours);
        double const to = efsun::RealOfSunClock(nominalStart + nominalMinutes / 60.0, hours);
        double span = to - from;
        if (span <= 0.0 && nominalMinutes > 0)
            span += 24.0;   // over midnight
        Window w;
        w.start = midnight + int64_t(std::llround(from * 3600.0));
        w.minutes = uint32_t(std::lround(span * 60.0));
        if (w.minutes < 1)
            w.minutes = 1;
        return w;
    }

    // Of consecutive days' windows (yesterday, today, tomorrow, in that order): the one the event should have now: the
    // first that has not ended (the one running, else the next to come).
    inline Window CurrentWindow(int64_t now, Window const* days, std::size_t count)
    {
        for (std::size_t i = 0; i < count; ++i)
            if (now < days[i].end())
                return days[i];
        return count ? days[count - 1] : Window{};
    }

    // Whether the core's own check runs an event with this window now (GameEventMgr::CheckOneGameEvent, an event that
    // comes round once a day).
    constexpr bool Runs(Window const& w, int64_t now)
    {
        return w.start < now && (now - w.start) % 86400 < int64_t(w.minutes) * 60;
    }

    // "07:22" for an hour of the day.
    inline std::string ClockText(double hour)
    {
        int const minutes = ((efsun::MinutesOf(hour) % 1440) + 1440) % 1440;
        char text[8];
        std::snprintf(text, sizeof(text), "%02d:%02d", minutes / 60, minutes % 60);
        return text;
    }
}

#endif
