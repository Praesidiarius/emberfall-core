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

// Standalone tests of the companion's size and growth (tests/run_tests.sh builds and runs them): src/petsize/EfPetSizeLogic.h,
// the stage arithmetic, the size by kind and stage, the object scale for a display, the reach and radius limits. Made-up
// stages and sizes: the numbers are the logic's, not the game's (content/data/pets.yaml `size`).

#include "EfPetSizeLogic.h"

#include <cmath>
#include <iostream>
#include <string>

using namespace Emberfall::PetSize;

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

    bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

    Rule Four()
    {
        Rule r;
        r.stages = {{0, 800}, {7200, 870}, {21600, 940}, {50400, 1000}};
        return r;
    }

    void TestStages()
    {
        Rule const r = Four();
        check(Valid(r), "four stages are valid");
        check(StageOf(r, 0) == 0, "fresh: the first stage");
        check(StageOf(r, 7199) == 0, "a second short of the second");
        check(StageOf(r, 7200) == 1, "two hours: the second");
        check(StageOf(r, 30000) == 2, "between six and fourteen hours: the third");
        check(StageOf(r, 50400) == 3 && StageOf(r, 4000000) == 3, "fourteen hours and on: the last");
        check(SecondsToNext(r, 0) == 7200, "two hours to the second");
        check(SecondsToNext(r, 7000) == 200, "the rest of the way");
        check(SecondsToNext(r, 21600) == 28800, "eight hours from six to fourteen");
        check(SecondsToNext(r, 50400) == 0 && SecondsToNext(r, 99999) == 0, "nothing after the last");
        check(Grew(r, 7199, 7200) && !Grew(r, 7200, 7300) && Grew(r, 0, 60000), "a stage crossed, or not");
        check(SecondsFor(r, 2) == 21600 && SecondsFor(r, 9) == 50400, "a stage's seconds; past the last, the last");
    }

    void TestInvalid()
    {
        Rule r = Four();
        r.stages[0].seconds = 5;
        check(!Valid(r), "the first stage must start at 0");
        r = Four();
        r.stages[2].seconds = 7200;
        check(!Valid(r), "seconds must rise");
        r = Four();
        r.stages[1].sharePermil = 0;
        check(!Valid(r), "a share of 0");
        r = Four();
        r.stages.clear();
        check(!Valid(r), "no stages");
        check(StageOf(r, 100) == 0 && SecondsToNext(r, 100) == 0 && near(SizeOf(r, 2.0f, 0), 2.0f), "no stages: full size");
    }

    void TestSizes()
    {
        Rule const r = Four();
        check(near(SizeOf(r, 2.0f, 0), 1.6f), "the first stage at 80 % of the kind's");
        check(near(SizeOf(r, 2.0f, 3), 2.0f) && near(SizeOf(r, 2.0f, 7), 2.0f), "the last at the kind's full size");
        // two beasts of one kind, one display twice the other's: the same size, so half the scale
        check(near(ScaleFor(r, 2.0f, 1.0f), 2.0f) && near(ScaleFor(r, 2.0f, 2.0f), 1.0f), "the scale is size over the display's");
        check(near(ScaleFor(r, 2.0f, 0.1f), r.scaleMax) && near(ScaleFor(r, 0.1f, 2.0f), r.scaleMin), "the scale within its limits");
        check(near(ScaleFor(r, 2.0f, 0.0f), 1.0f), "a display without a size: scale 1");
        check(near(ReachFor(r, 2.0f), 2.0f) && near(ReachFor(r, 0.5f), r.reachMin) && near(ReachFor(r, 9.0f), r.reachMax),
              "the reach follows the size within its limits");
        check(near(RadiusFor(r, 2.0f), 0.8f) && near(RadiusFor(r, 0.1f), r.radiusMin) && near(RadiusFor(r, 9.0f), r.radiusMax),
              "the radius follows the size within its limits");
    }
}

int main()
{
    TestStages();
    TestInvalid();
    TestSizes();
    std::cout << "petsize: " << g_checks - g_failed << "/" << g_checks << " checks passed\n";
    return g_failed ? 1 : 0;
}
