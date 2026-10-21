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

// Standalone tests of `.eftest zone`'s logic (src/testzone/EfTestZoneLogic.h; tests/run_tests.sh builds and runs them):
// finding a zone by text, the route per race, the quest plan (done, breadcrumbs, waiting), flight nodes, gear, purse.

#include "EfTestZoneLogic.h"

#include <iostream>
#include <string>

using namespace Emberfall::TestZone;

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

    constexpr uint32_t kHuman = 1, kYlvar = 12;

    std::vector<ZoneRow> Zones()
    {
        // as Emberfall's route on 2026-10-21, out of order on purpose
        std::vector<ZoneRow> z(6);
        z[0].key = "greymarch"; z[0].name = "Greymarch"; z[0].sort = 4; z[0].levelMin = 10; z[0].levelMax = 19;
        z[1].key = "elderwood"; z[1].name = "Elderwood"; z[1].sort = 1; z[1].races = RaceBit(kHuman);
        z[2].key = "gloamholt"; z[2].name = "Gloamholt"; z[2].sort = 2; z[2].races = RaceBit(kYlvar);
        z[3].key = "hightide-bastion"; z[3].name = "Hightide Bastion"; z[3].sort = 3; z[3].levelMin = 10;
        z[4].key = "kingsfall"; z[4].name = "Kingsfall"; z[4].sort = 5; z[4].levelMin = 20;
        z[5].key = "kingsfall-east"; z[5].name = "Kingsfall East"; z[5].sort = 6; z[5].levelMin = 26;
        return z;
    }

    std::string Keys(std::vector<ZoneRow> const& zones)
    {
        std::string out;
        for (ZoneRow const& z : zones)
            out += (out.empty() ? "" : ",") + z.key;
        return out;
    }

    void TestFind()
    {
        std::vector<ZoneRow> const zones = Zones();
        check(Find(zones, "greymarch") && Find(zones, "greymarch")->key == "greymarch", "the key");
        check(Find(zones, "GreyMarch") && Find(zones, "GreyMarch")->key == "greymarch", "any case");
        check(Find(zones, "hightide") && Find(zones, "hightide")->key == "hightide-bastion", "a key's start");
        check(Find(zones, "kingsfall") && Find(zones, "kingsfall")->key == "kingsfall", "an exact key over a longer one");
        std::string several;
        check(!Find(zones, "king", &several), "two zones: none");
        check(several == "kingsfall, kingsfall-east", "two zones: both named");
        check(!Find(zones, "redmere") && !Find(zones, ""), "no such zone");
    }

    void TestBefore()
    {
        std::vector<ZoneRow> const zones = Zones();
        check(Keys(Before(zones, kHuman, "greymarch")) == "elderwood,hightide-bastion", "a human before Greymarch");
        check(Keys(Before(zones, kYlvar, "greymarch")) == "gloamholt,hightide-bastion", "a ylf before Greymarch");
        check(Keys(Before(zones, kHuman, "kingsfall")) == "elderwood,hightide-bastion,greymarch", "a human before Kingsfall");
        check(Before(zones, kHuman, "elderwood").empty(), "a start zone: nothing before");
        check(Before(zones, kHuman, "gloamholt").empty(), "another race's start zone: nothing before");
        check(Before(zones, kHuman, "nowhere").empty(), "an unknown zone: nothing");
    }

    void TestPlan()
    {
        std::vector<ZoneRow> const zones = Zones();
        std::vector<QuestRow> quests = {
            {10, "elderwood", 2, "", RoleDone},
            {11, "elderwood", 1, "", RoleDone},
            {12, "elderwood", 3, "greymarch", RoleBreadcrumb},
            {13, "elderwood", 4, "greymarch", RoleAfter},
            {20, "greymarch", 1, "", RoleDone},
            {21, "greymarch", 2, "kingsfall", RoleBreadcrumb},
            {22, "greymarch", 3, "elderwood", RoleBreadcrumb},
            {30, "hightide-bastion", 1, "", RoleDone},
            {40, "gloamholt", 1, "hightide-bastion", RoleBreadcrumb},
        };
        QuestPlan p = PlanQuests(Before(zones, kHuman, "greymarch"), quests, "greymarch");
        check(p.done == std::vector<uint32_t>({11, 10, 30}), "Greymarch: Elderwood's and Hightide's done, route order");
        check(p.breadcrumbs == std::vector<uint32_t>({12}), "Greymarch: the breadcrumb in the log");
        check(p.waiting == std::vector<uint32_t>({13}), "Greymarch: the quest after it waits");

        p = PlanQuests(Before(zones, kHuman, "kingsfall"), quests, "kingsfall");
        check(p.done == std::vector<uint32_t>({11, 10, 12, 13, 30, 20, 22}), "Kingsfall: every earlier quest done");
        check(p.breadcrumbs == std::vector<uint32_t>({21}), "Kingsfall: Greymarch's breadcrumb");

        p = PlanQuests(Before(zones, kYlvar, "hightide-bastion"), quests, "hightide-bastion");
        check(p.done.empty() && p.breadcrumbs == std::vector<uint32_t>({40}), "a ylf at Hightide: the Yew Run in the log");

        p = PlanQuests(Before(zones, kHuman, "elderwood"), quests, "elderwood");
        check(p.done.empty() && p.breadcrumbs.empty() && p.waiting.empty(), "a start zone: no quest touched");
    }

    void TestNodes()
    {
        std::vector<ZoneRow> const zones = Zones();
        std::vector<TaxiRow> const taxi = {{"elderwood", 401}, {"gloamholt", 402}, {"hightide-bastion", 100},
                                           {"greymarch", 400}, {"kingsfall", 404}, {"kingsfall", 403}};
        check(Nodes(Before(zones, kHuman, "greymarch"), "greymarch", taxi) == std::vector<uint32_t>({401, 100, 400}),
              "a human at Greymarch: Millbrook, the Registry, Coldharrow");
        check(Nodes(Before(zones, kYlvar, "kingsfall"), "kingsfall", taxi) == std::vector<uint32_t>({402, 100, 400, 404, 403}),
              "a ylf at Kingsfall");
    }

    void TestGear()
    {
        std::vector<GearRow> const rows = {
            {8, 4, 100, 1}, {8, 4, 101, 6}, {8, 4, 102, 16}, {8, 15, 200, 4}, {8, 15, 201, 9}, {8, 15, 199, 9},
            {1, 4, 300, 3}, {8, 0, 400, 16},
        };
        auto g = PickGear(rows, 8, 10);
        check(g.size() == 2 && g[4] == 101 && g[15] == 199, "level 10: the highest at or below, the lower id on a tie");
        g = PickGear(rows, 8, 20);
        check(g[4] == 102 && g[0] == 400, "level 20: the next step and the head");
        check(PickGear(rows, 1, 2).empty(), "below every piece: nothing");
        check(PickGear(rows, 11, 60).empty(), "a class without rows: nothing");
    }

    void TestPurse()
    {
        check(Purse(10) == 10000 && Purse(20) == 40000 && Purse(1) == 100, "level squared silver");
    }
}

int main()
{
    TestFind();
    TestBefore();
    TestPlan();
    TestNodes();
    TestGear();
    TestPurse();
    std::cout << "test zone tests: " << (g_checks - g_failed) << "/" << g_checks << " passed\n";
    return g_failed ? 1 : 0;
}
