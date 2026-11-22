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

// Standalone tests of the telegraphed areas and the bots' step out of them (src/telegraphs/EfTelegraphLogic.h;
// tests/run_tests.sh builds and runs them): will the area cover a spot when the cast ends, the escape point on a made-up
// floor, the reaction, the caster's own cast, nothing for an unavoidable or short cast, the switch.

#include "EfTelegraphLogic.h"

#include <cmath>
#include <iostream>
#include <string>

using namespace Emberfall::Telegraphs;

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

    constexpr float kPi = 3.14159265f;

    // ---- will it cover me when the cast ends -------------------------------------------------------------------------
    void TestCover()
    {
        // a 90-degree cone ahead (east), 10 yd plus the bodies
        Danger const cone{ DangerShape::Cone, Vec2{ 0, 0 }, 12.0f, 0.0f, 90.0f * kPi / 180.0f };
        check(InDanger(cone, Vec2{ 6, 0 }, 1.5f) && InDanger(cone, Vec2{ 6, 5 }, 1.5f), "cone: ahead is inside");
        check(!InDanger(cone, Vec2{ -4, 0 }, 1.5f) && !InDanger(cone, Vec2{ 2, 8 }, 0.0f), "cone: behind and to the side are outside");
        check(!InDanger(cone, Vec2{ 15, 0 }, 1.5f), "cone: past its reach is outside");

        // a ring round the caster and a ground circle elsewhere: the same shape, other middles
        Danger const ring{ DangerShape::Circle, Vec2{ 0, 0 }, 11.0f };
        Danger const ground{ DangerShape::Circle, Vec2{ 20, 20 }, 9.0f };
        check(InDanger(ring, Vec2{ 7, 7 }, 1.5f) && !InDanger(ring, Vec2{ 10, 10 }, 0.0f), "ring: its edge");
        check(InDanger(ground, Vec2{ 25, 22 }, 0.0f) && !InDanger(ground, Vec2{ 0, 0 }, 1.5f), "ground circle: at its own middle");

        // the margin: a step short of the edge counts as inside
        check(InDanger(ring, Vec2{ 12.0f, 0 }, 1.5f) && !InDanger(ring, Vec2{ 12.0f, 0 }, 0.0f), "the margin widens the area");

        // a line ahead (north), 2 yd to each side, 15 yd long
        Danger const line{ DangerShape::Line, Vec2{ 0, 0 }, 15.0f, kPi / 2.0f, 0.0f, 2.0f };
        check(InDanger(line, Vec2{ 0.5f, 10 }, 0.0f) && InDanger(line, Vec2{ -1.9f, 3 }, 0.0f), "line: on the strip is inside");
        check(!InDanger(line, Vec2{ 3, 10 }, 0.0f) && !InDanger(line, Vec2{ 0, -4 }, 0.0f), "line: beside it and behind are outside");
        check(!InDanger(line, Vec2{ 0, 18 }, 0.0f), "line: past its end is outside");
        Vec2 const sideways = EscapePoint(line, Vec2{ 0.5f, 8 }, 2.5f, Vec2{ 0, 0 });
        check(!InDanger(line, sideways, 1.5f) && std::fabs(sideways.y - 8.0f) < 0.01f, "line: out sideways at the same depth");
    }

    // ---- the escape point on a made-up floor -----------------------------------------------------------------------
    void TestFloor()
    {
        // a ring of 10 round a caster at (0,0); the bot at (3,0) in it. Open ground: straight out east.
        Danger const ring{ DangerShape::Circle, Vec2{ 0, 0 }, 10.0f };
        auto anywhere = [](Vec2) { return true; };
        auto spot = PickEscape(ring, Vec2{ 3, 0 }, 1.5f, 2.5f, Vec2{ 0, 0 }, {}, {}, anywhere);
        check(spot && spot->x > 12.0f && std::fabs(spot->y) < 0.01f, "open floor: straight out from the middle");

        // a cliff to the east (no floor past x = 8): out round the side, the shortest way that has floor
        auto cliffEast = [](Vec2 c) { return c.x < 8.0f; };
        auto round = PickEscape(ring, Vec2{ 3, 0 }, 1.5f, 2.5f, Vec2{ 0, 0 }, {}, {}, cliffEast, 32);
        check(round && round->x < 8.0f && !InDanger(ring, *round, 1.5f), "a cliff: round to where there is floor");

        // another telegraph covers the north side, a pool the south: neither is taken
        Danger const other{ DangerShape::Circle, Vec2{ 7, 9 }, 5.0f };
        std::vector<Pool> const pools{ Pool{ Vec2{ 7, -9 }, 5.0f } };
        auto squeezed = PickEscape(ring, Vec2{ 3, 0 }, 1.5f, 2.5f, Vec2{ 0, 0 }, { other }, pools, cliffEast, 32);
        check(squeezed && !InDanger(other, *squeezed, 1.5f) && Dist(pools[0].at, *squeezed) > 6.5f && squeezed->x < 8.0f,
              "never into another telegraph or a pool");

        // a cone: the bot steps round the nearer side, not across the cone
        Danger const cone{ DangerShape::Cone, Vec2{ 0, 0 }, 10.0f, 0.0f, 60.0f * kPi / 180.0f };
        auto side = PickEscape(cone, Vec2{ 5, 1 }, 1.5f, 2.5f, Vec2{ -5, 0 }, {}, {}, anywhere);
        check(side && side->y > 0.0f && !InDanger(cone, *side, 1.5f) && Dist(*side, Vec2{ 5, 1 }) < 6.0f, "cone: a short step round its near side");

        // no floor anywhere: no spot, the bot takes the hit (and the world is asked only a few times)
        int asked = 0;
        auto nowhere = [&](Vec2) { ++asked; return false; };
        check(!PickEscape(ring, Vec2{ 3, 0 }, 1.5f, 2.5f, Vec2{ 0, 0 }, {}, {}, nowhere, 6) && asked == 6, "no floor: none, after six asks");
    }

    // ---- the reaction ------------------------------------------------------------------------------------------------
    void TestReaction()
    {
        Reaction const r;
        check(ReactionMs(r, 0.0f, 0.99f) == r.minMs && ReactionMs(r, 0.999f, 0.99f) < r.maxMs + 1, "a reaction within its range");
        check(ReactionMs(r, 0.5f, 0.0f) == ReactionMs(r, 0.5f, 0.99f) + r.lateMs, "a late one comes later by its own figure");
        check(r.minMs >= 300 && r.maxMs <= 1000 && r.lateChance > 0.0f && r.lateChance < 0.5f, "human, not perfect");

        // a side-step (3 yd) and a walk out of a ring from its middle (12 yd)
        float const s15 = EscapeShare(r, 1500, 3.0f, 100), s20 = EscapeShare(r, 2000, 3.0f, 100), s25 = EscapeShare(r, 2500, 3.0f, 100);
        check(s15 < 1.0f && s15 > 0.5f, "a 1.5-s side-step still catches some");
        check(s20 >= s15 && s25 >= s20 && s25 > 0.99f, "longer wind-ups: more get out, a 2.5-s side-step nearly all");
        check(EscapeShare(r, 2000, 12.0f, 100) < s20 && EscapeShare(r, 1500, 12.0f, 100) == 0.0f, "a long walk: fewer, none in 1.5 s");
        check(InTime(r, 600, 400, 1500) && !InTime(r, 900, 600, 1500), "in time only with the safety to spare");
        check(WalkMs(r, 7.0f) == 1000, "a player's run");
    }

    // ---- a caster bot's own cast -------------------------------------------------------------------------------------
    void TestOwnCast()
    {
        Reaction const r;
        check(OwnCastRule(r, 0, 400, 1000) == OwnCast::None, "not casting: nothing to decide");
        check(OwnCastRule(r, 300, 400, 1500) == OwnCast::Finish, "a cast that ends in time is finished first");
        check(OwnCastRule(r, 1200, 400, 1500) == OwnCast::Break, "a cast that would end after the hit is broken off");
        check(OwnCastRule(r, 900, 600, 1500) == OwnCast::Break, "finishing it would leave too little to walk out: broken off");
    }

    // ---- the rule: nothing for an unavoidable or short cast; the switch ----------------------------------------------
    void TestRule()
    {
        CastFacts c;
        c.telegraph = c.directDamage = c.byCreature = true;
        c.castMs = 2000;
        check(Avoidable(c) && Watches(true, c), "a creature's telegraphed 2-s hit");
        check(!Watches(false, c), "the switch off: nothing is watched");
        CastFacts s = c;
        s.castMs = kMinWindupMs - 100;
        check(!Avoidable(s), "a wind-up under the rule's minimum");
        s.castMs = kMinWindupMs;
        check(Avoidable(s), "exactly the minimum counts");
        CastFacts n = c;
        n.telegraph = false;
        check(!Avoidable(n), "no marker: nothing a player could see");
        n = c;
        n.directDamage = false;
        check(!Avoidable(n), "no direct damage (a stun, a pull): not the rule's");
        n = c;
        n.triggered = true;
        check(!Avoidable(n), "a triggered cast: no cast bar");
        n = c;
        n.byCreature = false;
        check(!Avoidable(n), "a player's cast");
        n = c;
        n.castMs = 0;
        check(!Avoidable(n), "an instant");
    }
}

int main()
{
    TestCover();
    TestFloor();
    TestReaction();
    TestOwnCast();
    TestRule();
    if (g_failed)
    {
        std::cerr << "telegraph tests: " << g_failed << " of " << g_checks << " checks failed\n";
        return 1;
    }
    std::cout << "telegraph tests: " << g_checks << " checks passed\n";
    return 0;
}
