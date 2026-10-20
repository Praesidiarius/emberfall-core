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

// Standalone tests of the GM command fixes' logic (src/gmcommands/EfGmCommandLogic.h; tests/run_tests.sh builds and runs
// them): which typed lines are `.modify speed ...` and `.die`, the move types per variant, the held rates.

#include "EfGmCommandLogic.h"

#include <array>
#include <iostream>
#include <string>

using namespace Emberfall::GmCommands;

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

    void TestParse()
    {
        Parsed p = Parse("modify speed 5");
        check(p.command == Command::Speed && p.kind == SpeedKind::All && p.rate == 5.0f, "modify speed 5: all, 5");
        p = Parse(".mod sp 2.5");
        check(p.command == Command::Speed && p.kind == SpeedKind::All && p.rate == 2.5f, ".mod sp 2.5: the core's prefixes");
        p = Parse("MODIFY  SPEED   walk 3");
        check(p.command == Command::Speed && p.kind == SpeedKind::Walk && p.rate == 3.0f, "case and extra blanks; walk");
        p = Parse("modify speed all 1");
        check(p.command == Command::Speed && p.kind == SpeedKind::All && p.rate == 1.0f, "speed all 1");
        check(Parse("modify speed b 2").kind == SpeedKind::BackWalk, "b: backwalk");
        check(Parse("modify speed swim 2").kind == SpeedKind::Swim, "swim");
        check(Parse("modify speed fly 2").kind == SpeedKind::Fly, "fly");
        check(Parse("modify speed abc").command == Command::None, "not a number: the stock handler answers");
        p = Parse("modify speed swim abc");
        check(p.command == Command::Speed && std::isnan(p.rate), "a variant, not a number: NAN (the stock handler answers)");
        check(Parse("modify speed").command == Command::None, "no value: the stock handler (usage)");
        check(Parse("modify speed walk").command == Command::None, "a variant without value: stock");
        check(Parse("modify speed 5 6").command == Command::None, "two numbers: stock");
        check(Parse("modify speed sideways 5").command == Command::None, "an unknown variant: stock");
        check(Parse("modify scale 5").command == Command::None, "another modify command");
        check(Parse("modify hp 5").command == Command::None, "modify hp");
        check(Parse("speed 5").command == Command::None, "not under modify");
        check(Parse("die").command == Command::Die, "die");
        check(Parse(".DIE").command == Command::Die, ".DIE");
        check(Parse("die now").command == Command::None, "die with an argument: stock");
        check(Parse("dismount").command == Command::None, "dismount is not die");
        check(Parse("").command == Command::None && Parse("   ").command == Command::None, "empty");
    }

    void TestBoundsAndTypes()
    {
        check(RateInBounds(0.1f) && RateInBounds(50.0f) && RateInBounds(5.0f), "0.1, 5 and 50 in bounds");
        check(!RateInBounds(0.05f) && !RateInBounds(51.0f) && !RateInBounds(NAN), "0.05, 51, NAN out");
        check(MoveTypesOf(SpeedKind::All) == std::vector<int>{ kMoveWalk, kMoveRun, kMoveSwim, kMoveFlight }, "all: walk, run, swim, flight");
        check(MoveTypesOf(SpeedKind::Walk) == std::vector<int>{ kMoveRun }, "walk: the run speed (as cs_modify)");
        check(MoveTypesOf(SpeedKind::BackWalk) == std::vector<int>{ kMoveRunBack }, "backwalk: run back");
        check(MoveTypesOf(SpeedKind::Swim) == std::vector<int>{ kMoveSwim }, "swim");
        check(MoveTypesOf(SpeedKind::Fly) == std::vector<int>{ kMoveFlight }, "fly: flight");
    }

    void TestRates()
    {
        Rates r;
        check(r.Empty(), "new: empty");
        for (int t : MoveTypesOf(SpeedKind::All))
            r.Set(t, 5.0f);
        check(!r.Empty() && r.Holds(kMoveRun) && r.Holds(kMoveWalk) && !r.Holds(kMoveRunBack), "speed 5: four types held");

        std::array<float, kMoveTypes> current{};
        current.fill(1.0f);
        current[kMoveWalk] = current[kMoveRun] = current[kMoveSwim] = current[kMoveFlight] = 5.0f;
        auto now = [&](int t) { return current[t]; };
        check(r.Drifted(now).empty(), "rates as held: nothing to set");
        current[kMoveRun] = 1.0f; // a speed aura's removal recalculated the run speed
        check(r.Drifted(now) == std::vector<int>{ kMoveRun }, "run recalculated: run set again");
        current[kMoveRun] = 1.6f, current[kMoveFlight] = 1.0f; // mounted
        check(r.Drifted(now) == (std::vector<int>{ kMoveRun, kMoveFlight }), "mounted: run and flight set again");

        r.Set(kMoveRun, 1.0f);
        check(!r.Holds(kMoveRun) && r.Holds(kMoveWalk), "run 1: run released, walk kept");
        for (int t : MoveTypesOf(SpeedKind::All))
            r.Set(t, 1.0f);
        check(r.Empty(), "speed 1: all released");
        check(r.Drifted(now).empty(), "released: nothing set again");
        r.Set(-1, 5.0f);
        r.Set(kMoveTypes, 5.0f);
        check(r.Empty(), "out-of-range types ignored");
    }
}

int main()
{
    TestParse();
    TestBoundsAndTypes();
    TestRates();
    std::cout << "gm command tests: " << (g_checks - g_failed) << "/" << g_checks << " passed\n";
    return g_failed ? 1 : 0;
}
