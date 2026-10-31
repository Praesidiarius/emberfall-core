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

// The boss mechanics toolkit without a core (Emberfall docs/design/dungeon-barrows.md §5b "The toolkit"): the pieces of a
// multi-phase five-player boss that every dungeon script can reuse, as plain state and step functions so that
// tests/dungeon_tests.cpp checks them without a server. EfBossKit.h is the core side (summoning a soak, pulsing an
// arena's edge, the energy bar on the target frame, the immune shroud); the Barrows' bosses are the first users.
//
//   CrossedThresholds   health thresholds, each once, in order, even through one big hit
//   PhasePlan           the phase for a health percentage (phase changes at thresholds)
//   Intermission        a timed or add-held phase in which the boss is immune; ends when its adds die or time is up
//   EnergyBar           a bar from 0 to 100 that fills by itself (and by events); the ultimate at 100
//   CastChain           several interruptible casts one after another (an interrupt rotation)
//   ResolveSoak         a soak circle: the damage shared among those in it, a group-wide failure below the number needed
//   ArenaRing           an arena that shrinks: who is outside the ring

#ifndef EMBERFALL_BOSS_TOOLKIT_H
#define EMBERFALL_BOSS_TOOLKIT_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Emberfall::Bosses
{
    // ---- timers -------------------------------------------------------------------------------------------------------
    // An event's first time after the pull (or its phase's start), then again within [min, max] (milliseconds).
    struct Timer
    {
        std::uint32_t first;
        std::uint32_t min;
        std::uint32_t max;
    };

    // Every repeat longer than the cast it starts plus a second: a mechanic never queues on top of itself.
    constexpr bool TimerFits(Timer t, std::uint32_t castMs) { return t.min <= t.max && t.min > castMs + 1000; }

    // ---- health thresholds -------------------------------------------------------------------------------------------
    // The thresholds of `list` (descending) crossed going to health `to` (percent) that have not fired yet (`fired`: a bit
    // per entry), in order; marks them fired. A big hit across two thresholds fires both, once each.
    template <std::size_t N>
    inline std::vector<std::size_t> CrossedThresholds(std::array<std::uint8_t, N> const& list, float to, std::uint32_t& fired)
    {
        std::vector<std::size_t> out;
        for (std::size_t i = 0; i < N; ++i)
            if (!(fired & (1u << i)) && to <= float(list[i]))
            {
                fired |= 1u << i;
                out.push_back(i);
            }
        return out;
    }

    // ---- phases --------------------------------------------------------------------------------------------------------
    // A fight's phases by health: phase 1 above the first threshold, phase k+1 at or below the k-th (descending). The boss
    // script keeps the phase it is in and changes it when PhaseFor says a later one; phases never go back (a heal above a
    // threshold keeps the phase), so a script asks NextPhase.
    template <std::size_t N>
    inline std::uint8_t PhaseFor(std::array<std::uint8_t, N> const& thresholds, float pct)
    {
        std::uint8_t phase = 1;
        for (std::size_t i = 0; i < N; ++i)
            if (pct <= float(thresholds[i]))
                phase = std::uint8_t(i + 2);
        return phase;
    }

    // The phase to change to from `current` at health `pct`: the later of the two (0: no change).
    template <std::size_t N>
    inline std::uint8_t NextPhase(std::array<std::uint8_t, N> const& thresholds, std::uint8_t current, float pct)
    {
        std::uint8_t const p = PhaseFor(thresholds, pct);
        return p > current ? p : 0;
    }

    // ---- intermission (the boss immune; adds or a timer end it) --------------------------------------------------------
    struct Intermission
    {
        bool active = false;
        std::uint32_t msLeft = 0;      // the time limit (0 with active: no limit)
        std::uint32_t addsAlive = 0;   // the adds that hold it (0: a timed intermission)
        bool timed = false;
    };

    // `adds` > 0: it ends with the last of them (or at `limitMs`, a safety net; 0 none); `adds` 0: it lasts `limitMs`.
    inline void StartIntermission(Intermission& i, std::uint32_t limitMs, std::uint32_t adds)
    {
        i.active = true;
        i.msLeft = limitMs;
        i.addsAlive = adds;
        i.timed = adds == 0;
    }

    // An add of it died: true when that ends the intermission.
    inline bool IntermissionAddDied(Intermission& i)
    {
        if (!i.active || i.timed || !i.addsAlive)
            return false;
        if (--i.addsAlive)
            return false;
        i.active = false;
        return true;
    }

    // Time runs: true when the limit ends it (whatever adds are left; the script decides what they do then).
    inline bool StepIntermission(Intermission& i, std::uint32_t diff)
    {
        if (!i.active || !i.msLeft)
            return false;
        i.msLeft = i.msLeft > diff ? i.msLeft - diff : 0;
        if (i.msLeft)
            return false;
        i.active = false;
        return true;
    }

    // ---- energy bar (the ultimate at 100) -------------------------------------------------------------------------------
    // Kept in thousandths so a slow fill (2.5 a second) loses nothing between updates.
    struct EnergyBar
    {
        std::uint32_t milli = 0;           // 0 .. 100000
        std::uint32_t perSecond = 0;       // energy a second (x1000 inside)
        bool paused = false;               // while the ultimate is cast, in an intermission
    };

    inline std::uint32_t Energy(EnergyBar const& e) { return e.milli / 1000; }

    // Time runs (`diff` ms): true when the bar has just reached 100 (it stays there until ResetEnergy).
    inline bool StepEnergy(EnergyBar& e, std::uint32_t diff)
    {
        if (e.paused || e.milli >= 100000)
            return false;
        e.milli = std::min<std::uint32_t>(100000, e.milli + e.perSecond * diff);
        return e.milli >= 100000;
    }

    // An event adds energy at once (a cinder bursting): true when that fills the bar.
    inline bool AddEnergy(EnergyBar& e, std::uint32_t energy)
    {
        if (e.milli >= 100000)
            return false;
        e.milli = std::min<std::uint32_t>(100000, e.milli + energy * 1000);
        return e.milli >= 100000;
    }

    inline void ResetEnergy(EnergyBar& e) { e.milli = 0; }

    // Seconds from empty to full at the bar's rate.
    inline float SecondsToFull(EnergyBar const& e) { return e.perSecond ? 100.0f / float(e.perSecond) : 0.0f; }

    // ---- cast chain (an interrupt rotation) ------------------------------------------------------------------------------
    // `count` casts, each started `gapMs` after the last one ended (or was interrupted).
    struct CastChain
    {
        std::uint32_t left = 0;     // casts still to start
        std::uint32_t nextIn = 0;   // ms until the next may start
        std::uint32_t gapMs = 0;
    };

    inline void StartChain(CastChain& c, std::uint32_t count, std::uint32_t gapMs)
    {
        c.left = count;
        c.nextIn = 0;
        c.gapMs = gapMs;
    }

    // Time runs: true when the next cast of the chain should start now. `casting`: the boss is still casting (the last
    // cast, or anything else): the gap counts from its end.
    inline bool StepChain(CastChain& c, std::uint32_t diff, bool casting)
    {
        if (!c.left)
            return false;
        if (casting)
        {
            c.nextIn = c.gapMs;
            return false;
        }
        c.nextIn = c.nextIn > diff ? c.nextIn - diff : 0;
        if (c.nextIn)
            return false;
        --c.left;
        c.nextIn = c.gapMs;
        return true;
    }

    // ---- soaks ------------------------------------------------------------------------------------------------------------
    // A circle that `needed` of the group must stand in when it goes off: `total` damage shared among them (each at least
    // `floor`, so a crowd still feels it). Fewer than needed: it fails (the script's failure: a group-wide hit, an add).
    struct SoakOutcome
    {
        bool failed = false;
        std::uint32_t perSoaker = 0;
    };

    inline SoakOutcome ResolveSoak(std::uint32_t soakers, std::uint32_t needed, std::uint32_t total, std::uint32_t floor)
    {
        SoakOutcome o;
        if (soakers < std::max<std::uint32_t>(needed, 1))
        {
            o.failed = true;
            return o;
        }
        o.perSoaker = std::max(floor, total / soakers);
        return o;
    }

    // ---- an arena that shrinks ------------------------------------------------------------------------------------------
    struct Point2
    {
        float x = 0, y = 0;
    };

    // Outside the ring of `radius` round `centre`, with the unit's own `reach` (its feet may stand on the edge).
    inline bool OutsideArena(Point2 p, Point2 centre, float radius, float reach)
    {
        return std::hypot(p.x - centre.x, p.y - centre.y) > radius + reach;
    }

    // The arena's radius after its `stage`-th shrink (stage 0: the whole room).
    template <std::size_t N>
    inline float ArenaRadius(float room, std::array<float, N> const& stages, std::size_t stage)
    {
        if (!stage)
            return room;
        return stages[std::min(stage, N) - 1];
    }

    // Inside any of the shelters (radius each): the safe spots of an ultimate.
    inline bool InAnyShelter(Point2 p, std::vector<Point2> const& shelters, float radius)
    {
        for (Point2 const& s : shelters)
            if (std::hypot(p.x - s.x, p.y - s.y) <= radius)
                return true;
        return false;
    }

    // `count` evenly spaced points on a circle of `radius` round `c`, the first at `start` radians.
    inline std::vector<Point2> RingPoints(Point2 c, float radius, std::uint32_t count, float start)
    {
        std::vector<Point2> out;
        for (std::uint32_t i = 0; i < count; ++i)
        {
            float const a = start + 6.2831853f * float(i) / float(count ? count : 1);
            out.push_back(Point2{ c.x + std::cos(a) * radius, c.y + std::sin(a) * radius });
        }
        return out;
    }
}

#endif
