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

// Standalone tests of the sun's hours (src/sun/EfSunLogic.h and the shared src/sun/EfSunTime.h; tests/run_tests.sh
// builds and runs them): the solar calculation against Zurich's sunrises and sunsets through the year, the three modes,
// the sun clock (the vectors the client's tests/SunClockTests.cpp shares), the parts of the day at the windows' edges in
// winter and in summer and their agreement with fishing's buckets, the EFSUN message, and the daylight event's window.

#include "EfFishLogic.h"
#include "EfSunLogic.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

using namespace Emberfall::Sun;
using efsun::Mode;
using efsun::Part;
using efsun::SunHours;

namespace
{
    int g_failed = 0, g_checks = 0;

    void check(bool ok, std::string const& what)
    {
        ++g_checks;
        if (!ok)
        {
            ++g_failed;
            std::cerr << "FAIL: " << what << "\n";
        }
    }

    constexpr Place kZurich{ 47.37, 8.54 };
    constexpr double kWinterTime = 1.0, kSummerTime = 2.0;   // Europe/Zurich's UTC offsets

    double At(int hour, int minute) { return hour + minute / 60.0; }

    // `hour` within `minutes` of `want`.
    bool Near(double hour, double want, double minutes) { return std::fabs(hour - want) * 60.0 <= minutes; }

    void expectSun(Date date, double offset, double sunrise, double sunset, double minutes, std::string const& what)
    {
        Solar const s = SolarHours(date, kZurich, offset);
        check(s.sky == Sky::RisesAndSets, what + ": the sun rises and sets");
        check(Near(s.sunrise, sunrise, minutes), what + ": sunrise " + ClockText(s.sunrise) + ", wanted " + ClockText(sunrise));
        check(Near(s.sunset, sunset, minutes), what + ": sunset " + ClockText(s.sunset) + ", wanted " + ClockText(sunset));
    }

    Settings In(Mode mode)
    {
        Settings s;
        s.mode = mode;
        return s;
    }

    double Apart(double a, double b) { return std::min(efsun::Wrap24(a - b), efsun::Wrap24(b - a)); }

    // ---- the calendar ------------------------------------------------------------------------------------------------
    void TestDates()
    {
        check(DaysFromCivil({ 1970, 1, 1 }) == 0 && DaysFromCivil({ 2000, 3, 1 }) == 11017 && DaysFromCivil({ 2026, 11, 10 }) == 20767,
              "days since 1970");
        for (int64_t day : { int64_t(-1), int64_t(0), int64_t(59), int64_t(11016), int64_t(20767), int64_t(24836) })
            check(DaysFromCivil(CivilFromDays(day)) == day, "a date and back");
        check(CivilFromDays(20767) == Date{ 2026, 11, 10 }, "20767 is 10 November 2026");
        check(AddDays({ 2026, 12, 31 }, 1) == Date{ 2027, 1, 1 } && AddDays({ 2028, 3, 1 }, -1) == Date{ 2028, 2, 29 } &&
                  AddDays({ 2027, 3, 1 }, -1) == Date{ 2027, 2, 28 }, "the day after, the day before");
        check(DayOfYear({ 2026, 1, 1 }) == 1 && DayOfYear({ 2026, 11, 10 }) == 314 && DayOfYear({ 2028, 12, 31 }) == 366, "the day of the year");
        check(IsLeapYear(2028) && !IsLeapYear(2026) && !IsLeapYear(2100) && IsLeapYear(2000), "leap years");
    }

    // ---- the sun over Zurich -------------------------------------------------------------------------------------------
    void TestSolar()
    {
        // The reference values of the brief (local time, "within about 3 minutes"; they are round numbers themselves).
        expectSun({ 2026, 11, 10 }, kWinterTime, At(7, 25), At(16, 55), 4.5, "10 November");
        expectSun({ 2026, 12, 21 }, kWinterTime, At(8, 10), At(16, 40), 3.5, "the winter solstice");
        expectSun({ 2026, 6, 21 }, kSummerTime, At(5, 30), At(21, 25), 3.0, "the summer solstice (summer time)");
        // The same days and the equinoxes against the almanac (Zurich's published times, to the minute).
        expectSun({ 2026, 11, 10 }, kWinterTime, At(7, 22), At(16, 57), 2.0, "10 November (almanac)");
        expectSun({ 2026, 12, 21 }, kWinterTime, At(8, 11), At(16, 37), 2.0, "the winter solstice (almanac)");
        expectSun({ 2026, 6, 21 }, kSummerTime, At(5, 29), At(21, 26), 2.0, "the summer solstice (almanac)");
        expectSun({ 2026, 3, 20 }, kWinterTime, At(6, 30), At(18, 38), 3.0, "the spring equinox (winter time)");
        expectSun({ 2026, 9, 23 }, kSummerTime, At(7, 13), At(19, 23), 3.0, "the autumn equinox (summer time)");
        // An equinox day is a little over twelve hours (the refraction, the sun's disc), its middle at Zurich's solar noon.
        Solar const equinox = SolarHours({ 2026, 3, 20 }, kZurich, kWinterTime);
        check(Near(equinox.sunset - equinox.sunrise, At(12, 8), 4.0), "the equinox's day is about 12 h 8 min");
        check(Near((equinox.sunrise + equinox.sunset) * 0.5, At(12, 33), 2.0), "solar noon at the equinox about 12:33");

        // A daylight saving change: the server's clock gives the day's offset, and the local hours move with it. The
        // moments themselves move by the season's two minutes a day.
        Solar const before = SolarHours({ 2026, 3, 28 }, kZurich, kWinterTime), after = SolarHours({ 2026, 3, 29 }, kZurich, kSummerTime);
        check(Near(after.sunrise - before.sunrise, At(0, 58), 1.0), "summer time's first sunrise is 58 min later on the clock");
        check(Near(after.sunset - before.sunset, At(1, 1), 1.0), "and its sunset 61 min later");
        expectSun({ 2026, 3, 29 }, kSummerTime, At(7, 13), At(19, 50), 3.0, "the day summer time starts");
        Solar const last = SolarHours({ 2026, 10, 24 }, kZurich, kSummerTime), first = SolarHours({ 2026, 10, 25 }, kZurich, kWinterTime);
        check(Near(last.sunrise - first.sunrise, At(0, 59), 1.5), "winter time's first sunrise is about an hour earlier on the clock");
        expectSun({ 2026, 10, 25 }, kWinterTime, At(6, 57), At(17, 22), 3.0, "the day winter time starts");
        // The same moment whatever the clock's offset.
        check(Near(SolarHours({ 2026, 6, 21 }, kZurich, 0.0).sunrise + 2.0, SolarHours({ 2026, 6, 21 }, kZurich, 2.0).sunrise, 0.001),
              "the offset only moves the clock");

        // Through the year: no day jumps (under four minutes from one day to the next), the days lengthen from the
        // winter solstice to the summer solstice and shorten again, a leap day fits in.
        double longest = 0.0, shortest = 24.0;
        Date longestOn, shortestOn;
        for (int year : { 2026, 2028 })
            for (Date d{ year, 1, 1 }; d.year == year; d = AddDays(d, 1))
            {
                Solar const today = SolarHours(d, kZurich, kWinterTime), next = SolarHours(AddDays(d, 1), kZurich, kWinterTime);
                check(Near(next.sunrise, today.sunrise, 4.0) && Near(next.sunset, today.sunset, 4.0),
                      "day to day " + std::to_string(d.year) + "-" + std::to_string(d.month) + "-" + std::to_string(d.day));
                double const length = today.sunset - today.sunrise;
                if (year == 2026 && length > longest)
                    longest = length, longestOn = d;
                if (year == 2026 && length < shortest)
                    shortest = length, shortestOn = d;
            }
        check(longestOn.month == 6 && longestOn.day >= 19 && longestOn.day <= 23, "the longest day is the summer solstice");
        check(shortestOn.month == 12 && shortestOn.day >= 19 && shortestOn.day <= 23, "the shortest day is the winter solstice");
        check(Near(longest, At(15, 57), 4.0) && Near(shortest, At(8, 27), 4.0), "Zurich's longest and shortest day");

        // Elsewhere: the southern summer (Sydney, summer time UTC+11), the equator, the far north.
        Solar const sydney = SolarHours({ 2026, 12, 21 }, { -33.87, 151.21 }, 11.0);
        check(Near(sydney.sunrise, At(5, 41), 4.0) && Near(sydney.sunset, At(20, 5), 4.0), "Sydney's summer solstice");
        Solar const quito = SolarHours({ 2026, 6, 21 }, { -0.18, -78.47 }, -5.0);
        check(Near(quito.sunset - quito.sunrise, At(12, 7), 4.0), "twelve hours at the equator");
        check(SolarHours({ 2026, 12, 21 }, { 69.65, 18.96 }, 1.0).sky == Sky::NeverRises, "Tromso's polar night");
        check(SolarHours({ 2026, 6, 21 }, { 69.65, 18.96 }, 2.0).sky == Sky::NeverSets, "Tromso's midnight sun");
        check(SolarHours({ 2026, 3, 20 }, { 69.65, 18.96 }, 1.0).sky == Sky::RisesAndSets, "Tromso at the equinox");
        // A place out of range is brought in; nonsense is Zurich.
        check(Sanitised(Place{ 95.0, 370.0 }).latitude == 89.0 && Near(Sanitised(Place{ 95.0, 370.0 }).longitude, 10.0, 0.001) &&
                  Sanitised(Place{ std::nan(""), 0.0 }).latitude == 47.37 && Near(Sanitised(Place{ 0.0, -190.0 }).longitude, 170.0, 0.001),
              "a place brought into range");
    }

    // ---- the modes -------------------------------------------------------------------------------------------------------
    void TestModes()
    {
        Date const winter{ 2026, 12, 21 }, summer{ 2026, 6, 21 }, autumn{ 2026, 11, 10 };
        // fixed: as before, whatever the date.
        for (Date d : { winter, summer, autumn })
            check(efsun::IsNominal(HoursFor(In(Mode::Fixed), d, kWinterTime)), "fixed is 06:00 and 19:45");
        // real: the place's own hours, in whole minutes (what goes on the wire is what the server uses).
        SunHours const real = HoursFor(In(Mode::Real), autumn, kWinterTime);
        check(Near(real.sunrise, At(7, 22), 2.0) && Near(real.sunset, At(16, 57), 2.0), "real on 10 November");
        check(real.sunrise * 60.0 == std::floor(real.sunrise * 60.0 + 0.5) && efsun::RoundedToMinutes(real).sunset == real.sunset, "whole minutes");
        check(efsun::Decode(efsun::Encode(Mode::Real, real))->hours.sunrise == real.sunrise &&
                  efsun::Decode(efsun::Encode(Mode::Real, real))->hours.sunset == real.sunset, "the wire carries them exactly");
        // softened: half way between the fixed hours and the real ones, so half the swing through the year.
        for (Date d : { winter, summer, autumn })
        {
            double const offset = d == summer ? kSummerTime : kWinterTime;
            SunHours const r = HoursFor(In(Mode::Real), d, offset), s = HoursFor(In(Mode::Softened), d, offset);
            check(Near(s.sunrise, (r.sunrise + 6.0) * 0.5, 1.0) && Near(s.sunset, (r.sunset + 19.75) * 0.5, 1.0), "softened is half way");
        }
        SunHours const softWinter = HoursFor(In(Mode::Softened), winter, kWinterTime), softSummer = HoursFor(In(Mode::Softened), summer, kSummerTime);
        SunHours const realWinter = HoursFor(In(Mode::Real), winter, kWinterTime), realSummer = HoursFor(In(Mode::Real), summer, kSummerTime);
        check(Near(softSummer.sunset - softWinter.sunset, (realSummer.sunset - realWinter.sunset) * 0.5, 1.5), "softened: half the swing");
        check(Near(softWinter.sunrise, At(7, 5), 2.0) && Near(softWinter.sunset, At(18, 11), 2.0), "softened at midwinter: 07:05 and 18:11");
        // Other fixed hours: fixed gives them, softened leans on them.
        Settings own = In(Mode::Fixed);
        own.fixed = { 7.0, 18.0 };
        check(HoursFor(own, winter, kWinterTime).sunrise == 7.0 && HoursFor(own, winter, kWinterTime).sunset == 18.0, "the config's fixed hours");
        own.mode = Mode::Softened;
        check(Near(HoursFor(own, winter, kWinterTime).sunset, (realWinter.sunset + 18.0) * 0.5, 1.0), "softened towards them");
        // Another place: its own hours.
        Settings sydney = In(Mode::Real);
        sydney.place = { -33.87, 151.21 };
        check(Near(HoursFor(sydney, winter, 11.0).sunset, At(20, 5), 4.0), "real in Sydney");
        // The far north: a day the sun clock can take (an hour of day round noon, an hour of night at the least).
        Settings tromso = In(Mode::Real);
        tromso.place = { 69.65, 18.96 };
        SunHours const dark = HoursFor(tromso, winter, 1.0), bright = HoursFor(tromso, summer, 2.0);
        check(Near(dark.sunset - dark.sunrise, 1.0, 1.0) && Near((dark.sunrise + dark.sunset) * 0.5, At(11, 42), 5.0), "a polar night keeps an hour of day");
        check(Near(bright.sunset - bright.sunrise, 23.0, 1.0), "a midnight sun keeps an hour of night");
        for (SunHours h : { dark, bright })
            check(h.sunrise >= 0.0 && h.sunset <= 24.0 && efsun::Sanitised(h).sunrise == h.sunrise && efsun::Sanitised(h).sunset == h.sunset,
                  "polar hours are a day already");
        // The names.
        check(efsun::ModeOf("real") == Mode::Real && efsun::ModeOf("softened") == Mode::Softened && efsun::ModeOf("fixed") == Mode::Fixed &&
                  !efsun::ModeOf("Real") && !efsun::ModeOf(""), "the modes' names");
    }

    // ---- the sun clock: the vectors the client's tests share ----------------------------------------------------------------
    void TestSunClock()
    {
        // fixed: the real clock, bit for bit
        bool same = true;
        for (float h = 0.0f; h < 24.0f; h += 0.0137f)
            same = same && efsun::SunClock(h, {}) == double(h) && efsun::RealOfSunClock(h, {}) == double(h);
        check(same && efsun::SunClock(31.0, {}) == 7.0 && efsun::SunClock(-2.0, {}) == 22.0 && efsun::SunClock(24.0, {}) == 0.0,
              "the fixed hours are the real clock");

        SunHours const winter{ 7.5, 16.5 };
        check(Near(efsun::SunClock(7.5, winter), 6.0, 1e-6) && Near(efsun::SunClock(16.5, winter), 19.75, 1e-6), "sunrise is 06:00, sunset 19:45");
        check(Near(efsun::SunClock(12.0, winter), 12.875, 1e-6) && Near(efsun::SunClock(0.0, winter), 0.875, 1e-6), "solar noon 12:52:30, midnight 00:52:30");
        check(Near(efsun::SunClock(9.75, winter), 9.4375, 1e-6) && Near(efsun::SunClock(20.0, winter), 19.75 + 3.5 / 15.0 * 10.25, 1e-6) &&
                  Near(efsun::SunClock(4.5, winter), 3.95, 1e-6), "between");
        check(Near(efsun::RealOfSunClock(18.0, winter), 7.5 + 12.0 / 13.75 * 9.0, 1e-6) &&
                  Near(efsun::RealOfSunClock(21.0, winter), 16.5 + 1.25 / 10.25 * 15.0, 1e-6) &&
                  Near(efsun::RealOfSunClock(5.0, winter), 7.5 - 1.0 / 10.25 * 15.0, 1e-6), "and back");

        // monotonic and continuous round the day, across midnight, and back again, for every kind of day
        SunHours const days[] = { {}, winter, { 8.0 + 10.0 / 60.0, 16.0 + 37.0 / 60.0 }, { 5.0 + 29.0 / 60.0, 21.0 + 26.0 / 60.0 },
                                  { 0.0, 1.0 }, { 23.0, 24.0 }, { 0.5, 23.5 }, { 11.0, 13.0 } };
        for (SunHours const& day : days)
        {
            double const span = day.sunset - day.sunrise, steepest = std::max(13.75 / span, 10.25 / (24.0 - span)), step = 1.0 / 720.0;
            double total = 0.0;
            bool forward = true, smooth = true, back = true;
            for (int i = 0; i < 24 * 720; ++i)
            {
                double const moved = efsun::Wrap24(efsun::SunClock((i + 1) * step, day) - efsun::SunClock(i * step, day));
                forward = forward && moved > 0.0;
                smooth = smooth && moved <= steepest * step + 1e-9;
                total += moved;
                back = back && Apart(i * step, efsun::RealOfSunClock(efsun::SunClock(i * step, day), day)) < 1e-9;
            }
            check(forward && smooth && Near(total, 24.0, 1e-4), "the sun clock goes once round, never back, never in a jump");
            check(efsun::Wrap24(efsun::SunClock(0.0001, day) - efsun::SunClock(23.9999, day)) <= steepest * 0.0002 + 1e-9 &&
                      efsun::SunClock(24.0, day) == efsun::SunClock(0.0, day), "no step at midnight");
            check(back, "the real clock from the sun clock");
        }
        // what cannot be a day is made one
        auto const is = [](SunHours a, SunHours b) { return a.sunrise == b.sunrise && a.sunset == b.sunset; };
        check(is(efsun::Sanitised({ 12.0, 12.0 }), { 12.0, 13.0 }) && is(efsun::Sanitised({ 0.0, 24.0 }), { 0.0, 23.0 }) &&
                  is(efsun::Sanitised({ -3.0, 30.0 }), { 0.0, 23.0 }) && is(efsun::Sanitised({ 23.8, 23.9 }), { 23.0, 24.0 }) &&
                  is(efsun::Sanitised({ 18.0, 6.0 }), { 18.0, 19.0 }) && is(efsun::Sanitised({ std::nan(""), 19.0 }), {}), "hours made a day");
    }

    // ---- the parts of the day ---------------------------------------------------------------------------------------------
    void TestParts()
    {
        using Emberfall::Fishing::TimeBitOf;
        // fixed hours: fishing's buckets by the clock's hour, as before
        bool same = true;
        for (int hour = -1; hour < 25; ++hour)
            same = same && TimeBitOf(efsun::PartAt(hour, {})) == effish::TimeOfHour(hour) &&
                   TimeBitOf(efsun::PartAt(hour + 0.99, {})) == effish::TimeOfHour(hour);
        check(same, "with the fixed hours the parts are fishing's hours: dawn 05-08, day 08-18, dusk 18-21, night 21-05");
        check(TimeBitOf(Part::Dawn) == effish::kDawn && TimeBitOf(Part::Night) == effish::kNight, "the bits");

        // the real hours: each edge where the sun clock reads it, in winter and in summer
        SunHours const winter = HoursFor(In(Mode::Real), { 2026, 12, 21 }, kWinterTime), summer = HoursFor(In(Mode::Real), { 2026, 6, 21 }, kSummerTime);
        struct Edge
        {
            double sunHour;
            Part before, from;
        };
        Edge const edges[] = { { 5.0, Part::Night, Part::Dawn }, { 8.0, Part::Dawn, Part::Day }, { 18.0, Part::Day, Part::Dusk },
                               { 21.0, Part::Dusk, Part::Night } };
        for (SunHours const& day : { winter, summer, HoursFor(In(Mode::Softened), { 2026, 12, 21 }, kWinterTime) })
        {
            for (Edge const& e : edges)
            {
                double const real = efsun::RealOfSunClock(e.sunHour, day);
                check(efsun::PartAt(real - 0.01, day) == e.before && efsun::PartAt(real + 0.01, day) == e.from,
                      std::string("the edge of ") + efsun::PartName(e.from));
            }
            check(efsun::PartAt(day.sunrise, day) == Part::Dawn && efsun::PartAt(day.sunset, day) == Part::Dusk &&
                      efsun::PartAt((day.sunrise + day.sunset) * 0.5, day) == Part::Day && efsun::PartAt(1.0, day) == Part::Night,
                  "the sun rises in the dawn and sets in the dusk");
        }
        // Midwinter (08:10 / 16:37): dawn from about 06:39, day from 09:24, dusk from 15:33, night from 18:31.
        check(Near(efsun::RealOfSunClock(5.0, winter), At(6, 39), 3.0) && Near(efsun::RealOfSunClock(8.0, winter), At(9, 24), 3.0) &&
                  Near(efsun::RealOfSunClock(18.0, winter), At(15, 33), 3.0) && Near(efsun::RealOfSunClock(21.0, winter), At(18, 31), 3.0),
              "midwinter's windows on the real clock");
        check(efsun::PartAt(6.0, winter) == Part::Night && efsun::PartAt(9.0, winter) == Part::Dawn && efsun::PartAt(15.0, winter) == Part::Day &&
                  efsun::PartAt(16.0, winter) == Part::Dusk && efsun::PartAt(18.0, winter) == Part::Dusk && efsun::PartAt(19.0, winter) == Part::Night,
              "midwinter's hours");
        // Midsummer (05:29 / 21:26): dawn from about 04:42, day from 07:48, dusk from 19:24, night from 22:25.
        check(Near(efsun::RealOfSunClock(5.0, summer), At(4, 42), 3.0) && Near(efsun::RealOfSunClock(8.0, summer), At(7, 48), 3.0) &&
                  Near(efsun::RealOfSunClock(18.0, summer), At(19, 24), 3.0) && Near(efsun::RealOfSunClock(21.0, summer), At(22, 25), 3.0),
              "midsummer's windows on the real clock");
        check(efsun::PartAt(4.5, summer) == Part::Night && efsun::PartAt(5.0, summer) == Part::Dawn && efsun::PartAt(19.0, summer) == Part::Day &&
                  efsun::PartAt(20.0, summer) == Part::Dusk && efsun::PartAt(22.0, summer) == Part::Dusk && efsun::PartAt(22.5, summer) == Part::Night,
              "midsummer's hours");
        check(std::string(efsun::PartName(Part::Dawn)) == "dawn" && std::string(efsun::PartName(Part::Day)) == "day" &&
                  std::string(efsun::PartName(Part::Dusk)) == "dusk" && std::string(efsun::PartName(Part::Night)) == "night", "the parts' names");
    }

    // ---- EFSUN -----------------------------------------------------------------------------------------------------------------
    void TestWire()
    {
        SunHours const november{ 7.0 + 22.0 / 60.0, 16.0 + 58.0 / 60.0 };
        check(efsun::Encode(Mode::Real, november) == "1;real;442;1018" && efsun::Encode(Mode::Fixed, {}) == "1;fixed;360;1185" &&
                  efsun::Encode(Mode::Softened, { 6.5, 18.0 }) == "1;softened;390;1080", "the message");
        auto const m = efsun::Decode("1;real;442;1018");
        check(m && m->mode == "real" && m->hours.sunrise == november.sunrise && m->hours.sunset == november.sunset, "and read back");
        auto const later = efsun::Decode("2;lunar;300;1300;88;x");
        check(later && later->mode == "lunar" && later->hours.sunrise == 5.0, "a later version's first four fields");
        for (char const* bad : { "", "1", "1;real;442", "0;real;442;1018", "x;real;442;1018", "1;;442;1018", "1;real;-1;1018", "1;real;442;1441",
                                 "1;real;442;460", "1;real;10;1400", "1;real;44a;1018", "1;real;442;", "1;real;1018;442",
                                 "1;a-mode-name-far-too-long;442;1018" })
            check(!efsun::Decode(bad), std::string("refused: ") + bad);
        check(std::string(efsun::kPrefix) == "EFSUN" && std::string(efsun::kPrefix).size() + 1 + efsun::Encode(Mode::Softened, { 23.0, 24.0 }).size() < 255,
              "the prefix, and the longest message fits a chat line");
        check(ClockText(7.0 + 22.0 / 60.0) == "07:22" && ClockText(0.0) == "00:00" && ClockText(23.999) == "00:00" && ClockText(19.75) == "19:45" &&
                  ClockText(-0.5) == "23:30", "the clock's text");
    }

    // ---- the daylight event's window -----------------------------------------------------------------------------------------
    void TestEventWindow()
    {
        int64_t const midnight = DaysFromCivil({ 2026, 12, 21 }) * 86400 - 3600;   // local 00:00 at UTC+1
        // fixed: 06:00 for 840 minutes, as the data says
        Window const fixed = DayWindow(midnight, 6.0, 840, {});
        check(fixed.start == midnight + 6 * 3600 && fixed.minutes == 840 && fixed.end() == midnight + 20 * 3600, "the fixed window is the data's");
        // a 9-hour day: from sunrise to a little after sunset (20:00 on the sun clock)
        Window const winter = DayWindow(midnight, 6.0, 840, { 7.5, 16.5 });
        check(winter.start == midnight + 27000 && winter.minutes == 562, "the winter window: 07:30 for 562 minutes");
        // a night event (20:00 for 600 minutes): over midnight
        Window const night = DayWindow(midnight, 20.0, 600, { 7.5, 16.5 });
        check(night.start >= winter.end() - 60 && night.start <= winter.end() && night.minutes == 878,
              "a night window: from where the day's ends (in whole minutes) to the next sunrise");
        check(DayWindow(midnight, 6.0, 0, {}).minutes == 1, "never an empty window");

        // which window now: the one running, else the next to come
        Window const days[3] = { DayWindow(midnight - 86400, 6.0, 840, { 7.5, 16.5 }), winter, DayWindow(midnight + 86400, 6.0, 840, { 7.5, 16.5 }) };
        check(CurrentWindow(midnight + 3600, days, 3) == days[1] && !Runs(days[1], midnight + 3600), "before sunrise: today's, not yet");
        check(CurrentWindow(winter.start + 60, days, 3) == days[1] && Runs(days[1], winter.start + 60), "by day: today's, running");
        check(CurrentWindow(winter.end() + 60, days, 3) == days[2] && !Runs(days[2], winter.end() + 60), "after it: tomorrow's, not yet");
        check(CurrentWindow(midnight - 86400 + 12 * 3600, days, 3) == days[0], "yesterday noon: yesterday's");
        Window const nights[3] = { DayWindow(midnight - 86400, 20.0, 600, { 7.5, 16.5 }), night, DayWindow(midnight + 86400, 20.0, 600, { 7.5, 16.5 }) };
        check(CurrentWindow(midnight + 3600, nights, 3) == nights[0] && Runs(nights[0], midnight + 3600),
              "a night window that began yesterday still runs after midnight");
        check(CurrentWindow(midnight + 12 * 3600, nights, 3) == nights[1] && !Runs(nights[1], midnight + 12 * 3600), "and tonight's comes after it");

        // Three days of Zurich's midwinter and midsummer in five-minute steps, in each mode: with the window picked as the
        // script picks it, the core's own check runs the event exactly while the sun clock is within 06:00-20:00.
        struct Season
        {
            Date first;
            double offset;
        };
        for (Season const& season : { Season{ { 2026, 12, 20 }, kWinterTime }, Season{ { 2026, 6, 20 }, kSummerTime } })
            for (Mode mode : { Mode::Real, Mode::Softened, Mode::Fixed })
            {
                int64_t const offset = int64_t(season.offset * 3600);
                bool agrees = true;
                int running = 0;
                for (int64_t t = DaysFromCivil(season.first) * 86400 - offset; t < DaysFromCivil(AddDays(season.first, 3)) * 86400 - offset; t += 300)
                {
                    Date const today = CivilFromDays((t + offset) / 86400);
                    Window three[3];
                    for (int i = 0; i < 3; ++i)
                    {
                        Date const d = AddDays(today, i - 1);
                        three[i] = DayWindow(DaysFromCivil(d) * 86400 - offset, 6.0, 840, HoursFor(In(mode), d, season.offset));
                    }
                    double const sunClock = efsun::SunClock(((t + offset) % 86400) / 3600.0, HoursFor(In(mode), today, season.offset));
                    if (Apart(sunClock, 6.0) < 0.05 || Apart(sunClock, 20.0) < 0.05)
                        continue;   // within a few minutes of an edge: the window is in whole minutes
                    bool const runs = Runs(CurrentWindow(t, three, 3), t);
                    agrees = agrees && runs == (sunClock > 6.0 && sunClock < 20.0);
                    running += runs ? 1 : 0;
                }
                check(agrees, std::string("the event runs by the sun clock (") + std::string(efsun::ModeName(mode)) + ")");
                // by day about 8 h 45 min at midwinter, 16 h 05 at midsummer; 14 h fixed
                check(running > 0 && running < 3 * 288, "it runs part of the day");
            }
    }
}

int main()
{
    TestDates();
    TestSolar();
    TestModes();
    TestSunClock();
    TestParts();
    TestWire();
    TestEventWindow();
    if (g_failed)
    {
        std::cerr << "sun_tests: " << g_failed << " of " << g_checks << " checks failed\n";
        return 1;
    }
    std::cout << "sun_tests: " << g_checks << " checks passed\n";
    return 0;
}
