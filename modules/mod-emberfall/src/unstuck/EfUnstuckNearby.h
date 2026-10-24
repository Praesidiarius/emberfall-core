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

// Nearby unstuck without the core (Emberfall docs/design/unstuck.md, "Nearby"): `.stuck nearby` frees a stuck player to
// the nearest good standing spot within 40 yd instead of the town. Which spots the module probes (rings of rising
// radius), what makes a probed spot good (walkable navmesh ground with solid floor under it, nothing around or above
// it, not under water, part of open ground, few hostiles about), which one wins, when the search stops, the rules
// (combat, dead, transports, battlegrounds, cooldown), the EFU state line and the messages in English and German.
// emberfall_unstuck.cpp probes the map; tests/unstuck_tests.cpp checks this.

#ifndef EMBERFALL_UNSTUCK_NEARBY_H
#define EMBERFALL_UNSTUCK_NEARBY_H

#include "EfUnstuckLogic.h"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Emberfall::Unstuck::Nearby
{
    // The search: rings around the player at these radii (yd), `perRing` points each (each ring turned half a step).
    struct Search
    {
        std::vector<float> radii = { 5, 8, 12, 16, 20, 25, 30, 35, 40 };
        int perRing = 12;
        // Once a good spot is found at radius r, the rings up to r + `settle` are still probed (a nearer floor may
        // lie a ring further out), then the search stops.
        float settle = 8;
    };

    struct Offset
    {
        float dx = 0, dy = 0, radius = 0;
    };

    // The probe points' offsets from the player, nearest ring first.
    inline std::vector<Offset> Rings(Search const& s)
    {
        std::vector<Offset> out;
        if (s.perRing <= 0)
            return out;
        float const step = 6.28318530718f / float(s.perRing);
        for (std::size_t r = 0; r < s.radii.size(); ++r)
        {
            float const turn = (r % 2) ? step * 0.5f : 0.0f;
            for (int i = 0; i < s.perRing; ++i)
            {
                float const a = turn + step * float(i);
                out.push_back({ std::cos(a) * s.radii[r], std::sin(a) * s.radii[r], s.radii[r] });
            }
        }
        return out;
    }

    // Whether the search stops before the ring at `nextRadius`: a good spot was found at `firstGoodRadius` and the
    // next ring lies beyond the settle distance.
    inline bool Stop(Search const& s, std::optional<float> firstGoodRadius, float nextRadius)
    {
        return firstGoodRadius && nextRadius > *firstGoodRadius + s.settle;
    }

    // What the core found at one probe point.
    struct Probe
    {
        float radius = 0;            // the ring it came from
        bool onMesh = false;         // a navmesh polygon near the point (the ground filter: no water, magma, slime)
        float x = 0, y = 0, z = 0;   // the spot: on the polygon, z the floor's height (map/vmap) when there is one
        bool floor = false;          // solid floor (terrain or a model) under the polygon point
        float floorGap = 0;          // |polygon height - floor height|
        bool headroom = false;       // nothing above the spot up to a player's height (vmap and game objects)
        int blockedSides = 0;        // of four short rays at chest height around the spot, how many hit something
        bool underWater = false;
        float openArea = 0;          // square yards of navmesh connected to the spot within the area radius
        int hostiles = 0;            // living hostile creatures near the spot
    };

    struct Rules
    {
        float minDistance = 5;       // 2D yd from the player: nearer is where they are stuck
        float maxDistance = 45;      // 2D yd: a ring point may snap a little outward
        float maxFloorGap = 1.5f;
        int maxBlockedSides = 1;     // two or more: inside or wedged against something
        float minOpenArea = 150;     // yd²: less is a closed pocket (inside a rock, a sealed room)
        int maxHostiles = 2;         // three or more: a camp
        float hostilePenalty = 12;   // score (yd) per hostile
        float climbWeight = 2;       // height difference counts this much more than the distance on the ground
    };

    enum class Reject
    {
        None,
        NoMesh,
        TooClose,
        TooFar,
        NoFloor,
        Headroom,
        Wedged,
        UnderWater,
        Pocket,
        Camp,
    };

    inline float Distance2D(float ax, float ay, float bx, float by)
    {
        float const dx = ax - bx, dy = ay - by;
        return std::sqrt(dx * dx + dy * dy);
    }

    // Why a probed spot is no good (Reject::None: good), seen from the player at (px, py).
    inline Reject Judge(Probe const& p, float px, float py, Rules const& r)
    {
        if (!p.onMesh)
            return Reject::NoMesh;
        float const d = Distance2D(p.x, p.y, px, py);
        if (d < r.minDistance)
            return Reject::TooClose;
        if (d > r.maxDistance)
            return Reject::TooFar;
        if (!p.floor || p.floorGap > r.maxFloorGap)
            return Reject::NoFloor;
        if (!p.headroom)
            return Reject::Headroom;
        if (p.blockedSides > r.maxBlockedSides)
            return Reject::Wedged;
        if (p.underWater)
            return Reject::UnderWater;
        if (p.openArea < r.minOpenArea)
            return Reject::Pocket;
        if (p.hostiles > r.maxHostiles)
            return Reject::Camp;
        return Reject::None;
    }

    // Lower is better: the distance on the ground, the climb weighted, hostiles nearby a penalty.
    inline float Score(Probe const& p, float px, float py, float pz, Rules const& r)
    {
        return Distance2D(p.x, p.y, px, py) + r.climbWeight * std::fabs(p.z - pz) + r.hostilePenalty * float(p.hostiles);
    }

    // The best good spot, if any.
    inline std::optional<std::size_t> Pick(std::vector<Probe> const& probes, float px, float py, float pz, Rules const& r)
    {
        std::optional<std::size_t> best;
        float bestScore = 0;
        for (std::size_t i = 0; i < probes.size(); ++i)
        {
            if (Judge(probes[i], px, py, r) != Reject::None)
                continue;
            float const s = Score(probes[i], px, py, pz, r);
            if (!best || s < bestScore)
            {
                best = i;
                bestScore = s;
            }
        }
        return best;
    }

    // The area of a polygon on the ground (x, y pairs; any winding), square yards.
    inline float PolygonArea(std::vector<float> const& xy)
    {
        std::size_t const n = xy.size() / 2;
        if (n < 3)
            return 0;
        float twice = 0;
        for (std::size_t i = 0; i < n; ++i)
        {
            std::size_t const j = (i + 1) % n;
            twice += xy[2 * i] * xy[2 * j + 1] - xy[2 * j] * xy[2 * i + 1];
        }
        return std::fabs(twice) * 0.5f;
    }

    // ---- the rules ---------------------------------------------------------------------------------------------------

    enum class Refusal
    {
        None,
        Off,          // Emberfall.Unstuck.Enable = 0 or Emberfall.Unstuck.Nearby = 0
        Dead,         // ghosts use the town unstuck: their graveyard
        Combat,
        Flight,       // on a flight path
        Transport,    // on a ship or zeppelin
        Vehicle,
        Battleground, // a battleground or arena
        Cooldown,
    };

    struct Ask
    {
        bool enabled = true;
        bool alive = true;
        bool inCombat = false;
        bool inFlight = false;
        bool onTransport = false;
        bool onVehicle = false;
        bool inBattleground = false;
        std::int64_t now = 0;         // seconds
        std::int64_t lastUse = 0;     // seconds; 0: never
        std::int64_t cooldown = 300;  // seconds
    };

    inline Refusal Check(Ask const& a)
    {
        if (!a.enabled)
            return Refusal::Off;
        if (!a.alive)
            return Refusal::Dead;
        if (a.inFlight)
            return Refusal::Flight;
        if (a.onTransport)
            return Refusal::Transport;
        if (a.onVehicle)
            return Refusal::Vehicle;
        if (a.inBattleground)
            return Refusal::Battleground;
        if (a.inCombat)
            return Refusal::Combat;
        if (Remaining(a.now, a.lastUse, a.cooldown) > 0)
            return Refusal::Cooldown;
        return Refusal::None;
    }

    // ---- what the client is told -------------------------------------------------------------------------------------

    // The EFU addon line (whispered to the player in their own name after every request and on `.stuck status`): the
    // seconds until nearby unstuck and town unstuck can be used again, -1 for off. "0 212".
    inline std::string StateLine(std::int64_t nearLeft, std::int64_t townLeft)
    {
        return std::to_string(nearLeft) + " " + std::to_string(townLeft);
    }

    // The messages (system chat, in the player's language; the client shows them in the middle of the screen too).
    // Every one starts with the prefix: "Unstuck: ", "Befreien: ".
    inline std::string Prefix(Lang l) { return l == Lang::De ? "Befreien: " : "Unstuck: "; }

    inline std::string Message(Refusal r, std::int64_t remaining, Lang l = Lang::En)
    {
        bool const de = l == Lang::De;
        std::string const p = Prefix(l);
        switch (r)
        {
            case Refusal::Off: return p + (de ? "in der Nähe befreien gibt es auf diesem Realm nicht." : "nearby unstuck is not available on this realm.");
            case Refusal::Dead: return p + (de ? "nicht als Geist; „Zur nächsten Stadt“ bringt dich zu deinem Friedhof." : "not while you are dead; \"To the nearest town\" takes you to your graveyard.");
            case Refusal::Combat: return p + (de ? "nicht im Kampf." : "not while you are in combat.");
            case Refusal::Flight: return p + (de ? "nicht während eines Flugs." : "not while you are flying.");
            case Refusal::Transport: return p + (de ? "nicht auf einem Schiff oder Transportmittel." : "not on a ship or other transport.");
            case Refusal::Vehicle: return p + (de ? "nicht in einem Fahrzeug." : "not while on a vehicle.");
            case Refusal::Battleground: return p + (de ? "nicht auf einem Schlachtfeld oder in einer Arena." : "not in a battleground or arena.");
            case Refusal::Cooldown: return p + (de ? "in der Nähe befreien wieder in " : "you can use nearby unstuck again in ") + Duration(remaining, l) + ".";
            default: return std::string();
        }
    }

    // Freed: "Unstuck: you are free, 12 yd from where you were."
    inline std::string Freed(float distance, Lang l = Lang::En)
    {
        std::string const yd = std::to_string(int(distance + 0.5f));
        return Prefix(l) + (l == Lang::De ? "du bist frei, " + yd + " m von deinem alten Platz." : "you are free, " + yd + " yd from where you were.");
    }

    // Nothing good within the search: the town unstuck takes over (its own rules and messages follow).
    inline std::string NothingNear(float maxRadius, Lang l = Lang::En)
    {
        std::string const yd = std::to_string(int(maxRadius + 0.5f));
        return Prefix(l) + (l == Lang::De ? "kein freier Platz im Umkreis von " + yd + " m; es geht zur nächsten Stadt."
                                          : "no free spot within " + yd + " yd; trying the nearest town.");
    }
}

#endif
