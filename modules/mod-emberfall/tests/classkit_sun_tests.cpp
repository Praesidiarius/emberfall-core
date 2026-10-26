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

// Standalone tests of the Sunwarden's arithmetic (src/classkit/EfClassKitSunLogic.h; tests/run_tests.sh builds and runs
// them): the overheal share, the pooled shields and their caps, restored charges, the counter, the mana on a hit, the
// low-health bonus, the periodic rider, the circle's raid cap and the combo's steps. The numbers are examples fed to
// the functions, not the kit's values (those: the game repo's content/data/classes/paladin.yaml and its tests).

#include "EfClassKitSunLogic.h"

#include <iostream>
#include <string>

using namespace Emberfall::ClassKit::SunLogic;

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
}

int main()
{
    // a share of the overheal
    check(OverhealShare(252, 52, 75) == 150, "200 overheal: 150");
    check(OverhealShare(252, 252, 75) == 0, "no overheal: nothing");
    check(OverhealShare(252, 0, 85) == 214, "all overheal at 85 %");
    check(OverhealShare(100, 140, 75) == 0, "more healed than the heal (never): nothing");

    // pooled shields: each share adds, up to a cap
    check(PooledAbsorb(0, 150, 300) == 150, "first share");
    check(PooledAbsorb(150, 200, 300) == 300, "capped at 30 %");
    check(PooledAbsorb(0, 50, 100) == 50 && PooledAbsorb(50, 50, 100) == 100 && PooledAbsorb(100, 50, 100) == 100,
          "a share an application, up to the cap");
    check(PooledAbsorb(20, 60, 100) == 80, "a worn shield is topped up");
    check(PooledAbsorb(120, 50, 100) == 120, "a cap that fell keeps what is held, adds nothing");

    // restored charges (stacks = charges recharging)
    check(RechargingAfterRestore(3, 0) == 0, "Salvation: all, recharge stopped");
    check(RechargingAfterRestore(2, 1) == 1, "Solar Reckoning: one back, the recharge runs on");
    check(RechargingAfterRestore(1, 1) == 0, "the last one back: full");
    check(RechargingAfterRestore(0, 1) == 0, "at full: lost");

    // a counter: fires every N, starting again after each
    CountStep s = Count(0, 3);
    check(!s.fire && s.count == 1, "first heal: 1");
    s = Count(1, 3);
    check(!s.fire && s.count == 2, "second: 2");
    s = Count(2, 3);
    check(s.fire && s.count == 0, "third: a charge, back to 0");
    s = Count(1, 2);
    check(s.fire && s.count == 0, "Renewed Devotion: the second fires");
    check(Count(0, 0).fire, "need 0 counts as 1");

    // mana on a hit: a percent of the pool, doubled on a crit with misc B
    check(ManaOnHit(2000, 15, false, true) == 300, "15 % of 2000");
    check(ManaOnHit(2000, 15, true, true) == 600, "a crit doubles");
    check(ManaOnHit(2000, 15, true, false) == 300, "no doubling without misc B");
    // playtest #67: of base mana (misc 1), whatever gear adds to the pool; max mana without it
    check(ManaPool(2400, 770, true) == 770, "misc 1: base mana");
    check(ManaPool(2400, 770, false) == 2400, "no misc: max mana");
    check(ManaPool(2400, 0, true) == 2400, "no base mana known: max mana");
    check(ManaOnHit(ManaPool(2400, 770, true), 25, true, true) == 385, "25 % of 770, doubled on a crit");

    // a bonus below a health threshold
    check(LowHealthBonus(425, 34.0f, 35, 20) == 510, "below 35 %: +20 %");
    check(LowHealthBonus(425, 35.0f, 35, 20) == 425, "at 35 %: nothing");
    check(LowHealthBonus(425, 10.0f, 0, 20) == 425, "no threshold: nothing");

    // a periodic rider: the whole split over the ticks
    check(PerTick(108, 6000, 2000) == 36, "108 in 3 ticks: 36");
    check(PerTick(2, 6000, 2000) == 1, "at least 1 a tick");
    check(PerTick(0, 6000, 2000) == 0, "nothing: nothing");
    check(PerTick(50, 1000, 2000) == 50, "fewer than one tick: all at once");

    // the circle: everyone in a party, the 6 most injured in a raid
    std::vector<float> hp = { 90, 20, 100, 50, 10, 70, 30, 80 };
    check(CircleTargets(hp, false, 6).size() == 8, "a party: all 8");
    std::vector<size_t> raid = CircleTargets(hp, true, 6);
    check(raid.size() == 6, "a raid: 6");
    check(raid == std::vector<size_t>({ 1, 3, 4, 5, 6, 7 }), "the most injured, in candidate order (100 % and 90 % left out)");
    check(CircleTargets({ 50, 40 }, true, 6).size() == 2, "fewer than the cap: all");

    // the combo: at 0 stacks the button itself, at 1 and 2 the next steps
    check(ComboStep(0, 3) == 0 && ComboStep(1, 3) == 1 && ComboStep(2, 3) == 2, "steps by stacks");
    check(ComboStep(4, 3) == 0, "past the steps: the button");

    std::cout << "classkit sun tests: " << (g_checks - g_failed) << "/" << g_checks << " passed\n";
    return g_failed ? 1 : 0;
}
