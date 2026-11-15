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

// Standalone tests of the Class Journeys' rules (tests/run_tests.sh builds and runs them): src/journeys/EfJourneyLogic.h,
// the steps owed to a bot and to a character from before the journey, the gates by rank, the tame credits, the places
// of a "use it there" objective and the stable's cycle. Made-up steps and spells: the numbers are the logic's.

#include "EfJourneyLogic.h"

#include <iostream>
#include <string>

using namespace Emberfall::Journeys;

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

    std::vector<Step> Steps()
    {
        return {
            {3, 1, 10, 1001, 0, kGateNone, kExistingTamed},
            {3, 2, 18, 1002, 0, kGateNone, kExistingNone},
            {3, 3, 24, 1003, 0, kGateElite, kExistingLevel},
            {3, 4, 32, 1004, 0, kGateNone, kExistingNone},
            {3, 5, 40, 1005, 0, kGateRare, kExistingLevel},
            {3, 6, 48, 1006, 0, kGateRareElite, kExistingLevel},
            {3, 7, 56, 1007, 1008, kGateNone, kExistingNone},
            {9, 1, 14, 2001, 0, kGateNone, kExistingNone},
        };
    }

    bool Has(std::vector<std::uint32_t> const& v, std::uint32_t x) { return std::find(v.begin(), v.end(), x) != v.end(); }

    void TestBots()
    {
        auto const steps = Steps();
        auto got = SpellsOwed(steps, {3, 30, true, false, false});
        check(got == std::vector<std::uint32_t>({1001, 1002, 1003}), "a bot of 30: every step at or below 30");
        got = SpellsOwed(steps, {3, 60, true, false, false});
        check(got.size() == 8 && Has(got, 1008) && !Has(got, 2001), "a bot of 60: all its class's steps and second spells");
        check(SpellsOwed(steps, {3, 9, true, false, false}).empty(), "a bot below the first step: none");
        check(SpellsOwed(steps, {9, 20, true, false, false}) == std::vector<std::uint32_t>({2001}), "another class's own");
    }

    void TestExisting()
    {
        auto const steps = Steps();
        check(SpellsOwed(steps, {3, 60, false, false, true}).empty(), "a player who is not migrating is owed nothing");
        auto got = SpellsOwed(steps, {3, 45, false, true, true});
        check(got == std::vector<std::uint32_t>({1001, 1003, 1005}), "an old hunter of 45 who tamed: steps 1, 3 and 5");
        got = SpellsOwed(steps, {3, 45, false, true, false});
        check(got == std::vector<std::uint32_t>({1003, 1005}), "an old hunter of 45 who never tamed: no step one");
        got = SpellsOwed(steps, {3, 60, false, true, true});
        check(got == std::vector<std::uint32_t>({1001, 1003, 1005, 1006}), "never the quests' own steps (2, 4, 7)");
        check(SpellsOwed(steps, {3, 5, false, true, false}).empty(), "a new character: nothing");
    }

    void TestGates()
    {
        auto const steps = Steps();
        check(GateOfRank(0) == kGateNone && GateOfRank(1) == kGateElite && GateOfRank(2) == kGateRareElite
              && GateOfRank(3) == kGateElite && GateOfRank(4) == kGateRare, "ranks to gates (boss as elite)");
        check(GateStep(steps, 3, kGateElite) && GateStep(steps, 3, kGateElite)->level == 24, "elites: step 3");
        check(GateStep(steps, 3, kGateRareElite)->level == 48, "rare elites: step 6");
        check(!GateStep(steps, 3, kGateNone), "a normal beast has no gate");
        check(!GateStep(steps, 9, kGateElite), "a class without gates");
    }

    void TestCredits()
    {
        std::vector<TameCredit> const c = {{11, kRankElite | kRankRareElite, 0}, {12, kRankRare | kRankRareElite, 0},
                                           {13, kRankRareElite, 0}, {13, kRankElite | kRankRare, 45}};
        check(TameCredits(c, 0, 50).empty(), "a normal beast: none of these");
        check(TameCredits(c, 1, 30) == std::vector<std::uint32_t>({11}), "an elite of 30: the elite's");
        check(TameCredits(c, 1, 46) == std::vector<std::uint32_t>({11, 13}), "an elite of 46: and an old beast's");
        check(TameCredits(c, 4, 40) == std::vector<std::uint32_t>({12}), "a rare of 40: the rare's");
        check(TameCredits(c, 2, 40) == std::vector<std::uint32_t>({11, 12, 13}), "a rare elite: all three, once each");
    }

    void TestCasts()
    {
        Cast const c{900, 77, 5201, 41};
        check(CastCounts(c, 77, 5200, 5201), "the area");
        check(CastCounts({900, 77, 5200, 41}, 77, 5200, 5204), "the zone");
        check(!CastCounts(c, 77, 5200, 5204), "another area");
        check(!CastCounts(c, 78, 5200, 5201), "another spell");
    }

    void TestStalls()
    {
        check(NextStall({}, -1) == -1, "no stalls");
        check(NextStall({false, false, false, false}, -1) == -1, "an empty stable");
        check(NextStall({false, true, false, true}, -1) == 1, "the first used stall");
        check(NextStall({false, true, false, true}, 1) == 3, "then the next");
        check(NextStall({false, true, false, true}, 3) == 1, "round the stable");
        check(NextStall({true, false, false, false}, 0) == 0, "one beast: itself again");
    }
}

int main()
{
    TestBots();
    TestExisting();
    TestGates();
    TestCredits();
    TestCasts();
    TestStalls();
    std::cout << "journeys: " << g_checks - g_failed << "/" << g_checks << " checks passed\n";
    return g_failed ? 1 : 0;
}
