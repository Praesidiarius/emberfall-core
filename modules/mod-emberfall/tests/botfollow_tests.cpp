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

// Standalone tests of the bots' follow rule (mod-playerbots' src/Emberfall/EmberfallFollowLogic.h; tests/run_tests.sh
// builds and runs them): who is followed, the window, the share per side, the spread rest, the exemptions, the zones.

#include "EmberfallFollowLogic.h"

#include <iostream>
#include <map>
#include <string>

using namespace EmberfallFollow::Logic;

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

    // a small world like content/data/bots/zones.yaml: 10-level bands, Accord, Kindred and shared zones
    std::vector<Zone> World()
    {
        return {
            {5001, 1, 10, SIDE_ACCORD},  {5410, 1, 10, SIDE_KINDRED},
            {5110, 10, 20, SIDE_ACCORD}, {5440, 10, 20, SIDE_KINDRED},
            {5160, 20, 30, SIDE_BOTH},   {5170, 20, 30, SIDE_ACCORD}, {5480, 20, 30, SIDE_KINDRED},
            {5200, 30, 40, SIDE_ACCORD}, {5510, 30, 40, SIDE_KINDRED},
            {5130, 40, 50, SIDE_ACCORD}, {5530, 40, 50, SIDE_BOTH},
            {5250, 50, 60, SIDE_ACCORD}, {5560, 50, 60, SIDE_KINDRED},
        };
    }

    // a zone of the band that holds `level` for the side (the bots stand where they belong before the pass)
    uint32_t HomeZone(uint8_t level, uint8_t side)
    {
        for (Zone const& z : World())
            if (ZoneFits(z, level, side))
                return z.id;
        return 0;
    }

    // n bots of a side spread evenly over 1-60, guids from `first`
    void Spread(std::vector<Bot>& bots, uint32_t n, uint8_t side, uint32_t first)
    {
        for (uint32_t i = 0; i < n; ++i)
        {
            uint8_t const level = uint8_t(1 + (i * 60) / n);
            bots.push_back({first + i, side, level, HomeZone(level, side), false});
        }
    }

    struct After
    {
        uint32_t inWindow = 0, total = 0;
    };

    // the bots of a side after the plan's orders (a re-level's level; every moved bot counts as in a fitting zone)
    After Apply(std::vector<Bot> const& bots, Plan const& plan, uint8_t side)
    {
        std::map<uint32_t, Order> byGuid;
        for (Order const& o : plan.orders)
            byGuid[o.guid] = o;
        After a;
        for (Bot const& b : bots)
        {
            if (b.side != side)
                continue;
            ++a.total;
            auto it = byGuid.find(b.guid);
            uint8_t const level = it != byGuid.end() ? it->second.level : b.level;
            bool const fits = it != byGuid.end() || InFittingZone(b, World());
            if (plan.window.Contains(level) && fits)
                ++a.inWindow;
        }
        return a;
    }

    void TestPickFollowed()
    {
        std::vector<uint32_t> const accounts{7, 3};
        check(PickFollowed(accounts, {}) == -1, "nobody online: nobody followed");
        check(PickFollowed(accounts, {{9, 100, 22}}) == -1, "an account not in the list is not followed");
        check(PickFollowed(accounts, {{3, 100, 30}}) == 0, "the second listed account when it alone is online");
        check(PickFollowed(accounts, {{3, 100, 30}, {7, 200, 22}}) == 1, "two followed accounts online: the first listed wins");
        check(PickFollowed({0, 3}, {{3, 100, 30}}) == 0, "an unresolved name (account 0) is skipped");
        check(PickFollowed({}, {{3, 100, 30}}) == -1, "no account configured: nobody followed");
    }

    void TestWindow()
    {
        Window w = WindowFor(22, 3, 60);
        check(w.lo == 19 && w.hi == 25 && w.Size() == 7, "level 22 +/- 3: 19-25");
        check(w.Contains(19) && w.Contains(25) && !w.Contains(18) && !w.Contains(26), "the window's ends are inside");
        check(w.Distance(10) == 9 && w.Distance(60) == 35 && w.Distance(22) == 0, "distance to the window's nearest end");
        w = WindowFor(2, 3, 60);
        check(w.lo == 1 && w.hi == 5, "clamped at level 1");
        w = WindowFor(59, 3, 60);
        check(w.lo == 56 && w.hi == 60, "clamped at the cap 60");
        w = WindowFor(70, 3, 60);
        check(w.lo == 57 && w.hi == 60, "a level above the cap counts as the cap");
        w = WindowFor(22, 0, 60);
        check(w.lo == 22 && w.hi == 22, "window 0: the level itself");
    }

    void TestZones()
    {
        std::vector<Zone> const zones = World();
        check(ZoneFits(zones[4], 22, SIDE_ACCORD) && ZoneFits(zones[4], 22, SIDE_KINDRED), "a shared zone fits both sides");
        check(!ZoneFits(zones[5], 22, SIDE_KINDRED), "an Accord zone does not fit a Kindred bot");
        check(ZoneFits(zones[2], 20, SIDE_ACCORD) && ZoneFits(zones[5], 20, SIDE_ACCORD), "level 20 fits the 10-20 and the 20-30 band");
        check(ParseSide("accord") == SIDE_ACCORD && ParseSide("kindred") == SIDE_KINDRED && ParseSide("both") == SIDE_BOTH
                  && ParseSide("") == SIDE_BOTH, "the side words of botpilot conf");

        std::map<uint32_t, uint32_t> counts{{5160, 4}, {5170, 2}};
        check(PickZone(zones, counts, 22, SIDE_ACCORD, 0, {}) == 5170, "the fitting zone with the fewest bots");
        counts[5170] = 4;
        check(PickZone(zones, counts, 22, SIDE_ACCORD, 0, {}) == 5160, "ties: the first listed");
        check(PickZone(zones, counts, 22, SIDE_KINDRED, 0, {}) == 5480, "a Kindred bot: its zone and the shared one");
        check(PickZone(zones, counts, 22, SIDE_ACCORD, 4, {}) == 0, "every fitting zone at the cap: none");
        check(PickZone(zones, counts, 22, SIDE_ACCORD, 5, {5160}) == 5170, "a zone with no place found is skipped");
        check(PickZone(zones, counts, 61, SIDE_ACCORD, 0, {}) == 0, "no band holds the level: none");
    }

    void TestPlanShareAndSides()
    {
        // the brief's case: 150 bots, 75 % Accord, spread over 1-60, the followed character at 22
        std::vector<Bot> bots;
        Spread(bots, 113, SIDE_ACCORD, 1000);
        Spread(bots, 37, SIDE_KINDRED, 5000);
        Window const w = WindowFor(22, 3, 60);
        Plan const plan = MakePlan(bots, World(), w, 22, 90, 60);
        check(plan.accord.target == 102 && plan.kindred.target == 33, "90 % of each side, rounded (102 of 113, 33 of 37)");
        After const a = Apply(bots, plan, SIDE_ACCORD), k = Apply(bots, plan, SIDE_KINDRED);
        check(a.inWindow == 102, "the Accord's share ends in the window: " + std::to_string(a.inWindow));
        check(k.inWindow == 33, "the Kindred's share ends in the window: " + std::to_string(k.inWindow));
        check(a.total == 113 && k.total == 37, "no bot changes side: the ratio stays");
        uint32_t places = 0;
        for (Order const& o : plan.orders)
        {
            check(w.Contains(o.level), "every order ends in the window");
            if (o.kind == Move::Place)
                ++places;
        }
        // the spread: the in-window bots in a 10-20 zone at 19 and 20... HomeZone puts 20 in 10-20, which still fits
        check(places == 0, "bots spread to their bands' zones need no new place");
        // re-levels come furthest first
        uint8_t last = 255;
        bool ordered = true;
        for (Order const& o : plan.orders)
            if (o.kind == Move::Relevel)
            {
                ordered = ordered && o.distance <= last;
                last = o.distance;
            }
        check(ordered, "re-levels are carried out furthest from the window first");
        // the levels in the window come out even (each side: no level more than one above another)
        for (uint8_t side : {SIDE_ACCORD, SIDE_KINDRED})
        {
            std::map<uint8_t, uint32_t> at;
            std::map<uint32_t, Order> byGuid;
            for (Order const& o : plan.orders)
                byGuid[o.guid] = o;
            for (Bot const& b : bots)
                if (b.side == side)
                {
                    auto it = byGuid.find(b.guid);
                    uint8_t const l = it != byGuid.end() ? it->second.level : b.level;
                    if (w.Contains(l))
                        ++at[l];
                }
            uint32_t lo = 1000, hi = 0;
            for (int l = w.lo; l <= w.hi; ++l)
            {
                lo = std::min(lo, at[uint8_t(l)]);
                hi = std::max(hi, at[uint8_t(l)]);
            }
            check(hi - lo <= 1, "the window's levels are filled evenly (side " + std::to_string(side) + ")");
        }
    }

    void TestReserveStaysSpread()
    {
        std::vector<Bot> bots;
        Spread(bots, 100, SIDE_ACCORD, 1000);
        Window const w = WindowFor(22, 3, 60);
        Plan const plan = MakePlan(bots, World(), w, 22, 90, 60);
        std::set<uint32_t> moved;
        for (Order const& o : plan.orders)
            moved.insert(o.guid);
        uint32_t below = 0, above = 0, low = 1000, high = 0;
        for (Bot const& b : bots)
            if (!moved.count(b.guid) && !w.Contains(b.level))
            {
                (b.level < w.lo ? below : above)++;
                low = std::min<uint32_t>(low, b.level);
                high = std::max<uint32_t>(high, b.level);
            }
        check(below + above == plan.accord.reserve && plan.accord.reserve == 10, "10 bots stay outside the window");
        check(below >= 2 && above >= 6, "the rest stays spread below and above the window (" + std::to_string(below) +
                                             " below, " + std::to_string(above) + " above)");
        check(low <= 5 && high >= 55, "the rest reaches both ends of the world (" + std::to_string(low) + "-" +
                                          std::to_string(high) + ")");
        // a second pass two hours on: the followed character at 30; the rest is still spread
        std::vector<Bot> after = bots;
        std::map<uint32_t, Order> byGuid;
        for (Order const& o : plan.orders)
            byGuid[o.guid] = o;
        for (Bot& b : after)
            if (auto it = byGuid.find(b.guid); it != byGuid.end())
            {
                b.level = it->second.level;
                b.zone = HomeZone(b.level, b.side);
            }
        Window const w2 = WindowFor(30, 3, 60);
        Plan const plan2 = MakePlan(after, World(), w2, 30, 90, 60);
        After const a2 = Apply(after, plan2, SIDE_ACCORD);
        check(a2.inWindow == 90, "the second pass moves the population to 27-33: " + std::to_string(a2.inWindow));
        std::set<uint32_t> moved2;
        for (Order const& o : plan2.orders)
            moved2.insert(o.guid);
        low = 1000, high = 0;
        for (Bot const& b : after)
            if (!moved2.count(b.guid) && !w2.Contains(b.level))
            {
                low = std::min<uint32_t>(low, b.level);
                high = std::max<uint32_t>(high, b.level);
            }
        check(low <= 5 && high >= 55, "after the second pass the rest still reaches both ends (" + std::to_string(low) + "-" +
                                          std::to_string(high) + ")");
    }

    void TestLeftAloneAndPlaces()
    {
        Window const w = WindowFor(22, 3, 60);
        std::vector<Bot> bots;
        // 9 bots already in the window and in a fitting zone, 1 at level 50: share 90 % of 10 = 9: nothing to do
        for (uint32_t i = 0; i < 9; ++i)
            bots.push_back({100 + i, SIDE_ACCORD, uint8_t(20 + i % 6), 5170, false});
        bots.push_back({200, SIDE_ACCORD, 50, 5130, false});
        Plan plan = MakePlan(bots, World(), w, 22, 90, 60);
        check(plan.orders.empty() && plan.accord.fit == 9, "bots in the window and in a fitting zone are left alone");
        // one of them stands in a 1-10 zone: a place only, its level kept
        bots[0].zone = 5001;
        plan = MakePlan(bots, World(), w, 22, 90, 60);
        check(plan.orders.size() == 1 && plan.orders[0].guid == 100 && plan.orders[0].kind == Move::Place &&
                  plan.orders[0].level == bots[0].level, "an in-window bot in a wrong zone gets a new place only");
        // more than the share already fits: no bot is pushed out
        plan = MakePlan(bots, World(), w, 22, 50, 60);
        check(plan.orders.empty(), "more than the share fits already: nothing changes");
        // share 0: nothing
        plan = MakePlan(bots, World(), w, 22, 0, 60);
        check(plan.orders.empty(), "share 0: nothing changes");
    }

    void TestExemptions()
    {
        Exemptions e;
        check(!e.Any(), "a free bot is not exempt");
        for (int i = 0; i < 7; ++i)
        {
            Exemptions x;
            bool* flags[] = {&x.grouped, &x.combat, &x.dead, &x.seen, &x.dungeon, &x.vehicle, &x.travelling};
            *flags[i] = true;
            check(x.Any(), "each exemption keeps a bot out of the pass (" + std::to_string(i) + ")");
        }

        Window const w = WindowFor(22, 3, 60);
        std::vector<Bot> bots;
        Spread(bots, 20, SIDE_ACCORD, 1000);
        // the bot at level 58 is in a group with a player, the one at 55 fights: neither is ordered, both stay
        for (Bot& b : bots)
            if (b.level >= 55)
                b.exempt = true;
        Plan plan = MakePlan(bots, World(), w, 22, 90, 60);
        for (Order const& o : plan.orders)
            for (Bot const& b : bots)
                if (b.guid == o.guid)
                    check(!b.exempt, "an exempt bot gets no order");
        check(plan.accord.target == 18 && plan.accord.reserve == 2 && plan.accord.exempt == 2,
              "the exempt bots fill the reserve (they are taken next pass)");
        After const a = Apply(bots, plan, SIDE_ACCORD);
        check(a.inWindow == 18, "the share is still reached with the exempt bots kept out");
        // an exempt in-window bot in a wrong zone: the reserve shrinks by one so the share holds
        bots.clear();
        Spread(bots, 20, SIDE_ACCORD, 1000);
        bots.push_back({9000, SIDE_ACCORD, 22, 5001, true});
        plan = MakePlan(bots, World(), w, 22, 90, 60);
        After const b = Apply(bots, plan, SIDE_ACCORD);
        check(b.inWindow == plan.accord.target, "an exempt misplaced bot is made up for by one more re-level");
        // everyone exempt: no order at all
        for (Bot& x : bots)
            x.exempt = true;
        plan = MakePlan(bots, World(), w, 22, 90, 60);
        check(plan.orders.empty(), "every bot exempt: nothing changes");
    }

    void TestPace()
    {
        check(Due(0, 2) == 0 && Due(499, 2) == 0 && Due(500, 2) == 1 && Due(60000, 2) == 120, "two bots a second");
        check(Due(1000, 0) == 1, "pace 0 counts as one a second");
    }
}

int main()
{
    TestPickFollowed();
    TestWindow();
    TestZones();
    TestPlanShareAndSides();
    TestReserveStaysSpread();
    TestLeftAloneAndPlaces();
    TestExemptions();
    TestPace();
    if (g_failed)
    {
        std::cerr << "botfollow_tests: " << g_failed << " of " << g_checks << " checks failed\n";
        return 1;
    }
    std::cout << "botfollow_tests: " << g_checks << " checks passed\n";
    return 0;
}
