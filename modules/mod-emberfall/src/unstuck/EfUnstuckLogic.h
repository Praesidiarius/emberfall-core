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

// The unstuck logic that needs no core (Emberfall docs/design/unstuck.md): where `.unstuck` sends a player and whether
// they may use it now. emberfall_unstuck.cpp uses it; tests/unstuck_tests.cpp (tests/run_tests.sh) checks it.

#ifndef EMBERFALL_UNSTUCK_LOGIC_H
#define EMBERFALL_UNSTUCK_LOGIC_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Emberfall::Unstuck
{
    // A town point: an ef_unstuck_point row (Emberfall content/data/world.yaml, teleports with `town:`).
    struct Point
    {
        std::uint32_t id = 0;
        std::uint32_t map = 0;
        float x = 0, y = 0, z = 0, o = 0;
        std::string name;
    };

    enum class Where
    {
        Town,       // the nearest town point on the player's map
        Entrance,   // an instance: its start point (the door's areatrigger_teleport target)
        Home,       // no town point on this map and not an instance: the hearth's home
    };

    struct Target
    {
        Where where = Where::Home;
        std::optional<std::size_t> town;   // index into the points, for Where::Town
    };

    // The nearest town point on `map` to (x, y, z) (3D distance), if any.
    inline std::optional<std::size_t> NearestTown(std::vector<Point> const& points, std::uint32_t map, float x, float y, float z)
    {
        std::optional<std::size_t> best;
        float bestD = 0;
        for (std::size_t i = 0; i < points.size(); ++i)
        {
            Point const& p = points[i];
            if (p.map != map)
                continue;
            float dx = p.x - x, dy = p.y - y, dz = p.z - z;
            float d = dx * dx + dy * dy + dz * dz;
            if (!best || d < bestD)
            {
                best = i;
                bestD = d;
            }
        }
        return best;
    }

    // Where the player goes: in an instance with a known entrance, the entrance; else the nearest town on the map;
    // else home.
    inline Target Choose(std::vector<Point> const& points, std::uint32_t map, bool instance, bool hasEntrance, float x,
                         float y, float z)
    {
        Target t;
        if (instance && hasEntrance)
        {
            t.where = Where::Entrance;
            return t;
        }
        t.town = NearestTown(points, map, x, y, z);
        t.where = t.town ? Where::Town : Where::Home;
        return t;
    }

    enum class Refusal
    {
        None,
        Off,        // Emberfall.Unstuck.Enable = 0
        Combat,
        Cooldown,
        Flight,     // on a flight path
    };

    struct Ask
    {
        bool enabled = true;
        bool alive = true;
        bool inCombat = false;
        bool inFlight = false;
        std::int64_t now = 0;         // seconds
        std::int64_t lastUse = 0;     // seconds; 0: never
        std::int64_t cooldown = 300;  // seconds
    };

    // Seconds until the player may use it again (0: now).
    inline std::int64_t Remaining(std::int64_t now, std::int64_t lastUse, std::int64_t cooldown)
    {
        if (lastUse <= 0 || cooldown <= 0)
            return 0;
        std::int64_t left = lastUse + cooldown - now;
        return left > 0 ? left : 0;
    }

    // Whether the player may use it now. The dead are released to their graveyard as the stock Stuck effect does: no
    // combat or cooldown rule for them (a graveyard is no fast travel).
    inline Refusal Check(Ask const& a)
    {
        if (!a.enabled)
            return Refusal::Off;
        if (a.inFlight)
            return Refusal::Flight;
        if (!a.alive)
            return Refusal::None;
        if (a.inCombat)
            return Refusal::Combat;
        if (Remaining(a.now, a.lastUse, a.cooldown) > 0)
            return Refusal::Cooldown;
        return Refusal::None;
    }

    // "4 min 12 s", "40 s".
    inline std::string Duration(std::int64_t seconds)
    {
        if (seconds < 60)
            return std::to_string(seconds) + " s";
        std::int64_t m = seconds / 60, s = seconds % 60;
        return std::to_string(m) + " min" + (s ? " " + std::to_string(s) + " s" : std::string());
    }

    // The message for a refusal (system chat; the client's Unstuck dialog shows it too: every one starts "Unstuck: ").
    inline std::string Message(Refusal r, std::int64_t remaining)
    {
        switch (r)
        {
            case Refusal::Off: return "Unstuck: not available on this realm.";
            case Refusal::Combat: return "Unstuck: not while you are in combat.";
            case Refusal::Cooldown: return "Unstuck: you can use it again in " + Duration(remaining) + ".";
            case Refusal::Flight: return "Unstuck: not while you are flying.";
            default: return std::string();
        }
    }
}

#endif
