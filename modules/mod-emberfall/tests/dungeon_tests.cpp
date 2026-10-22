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

// Standalone tests of the dungeon logic that needs no core (src/dungeons/EfDungeonLogic.h; tests/run_tests.sh builds and
// runs them): the dev warden's gating (Emberfall docs/design/dungeon-barrows.md §8: with the test tools off it never
// stays in the world and offers nothing, whatever the player), its menu, the barrows' seals, and the boss fights' timers,
// thresholds and phases (src/dungeons/EfBarrowsBossLogic.h, §5a).

#include "EfBarrowsBossLogic.h"
#include "EfDungeonLogic.h"

#include <cmath>

#include <iostream>
#include <string>

using namespace Emberfall::Dungeons;
namespace B = Emberfall::Barrows;

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

    // The gating proof: tools off -> the warden leaves, no option for anybody anywhere, no option can be taken.
    void TestOffMeansNothing()
    {
        check(!WardenStays(false) && WardenStays(true), "the warden stays only with the test tools on");
        for (std::uint32_t map : { 0u, kBarrowsMap, 750u })
            for (int level : { 1, 10, 54, 55, 59, 60 })
                for (bool combat : { false, true })
                {
                    WardenAsk a;
                    a.testToolsOn = false;
                    a.map = map;
                    a.level = std::uint8_t(level);
                    a.inCombat = combat;
                    check(WardenOptions(a).empty(), "tools off: no options (map " + std::to_string(map) + ", level " +
                                                        std::to_string(level) + ")");
                    for (std::uint32_t o = 1; o <= 5; ++o)
                        check(!Offers(a, WardenOption(o)), "tools off: option " + std::to_string(o) + " cannot be taken");
                }
    }

    void TestMenu()
    {
        WardenAsk a;
        a.testToolsOn = true;
        a.map = 0;
        a.level = 12;
        auto opts = WardenOptions(a);
        check(opts.size() == 4 && opts[0] == WardenOption::ToBarrows && opts[1] == WardenOption::Level60 &&
                  opts[2] == WardenOption::ToDoor && opts[3] == WardenOption::ResetBarrows,
              "Millbrook at 12: barrows, level 60, the door, reset");
        a.level = 60;
        opts = WardenOptions(a);
        check(opts.size() == 3 && !Offers(a, WardenOption::Level60), "at 60: no level option");
        check(!Offers(a, WardenOption::ToMillbrook), "outside: no way back to Millbrook (you are there)");
        a.map = kBarrowsMap;
        opts = WardenOptions(a);
        check(opts.size() == 2 && opts[0] == WardenOption::ToMillbrook && opts[1] == WardenOption::ToDoor,
              "inside at 60: back to Millbrook, the door");
        check(!Offers(a, WardenOption::ToBarrows) && !Offers(a, WardenOption::ResetBarrows), "inside: no way in, no reset");
        a.level = 58;
        check(Offers(a, WardenOption::Level60), "inside at 58: level 60 offered");
        a.maxLevel = 58;
        check(!Offers(a, WardenOption::Level60), "a realm capped at 58: nothing above the cap");
        a.inCombat = true;
        check(WardenOptions(a).empty(), "in combat: nothing");
        for (std::uint32_t o = 1; o <= 5; ++o)
            check(std::string(WardenOptionText(WardenOption(o))).size() > 10, "option text " + std::to_string(o));
    }

    void TestSeals()
    {
        check(SealBit(DATA_SEAL_KEENING) == 1 && SealBit(DATA_SEAL_BROOD) == 2 && SealBit(DATA_SEAL_PYRE) == 4, "seal bits");
        check(SealBit(DATA_FIRST_INTERRED) == 0 && SealBit(DATA_POST_KEEPER) == 0 && SealBit(0) == 0, "no seal bit for others");
        std::uint32_t m = 0;
        SealResult r = BreakSeal(m, DATA_SEAL_BROOD, kEncounterDone);
        check(r.mask == 2 && !r.wakes, "first seal: no wake");
        r = BreakSeal(r.mask, DATA_SEAL_BROOD, kEncounterDone);
        check(r.mask == 2 && !r.wakes, "the same seal twice: nothing more");
        r = BreakSeal(r.mask, DATA_SEAL_PYRE, 1);
        check(r.mask == 2 && !r.wakes, "not DONE: no seal");
        r = BreakSeal(r.mask, DATA_SEAL_PYRE, kEncounterDone);
        check(r.mask == 6 && !r.wakes && SealsBroken(r.mask) == 2, "second seal");
        r = BreakSeal(r.mask, DATA_SEAL_KEENING, kEncounterDone);
        check(r.mask == 7 && r.wakes && AllSealsBroken(r.mask) && SealsBroken(r.mask) == 3, "third seal wakes the First Interred");
        r = BreakSeal(r.mask, DATA_SEAL_KEENING, kEncounterDone);
        check(r.mask == 7 && !r.wakes, "once awake, never woken again");
        // any order
        std::uint32_t const orders[6][3] = { { 1, 2, 3 }, { 1, 3, 2 }, { 2, 1, 3 }, { 2, 3, 1 }, { 3, 1, 2 }, { 3, 2, 1 } };
        for (auto const& o : orders)
        {
            std::uint32_t mask = 0;
            int wakes = 0;
            for (std::uint32_t t : o)
            {
                SealResult s = BreakSeal(mask, t, kEncounterDone);
                mask = s.mask;
                wakes += s.wakes ? 1 : 0;
            }
            check(mask == 7 && wakes == 1, "every order wakes it exactly once");
        }
    }

    void TestPlaces()
    {
        check(kBarrowsInside.map == kBarrowsMap && kBarrowsDoor.map == 0 && kMillbrook.map == 0, "the places' maps");
        check(kHonourGuard1.map == kBarrowsMap && kHonourGuard2.z == kHonourGuard3.z, "the honour guard on the hall floor");
    }

    // ---- the boss fights (src/dungeons/EfBarrowsBossLogic.h, dungeon-barrows.md §5a) ---------------------------------
    void TestBossTimers()
    {
        namespace T = B::Timers;
        // Every timer: a first time within the fight's opening, a range that is a range, and longer than any cast it
        // starts (a boss never queues the same mechanic on top of itself).
        struct Named
        {
            char const* name;
            B::Timer t;
            std::uint32_t castMs;
        };
        Named const all[] = {
            { "Tally Sweep", T::kTallySweep, 2500 }, { "Numbered", T::kNumbered, 0 }, { "Shoulder the Post", T::kShoulder, 0 },
            { "Keen", T::kKeen, 3000 }, { "Grave Chill", T::kGraveChill, 2000 }, { "Mourning Veil", T::kMourningVeil, 0 },
            { "the brood", T::kBrood, 0 }, { "Brood Bile", T::kBroodBile, 0 }, { "Pyre Ash", T::kPyreAsh, 1500 },
            { "Stoker's Brand", T::kBrand, 1500 }, { "Bank the Fires", T::kBank, 2000 }, { "Barrow Weight", T::kWeight, 0 },
            { "Prefect's Sentence", T::kSentence, 0 }, { "Kneel", T::kKneel, 3000 },
        };
        for (Named const& n : all)
        {
            check(n.t.first >= 2000 && n.t.first <= 20000, std::string(n.name) + ": first within 2-20 s");
            check(n.t.min <= n.t.max && n.t.min > n.castMs + 1000, std::string(n.name) + ": a range, longer than its cast");
        }
        // The run-out mark's fuse is the spell's 6 s; the next mark comes only after the last one went off.
        check(T::kNumberedFuse == 6000 && T::kNumbered.min > T::kNumberedFuse, "Numbered: one mark at a time");
        // The interruptible Keen is not more often than one interrupt's cooldown in a group of two kickers (12-18 s).
        check(T::kKeen.min >= 12000, "Keen: room for the kicks' cooldowns");
        // A heave every 2.5 s over a 2 s cast; six heaves in a burrow of 15 s.
        check(T::kHeaveEveryMs > 2000 && T::kBurrowMs / T::kHeaveEveryMs == 6, "the burrow's heaves");
        // The hard enrage after the soft one could be reached at a normal pace (the run's B5 took 129 s).
        check(T::kHardEnrageMs == 360000, "six minutes to Long Patience Ends");
    }

    void TestThresholds()
    {
        std::uint32_t fired = 0;
        check(B::CrossedThresholds(B::Thresholds::kCinders, 80.0f, fired).empty() && fired == 0, "nothing above 75 %");
        auto a = B::CrossedThresholds(B::Thresholds::kCinders, 74.9f, fired);
        check(a.size() == 1 && a[0] == 0 && fired == 1, "75 % once");
        check(B::CrossedThresholds(B::Thresholds::kCinders, 70.0f, fired).empty(), "75 % not twice");
        // One big hit through 50 and 25 fires both, in order.
        auto b = B::CrossedThresholds(B::Thresholds::kCinders, 20.0f, fired);
        check(b.size() == 2 && b[0] == 1 && b[1] == 2 && fired == 7, "a hit across two thresholds fires both");
        check(B::CrossedThresholds(B::Thresholds::kCinders, 0.0f, fired).empty(), "and never again");
        // Every list descends and stays inside the fight.
        auto descending = [](auto const& l) {
            for (std::size_t i = 1; i < l.size(); ++i)
                if (l[i] >= l[i - 1])
                    return false;
            return l.front() < 100 && l.back() > 0;
        };
        check(descending(B::Thresholds::kShades) && descending(B::Thresholds::kBurrow) && descending(B::Thresholds::kCinders) &&
                  descending(B::Thresholds::kWard), "the thresholds descend");
        // The First Interred's second ward ends before its enrage (two mechanics never start at once).
        check(B::Thresholds::kWard.back() > B::Thresholds::kEnrage + 10, "the ward and the enrage apart");
    }

    void TestBurrow()
    {
        B::BurrowState s;
        check(B::StepBurrow(s, 100, false) == B::BurrowAction::None, "above: nothing");
        B::StartBurrow(s);
        check(s.phase == B::BurrowPhase::Burrowed && s.msLeft == B::Timers::kBurrowMs, "burrowed for 15 s");
        int heaves = 0, emerge = 0, surface = 0;
        bool casting = false;
        std::uint32_t castLeft = 0, t = 0;
        for (; t < 30000 && !surface; t += 100)
        {
            if (castLeft)
                castLeft = castLeft > 100 ? castLeft - 100 : 0;
            casting = castLeft > 0;
            switch (B::StepBurrow(s, 100, casting))
            {
                case B::BurrowAction::Heave: ++heaves; castLeft = 2000; break;
                case B::BurrowAction::Emerge: ++emerge; castLeft = B::Timers::kEmergeCastMs; break;
                case B::BurrowAction::Surface: ++surface; break;
                default: break;
            }
        }
        check(heaves >= 5 && heaves <= 6, "five or six heaves under the earth: " + std::to_string(heaves));
        check(emerge == 1 && surface == 1 && s.phase == B::BurrowPhase::Above, "one Emerge, then above again");
        check(t >= B::Timers::kBurrowMs + B::Timers::kEmergeCastMs && t <= B::Timers::kBurrowMs + 4000, "back up after ~17 s: " + std::to_string(t));
        // A heave still being cast when the time is up: Emerge waits for it.
        B::StartBurrow(s);
        check(B::StepBurrow(s, B::Timers::kBurrowMs, true) == B::BurrowAction::None && B::StepBurrow(s, 0, false) == B::BurrowAction::Emerge,
              "Emerge after the last heave's cast");
    }

    void TestWard()
    {
        B::WardState w;
        check(!B::BearerDied(w), "no ward, nothing drops");
        B::RaiseWard(w, B::kBearersPerWard);
        check(w.up && w.bearersAlive == 2 && w.wards == 1, "two bearers hold it");
        check(!B::BearerDied(w) && w.up, "one bearer down: still up");
        check(B::BearerDied(w) && !w.up, "the second: it drops");
        check(!B::BearerDied(w), "and only once");
        B::RaiseWard(w, B::kBearersPerWard);
        check(w.up && w.wards == 2, "the second ward");
    }

    void TestRooms()
    {
        // The rooms' middles from layout.yaml's design coordinates, as the spawns (dungeon_spawns.yaml) have them.
        check(std::fabs(B::kWidowsBarrow.x - 313.33f) < 0.01f && std::fabs(B::kWidowsBarrow.y - 425.33f) < 0.01f, "the Widow's Barrow");
        check(std::fabs(B::kFirstBarrow.x - 463.33f) < 0.01f && std::fabs(B::kFirstBarrow.y - 268.33f) < 0.01f, "the First Barrow");
        auto ring = B::Ring(B::kFirstBarrow, B::kBearerRadius, 2, 0.0f);
        check(ring.size() == 2 && std::fabs(std::hypot(ring[0].x - ring[1].x, ring[0].y - ring[1].y) - 2 * B::kBearerRadius) < 0.01f,
              "two bearers on opposite sides");
        check(B::kBearerRadius < B::kFirstRadius - 3 && B::kShadeSpawnRadius < B::kWidowsRadius, "adds inside the rooms");
        // A shade crosses to her in a few seconds (not at once, not forever): 7 yd/s x the rate.
        float const secs = B::kShadeSpawnRadius / (7.0f * B::kShadeSpeedRate);
        check(secs > 6.0f && secs < 15.0f, "a shade's walk: " + std::to_string(secs) + " s");
        // The shades rise on the far side from her: wherever she is tanked in the barrow, 20 yd or more to walk.
        for (float dx : { -20.0f, 0.0f, 20.0f })
            for (auto const& p : B::FarthestOnRing(B::kWidowsBarrow, B::kShadeSpawnRadius, 8, B::kShadesPerWave,
                                                    B::Point{ B::kWidowsBarrow.x + dx, B::kWidowsBarrow.y - dx * 0.5f, 0 }, 0.3f))
                check(std::hypot(p.x - B::kWidowsBarrow.x - dx, p.y - B::kWidowsBarrow.y + dx * 0.5f) >= 20.0f, "a shade far from her");
        // A player outruns a cinder.
        check(7.0f * B::kCinderRunSpeed < 7.0f, "cinders are slower than a running player");
    }
}

int main()
{
    TestOffMeansNothing();
    TestMenu();
    TestSeals();
    TestPlaces();
    TestBossTimers();
    TestThresholds();
    TestBurrow();
    TestWard();
    TestRooms();
    std::cout << "dungeon tests: " << (g_checks - g_failed) << " of " << g_checks << " checks passed\n";
    return g_failed ? 1 : 0;
}
