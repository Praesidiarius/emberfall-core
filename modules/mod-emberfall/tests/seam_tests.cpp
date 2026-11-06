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

// Standalone tests of the seam guard's logic (src/seam/EfSeamLogic.h; tests/run_tests.sh builds and runs them): which
// area is beyond the seam on which map, remembering the last spot, back or home, who is left alone, the wording.

#include "EfSeamLogic.h"

#include <iostream>
#include <string>

using namespace Emberfall::Seam;

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

    // The two rows the content repo writes: map 1's margin of the south, map 754's margin of the north.
    Areas const kAreas = { { 5401, 1 }, { 5402, 754 } };

    Seen At(uint32_t map, uint32_t zone, uint32_t area, float x = 100.0f)
    {
        Seen s;
        s.map = map;
        s.zone = zone;
        s.area = area;
        s.here = { map, x, 200.0f, 30.0f, 1.5f };
        return s;
    }

    void TestBeyond()
    {
        check(IsBeyond(kAreas, 1, 5401, 5401), "map 1's margin zone is beyond");
        check(IsBeyond(kAreas, 754, 5402, 5402), "map 754's margin zone is beyond");
        check(IsBeyond(kAreas, 1, 5401, 6000), "a subarea of the margin zone is beyond");
        check(IsBeyond(kAreas, 1, 6000, 5401), "the margin as a subarea is beyond");
        check(!IsBeyond(kAreas, 754, 5401, 5401), "map 1's margin area on map 754 is not");
        check(!IsBeyond(kAreas, 1, 5402, 5402), "map 754's margin area on map 1 is not");
        check(!IsBeyond(kAreas, 1, 400, 401), "an ordinary area is not");
        check(!IsBeyond({}, 1, 5401, 5401), "no rows: nothing is beyond");
    }

    void TestBack()
    {
        Memory m;
        check(Step(kAreas, At(1, 400, 401, 10.0f), m) == Act::Remember && m.has && m.spot.x == 10.0f,
              "own side: the spot is remembered");
        check(Step(kAreas, At(1, 400, 402, 20.0f), m) == Act::Remember && m.spot.x == 20.0f, "the newest spot wins");
        Memory const before = m;
        check(Step(kAreas, At(1, 5401, 5401, 30.0f), m) == Act::SendBack, "beyond: back");
        check(m.spot.x == 20.0f && m.spot.map == 1 && m.spot.o == before.spot.o, "beyond: the spot stays the last own one");
        check(Step(kAreas, At(1, 5401, 5401, 31.0f), m) == Act::SendBack, "still beyond: back again");
    }

    void TestHome()
    {
        Memory m;
        check(Step(kAreas, At(1, 5401, 5401), m) == Act::SendHome && !m.has, "beyond with no spot (login there): home");
        Memory other;
        Step(kAreas, At(754, 500, 501), other);
        check(Step(kAreas, At(1, 5401, 5401), other) == Act::SendHome, "a spot on the other map does not count: home");
    }

    void TestLeftAlone()
    {
        Memory m;
        Step(kAreas, At(1, 400, 401, 10.0f), m);

        Seen gm = At(1, 5401, 5401);
        gm.gm = true;
        check(Decide(kAreas, gm, m) == Act::None, "a GM is left alone");
        Seen flying = At(1, 5401, 5401);
        flying.moved = true;
        check(Decide(kAreas, flying, m) == Act::None, "a flight, ship or vehicle is left alone");
        Seen porting = At(1, 5401, 5401);
        porting.teleporting = true;
        check(Decide(kAreas, porting, m) == Act::None, "a teleport under way is left alone");

        Seen falling = At(1, 400, 401, 99.0f);
        falling.falling = true;
        check(Step(kAreas, falling, m) == Act::None && m.spot.x == 10.0f, "a spot in the air is not remembered");
        Seen fallingBeyond = At(1, 5401, 5401);
        fallingBeyond.falling = true;
        check(Decide(kAreas, fallingBeyond, m) == Act::SendBack, "falling into the margin: back");
        Seen gmOwn = At(1, 400, 401, 55.0f);
        gmOwn.gm = true;
        check(Step(kAreas, gmOwn, m) == Act::None && m.spot.x == 10.0f, "a GM's spot is not remembered");
    }

    void TestWording()
    {
        check(LangOf(0) == Lang::En && LangOf(2) == Lang::Fr && LangOf(3) == Lang::De && LangOf(8) == Lang::En,
              "locales: enUS, frFR, deDE, else English");
        check(Message(Lang::En) == "The ground ends here. Take a ship.", "English");
        check(Message(Lang::De) == "Hier endet das Land. Nimm ein Schiff.", "German");
        check(Message(Lang::Fr) == "La terre s'arrête ici. Prenez un navire.", "French");
    }
}

int main()
{
    TestBeyond();
    TestBack();
    TestHome();
    TestLeftAlone();
    TestWording();
    if (g_failed)
    {
        std::cerr << "seam_tests: " << g_failed << " of " << g_checks << " checks failed\n";
        return 1;
    }
    std::cout << "seam_tests: " << g_checks << " checks passed\n";
    return 0;
}
