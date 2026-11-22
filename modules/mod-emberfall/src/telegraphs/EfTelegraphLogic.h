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

#pragma once

// Telegraphed areas (Emberfall docs/design/mob-abilities.md §2, §13): the one geometry of a creature's telegraphed cast
// and the rules for stepping out of it, shared by the companions (EfCompanionAI.cpp) and the bots (mod-playerbots'
// src/Emberfall/EmberfallTelegraphs.cpp). Plain C++20 over plain values, so tests/telegraph_tests.cpp and
// tests/companion_tests.cpp check it standalone. The core-facing half (a cast -> its area, the avoidable rule's data)
// is EfTelegraphs.h.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace Emberfall::Telegraphs
{
    // ---- the geometry ------------------------------------------------------------------------------------------------
    struct Vec2
    {
        float x = 0, y = 0;
    };

    inline float Dist(Vec2 a, Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

    // The smallest absolute difference between two angles (radians), 0..pi.
    inline float AngleDiff(float a, float b)
    {
        float d = std::fmod(std::fabs(a - b), 6.2831853f);
        return d > 3.14159265f ? 6.2831853f - d : d;
    }

    // The client's EfSpellTelegraph shapes (mob-abilities.md §2): a cone ahead of the caster, a circle round it, a circle
    // at the cast's target or point (Circle with another origin), a strip ahead of it.
    enum class DangerShape : uint8_t { None, Cone, Circle, Line };

    // A telegraphed area: a cone from `origin` facing `facing` (full `angle`, radians), a circle round `origin`, or a line
    // (a strip `width` yards to each side of the facing, `radius` long). The radius holds the core's leeway already (the
    // effect radius plus the bodies, mob-abilities.md §3), the caller's own size included.
    struct Danger
    {
        DangerShape shape = DangerShape::None;
        Vec2 origin;
        float radius = 0;
        float facing = 0;    // cone, line
        float angle = 0;     // cone only
        float width = 0;     // line only: half its width
    };

    // Inside, with `margin` yards to spare (a step short of the edge still counts as inside).
    inline bool InDanger(Danger const& d, Vec2 p, float margin)
    {
        float const dist = Dist(d.origin, p);
        if (d.shape == DangerShape::Circle)
            return dist <= d.radius + margin;
        if (d.shape == DangerShape::Line)
        {
            // the core's line (Position::HasInLine): the caster's front half, sideways under the width
            float const fx = std::cos(d.facing), fy = std::sin(d.facing);
            float const along = (p.x - d.origin.x) * fx + (p.y - d.origin.y) * fy;
            float const side = std::fabs(-(p.x - d.origin.x) * fy + (p.y - d.origin.y) * fx);
            return along >= -margin && along <= d.radius + margin && side <= d.width + margin;
        }
        if (d.shape != DangerShape::Cone || dist > d.radius + margin)
            return false;
        if (dist < 1.0f)
            return true;   // at the caster's feet: inside its boundary, which the core counts as in front
        float const toP = std::atan2(p.y - d.origin.y, p.x - d.origin.x);
        // The cone's edge widened by the margin at this distance.
        return AngleDiff(toP, d.facing) <= d.angle * 0.5f + std::atan2(margin, dist);
    }

    // Where to step to get out: for a circle straight out from its middle to `clear` yards past the edge (from the very
    // middle towards `prefer`); for a cone round the side nearer to us, past its edge by an angle worth `clear` yards,
    // at least 4 yd from the caster; for a line sideways past its edge. The shortest walk that ends outside.
    inline Vec2 EscapePoint(Danger const& d, Vec2 p, float clear, Vec2 prefer)
    {
        float const dist = Dist(d.origin, p);
        if (d.shape == DangerShape::Circle)
        {
            float a = std::atan2(p.y - d.origin.y, p.x - d.origin.x);
            if (dist < 0.3f)
                a = std::atan2(prefer.y - d.origin.y, prefer.x - d.origin.x);
            float const r = d.radius + clear;
            return Vec2{ d.origin.x + std::cos(a) * r, d.origin.y + std::sin(a) * r };
        }
        if (d.shape == DangerShape::Line)
        {
            float const fx = std::cos(d.facing), fy = std::sin(d.facing);
            float const along = (p.x - d.origin.x) * fx + (p.y - d.origin.y) * fy;
            float side = -(p.x - d.origin.x) * fy + (p.y - d.origin.y) * fx;
            if (std::fabs(side) < 0.1f)
                side = -(prefer.x - d.origin.x) * fy + (prefer.y - d.origin.y) * fx;
            float const s = (side >= 0 ? 1.0f : -1.0f) * (d.width + clear);
            return Vec2{ d.origin.x + fx * along - fy * s, d.origin.y + fy * along + fx * s };
        }
        float const r = std::max(dist, 4.0f);
        float const toP = dist < 0.3f ? std::atan2(prefer.y - d.origin.y, prefer.x - d.origin.x) : std::atan2(p.y - d.origin.y, p.x - d.origin.x);
        // Which side: the sign of the angle from the facing to us (wrapped to -pi..pi).
        float const rel = std::remainder(toP - d.facing, 6.2831853f);
        float const side = rel >= 0 ? 1.0f : -1.0f;
        float const out = d.angle * 0.5f + std::atan2(clear, r) + 0.15f;
        float const a = d.facing + side * out;
        return Vec2{ d.origin.x + std::cos(a) * r, d.origin.y + std::sin(a) * r };
    }

    // ---- the avoidable rule (mob-abilities.md §13; the game repo's avoidable_hits.py, formulas.yaml avoidable_hits) ----
    // A cast one can step out of: a telegraphed spell (an EfSpellTelegraph row, the client's marker) cast with a cast
    // bar (not triggered) of at least min_windup, with direct damage (a school_damage effect). The same data flags as
    // the rule's; no list of spell ids.
    constexpr uint32_t kMinWindupMs = 1500;   // formulas.yaml avoidable_hits.min_windup (1.5 s)

    struct CastFacts
    {
        bool telegraph = false;      // the spell has an EfSpellTelegraph row
        bool directDamage = false;   // a school_damage effect
        bool triggered = false;      // cast triggered: no cast bar, no marker
        bool byCreature = false;     // a creature's cast (not a player's)
        uint32_t castMs = 0;         // the cast's time (0: instant)
    };

    inline bool Avoidable(CastFacts const& c)
    {
        return c.telegraph && c.directDamage && !c.triggered && c.byCreature && c.castMs >= kMinWindupMs;
    }

    // Whether the bots watch a cast at all: their switch (Playerbots.Emberfall.AvoidTelegraphs) and the rule.
    inline bool Watches(bool switchOn, CastFacts const& c) { return switchOn && Avoidable(c); }

    // ---- the bots' reaction ([proposal]: Nathanael's "not perfect on purpose"; the figures in one place) --------------
    struct Reaction
    {
        uint32_t minMs = 400;        // a reaction is drawn evenly between these, per cast and bot
        uint32_t maxMs = 900;
        float lateChance = 0.15f;    // and one in this many reacts late by
        uint32_t lateMs = 600;
        uint32_t safetyMs = 150;     // out this long before the hit lands (the server's tick and the hit's leeway)
        float runSpeed = 7.0f;       // yards a second, a player's run
        float margin = 1.5f;         // a bot this close to the edge counts as inside
        float clear = 2.5f;          // and steps this far past it
    };

    // The reaction drawn from two rolls in [0, 1): one for the time, one for "late".
    inline uint32_t ReactionMs(Reaction const& r, float roll, float lateRoll)
    {
        roll = std::clamp(roll, 0.0f, 1.0f);
        uint32_t ms = r.minMs + uint32_t(float(r.maxMs - r.minMs) * roll);
        if (lateRoll < r.lateChance)
            ms += r.lateMs;
        return ms;
    }

    inline uint32_t WalkMs(Reaction const& r, float yards) { return uint32_t(std::max(yards, 0.0f) / r.runSpeed * 1000.0f); }

    // Out in time: the reaction plus the walk end before the hit lands, with the safety to spare.
    inline bool InTime(Reaction const& r, uint32_t reactMs, uint32_t walkMs, uint32_t castMs)
    {
        return uint64_t(reactMs) + walkMs + r.safetyMs <= castMs;
    }

    // On paper: the share of bots that are out in time from a cast of `castMs` with a walk of `walkYards`, the bot's
    // reaction tick `tickMs` (a bot looks every tickMs; on average it sees the reaction half a tick late).
    inline float EscapeShare(Reaction const& r, uint32_t castMs, float walkYards, uint32_t tickMs)
    {
        float const budget = float(castMs) - float(WalkMs(r, walkYards)) - float(r.safetyMs) - float(tickMs) * 0.5f;
        auto share = [&](float extra) {
            float const t = budget - extra;
            if (t < float(r.minMs))
                return 0.0f;
            if (t >= float(r.maxMs))
                return 1.0f;
            return (t - float(r.minMs)) / float(r.maxMs - r.minMs);
        };
        return (1.0f - r.lateChance) * share(0.0f) + r.lateChance * share(float(r.lateMs));
    }

    // A caster bot's own cast when it should step out ([proposal]): finished when it ends early enough to walk out after
    // it, else broken off at once.
    enum class OwnCast : uint8_t { None, Finish, Break };

    inline OwnCast OwnCastRule(Reaction const& r, uint32_t ownLeftMs, uint32_t walkMs, uint32_t hitLeftMs)
    {
        if (!ownLeftMs)
            return OwnCast::None;
        return uint64_t(ownLeftMs) + walkMs + r.safetyMs <= hitLeftMs ? OwnCast::Finish : OwnCast::Break;
    }

    // ---- the escape point -------------------------------------------------------------------------------------------
    struct Pool
    {
        Vec2 at;
        float radius = 0;
    };

    // The candidate spots out of `d` for someone at `self`, nearest first: the plain escape point, round the area's
    // middle (a circle), round both sides, past the reach and behind the caster (a cone), to both sides (a line).
    inline std::vector<Vec2> EscapeCandidates(Danger const& d, Vec2 self, float clear, Vec2 prefer)
    {
        std::vector<Vec2> out;
        out.push_back(EscapePoint(d, self, clear, prefer));
        float const dist = Dist(d.origin, self);
        if (d.shape == DangerShape::Circle)
        {
            float const r = d.radius + clear;
            float const a0 = dist < 0.3f ? std::atan2(prefer.y - d.origin.y, prefer.x - d.origin.x)
                                         : std::atan2(self.y - d.origin.y, self.x - d.origin.x);
            for (int k = 1; k <= 9; ++k)
                for (float sgn : { 1.0f, -1.0f })
                {
                    float const a = a0 + sgn * 0.35f * float(k);
                    out.push_back(Vec2{ d.origin.x + std::cos(a) * r, d.origin.y + std::sin(a) * r });
                }
        }
        else if (d.shape == DangerShape::Cone)
        {
            for (float r : { std::max(dist, 4.0f), std::max(dist, 4.0f) + 3.0f })
                for (float sgn : { 1.0f, -1.0f })
                {
                    float const a = d.facing + sgn * (d.angle * 0.5f + std::atan2(clear, r) + 0.15f);
                    out.push_back(Vec2{ d.origin.x + std::cos(a) * r, d.origin.y + std::sin(a) * r });
                }
            // straight on past its reach, and behind the caster
            float const toP = dist < 0.3f ? d.facing : std::atan2(self.y - d.origin.y, self.x - d.origin.x);
            out.push_back(Vec2{ d.origin.x + std::cos(toP) * (d.radius + clear), d.origin.y + std::sin(toP) * (d.radius + clear) });
            out.push_back(Vec2{ d.origin.x - std::cos(d.facing) * 4.0f, d.origin.y - std::sin(d.facing) * 4.0f });
        }
        else if (d.shape == DangerShape::Line)
        {
            float const fx = std::cos(d.facing), fy = std::sin(d.facing);
            float const along = std::max(0.0f, (self.x - d.origin.x) * fx + (self.y - d.origin.y) * fy);
            for (float step : { 0.0f, 3.0f, -3.0f })
                for (float sgn : { 1.0f, -1.0f })
                {
                    float const s = sgn * (d.width + clear), at = std::max(0.0f, along + step);
                    out.push_back(Vec2{ d.origin.x + fx * at - fy * s, d.origin.y + fy * at + fx * s });
                }
            out.push_back(Vec2{ d.origin.x + fx * (d.radius + clear), d.origin.y + fy * (d.radius + clear) });
        }
        std::stable_sort(out.begin(), out.end(), [&](Vec2 a, Vec2 b) { return Dist(self, a) < Dist(self, b); });
        return out;
    }

    // The nearest spot out of `d` (with `margin` to spare) that is in none of the `others` areas and no pool, and that
    // `open(spot)` accepts (the world's checks: the navmesh path, no ledge, no other pack's reach), asked at most
    // `maxOpen` times (it is the costly part). None: no such spot, the bot takes the hit.
    template <typename Open>
    std::optional<Vec2> PickEscape(Danger const& d, Vec2 self, float margin, float clear, Vec2 prefer, std::vector<Danger> const& others,
                                   std::vector<Pool> const& pools, Open const& open, uint32_t maxOpen = 6)
    {
        uint32_t asked = 0;
        for (Vec2 const& c : EscapeCandidates(d, self, clear, prefer))
        {
            if (InDanger(d, c, margin))
                continue;
            if (std::any_of(others.begin(), others.end(), [&](Danger const& o) { return InDanger(o, c, margin); }))
                continue;
            if (std::any_of(pools.begin(), pools.end(), [&](Pool const& p) { return Dist(p.at, c) <= p.radius + margin; }))
                continue;
            if (asked++ >= maxOpen)
                break;
            if (open(c))
                return c;
        }
        return std::nullopt;
    }
}
