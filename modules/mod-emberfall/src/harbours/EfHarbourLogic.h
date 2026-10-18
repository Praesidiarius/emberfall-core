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

#ifndef EMBERFALL_HARBOUR_LOGIC_H
#define EMBERFALL_HARBOUR_LOGIC_H

// The harbourmaster's answer (Emberfall docs/design/harbours-and-ships.md §3.3), without the core: which of a ship's
// stops is this pier's, and from the ship's place in its period whether it lies at the pier now (and sails in so many
// seconds) or when it is due. The script (emberfall_harbours.cpp) feeds it the transport template's stop frames and the
// live ship's path progress; tests/harbour_tests.cpp checks it standalone.

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Emberfall::Harbours
{
    // One stop frame of a ship's path: where (map, x, y) and when, in ms of its period (KeyFrame::ArriveTime /
    // DepartureTime).
    struct Stop
    {
        uint32_t map = 0;
        float x = 0.0f, y = 0.0f;
        uint32_t arrive = 0, depart = 0;
    };

    // The stop of `stops` on `map` nearest to (x, y) within `reach` yards (the pier's berth beside its harbourmaster).
    inline std::optional<Stop> BerthNear(std::vector<Stop> const& stops, uint32_t map, float x, float y, float reach = 90.0f)
    {
        std::optional<Stop> best;
        float bestD = reach;
        for (Stop const& s : stops)
        {
            if (s.map != map)
                continue;
            float const d = std::hypot(s.x - x, s.y - y);
            if (d <= bestD)
            {
                bestD = d;
                best = s;
            }
        }
        return best;
    }

    struct Sailing
    {
        bool atPier = false;      // the ship lies at the pier now
        uint32_t untilDepart = 0; // seconds until it sails (at the pier) or, due, until it sails after its next arrival
        uint32_t untilArrive = 0; // seconds until it comes in (0 when at the pier)
    };

    // Where the ship is in its timetable at `progress` ms (any value: wrapped into the period). A stop at the period's
    // start (arrive 0) is also reached at its end: the path's last key frame is the first berth again.
    inline Sailing NextSailing(Stop const& stop, uint64_t progress, uint32_t period)
    {
        Sailing s;
        if (!period)
            return s;
        uint32_t const t = uint32_t(progress % period);
        auto const secs = [](uint32_t ms) { return uint32_t((ms + 999) / 1000); };
        if (t >= stop.arrive && t < stop.depart)
        {
            s.atPier = true;
            s.untilDepart = secs(stop.depart - t);
            return s;
        }
        uint32_t const toArrive = t < stop.arrive ? stop.arrive - t : period - t + stop.arrive;
        s.untilArrive = secs(toArrive);
        s.untilDepart = secs(toArrive + (stop.depart - stop.arrive));
        return s;
    }

    // "1 min 20 s", "45 s", "2 min".
    inline std::string Duration(uint32_t seconds)
    {
        uint32_t const m = seconds / 60, s = seconds % 60;
        if (!m)
            return std::to_string(s) + " s";
        if (!s)
            return std::to_string(m) + " min";
        return std::to_string(m) + " min " + std::to_string(s) + " s";
    }

    // The gossip line: "The War Ferry is at the pier: it sails in 35 s." / "The War Ferry is due in 2 min 10 s and sails
    // 50 s after."
    inline std::string Describe(std::string const& ship, Sailing const& s, uint32_t waitSeconds)
    {
        if (s.atPier)
            return ship + " is at the pier: it sails in " + Duration(s.untilDepart) + ".";
        return ship + " is due in " + Duration(s.untilArrive) + " and sails " + Duration(waitSeconds) + " after.";
    }
}

#endif
