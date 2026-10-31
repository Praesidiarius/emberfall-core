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
// thresholds and phases (src/dungeons/EfBarrowsBossLogic.h, §5a) and the boss toolkit (src/dungeons/EfBossToolkit.h, §5b).

#include "EfBarrowsBossLogic.h"
#include "EfDungeonLogic.h"

#include <array>
#include <cmath>

#include <iostream>
#include <string>

using namespace Emberfall::Dungeons;
namespace B = Emberfall::Barrows;
namespace Bosses = Emberfall::Bosses;

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

    // ---- the boss fights (src/dungeons/EfBarrowsBossLogic.h, dungeon-barrows.md §5a, v2 of 2026-10-31) ---------------
    void TestBossTimers()
    {
        namespace T = B::Timers;
        // Every timer: a first time within the fight's (or its phase's) opening, a range that is a range, and longer
        // than any cast it starts (a boss never queues the same mechanic on top of itself).
        struct Named
        {
            char const* name;
            B::Timer t;
            std::uint32_t castMs;
        };
        Named const all[] = {
            { "Tally Sweep", T::kTallySweep, 2500 }, { "Numbered", T::kNumbered, 0 }, { "Shoulder the Post", T::kShoulder, 0 },
            { "Keen", T::kKeen, 3000 }, { "Grave Chill", T::kGraveChill, 2000 }, { "Mourning Veil", T::kMourningVeil, 0 },
            { "the wake's veil", T::kWakeVeil, 0 }, { "Lament", T::kLament, T::kLamentCastMs },
            { "the chorus", T::kChorus, T::kChorusCasts * (T::kChorusCastMs + T::kChorusGapMs) },
            { "the brood", T::kBrood, 0 }, { "the frenzied brood", T::kBroodFrenzied, 0 }, { "Brood Bile", T::kBroodBile, 0 },
            { "the clutch", T::kClutch, T::kClutchFuseMs }, { "the frenzied clutch", T::kClutchFrenzied, T::kClutchFuseMs },
            { "Pyre Ash", T::kPyreAsh, 1500 }, { "Stoker's Brand", T::kBrand, 1500 }, { "Bank the Fires", T::kBank, 2000 },
            { "Barrow Weight", T::kWeight, 0 }, { "Prefect's Sentence", T::kSentence, 0 }, { "Kneel", T::kKneel, 3000 },
            { "the tithe", T::kTithe, T::kTitheFuseMs },
        };
        for (Named const& n : all)
        {
            check(n.t.first >= 2000 && n.t.first <= 20000, std::string(n.name) + ": first within 2-20 s");
            check(Bosses::TimerFits(n.t, n.castMs), std::string(n.name) + ": a range, longer than its cast");
        }
        // The run-out mark's fuse is the spell's 6 s; the next mark comes only after the last one went off.
        check(T::kNumberedFuse == 6000 && T::kNumbered.min > T::kNumberedFuse, "Numbered: one mark at a time");
        check(B::NumberedPerMark(B::PHASE_ONE) == 1 && B::NumberedPerMark(B::PHASE_TWO) == 2, "two numbered in the recount");
        // The posts land as the sweep does (its 2.5 s cast), never before.
        check(T::kPostsAfterSweepMs >= 2500 && T::kPostsAfterSweepMs < 4000, "the posts as the sweep lands");
        // The interruptible Keen is not more often than one interrupt's cooldown in a group of two kickers (12-18 s).
        check(T::kKeen.min >= 12000, "Keen: room for the kicks' cooldowns");
        // The chorus needs three different kicks: three casts, each long enough to be kicked after a reaction (0.5 s);
        // the next chorus after every kick's cooldown (18 s at most) is back.
        check(T::kChorusCasts == 3 && T::kChorusCastMs >= 1500 && T::kChorus.min >= 18000, "the chorus: an interrupt rotation");
        // The mourners lament one after another (not at once), each often enough to matter, rarely enough to kick.
        check(T::kLamentStaggerMs * (B::kMournersPerWake - 1) < T::kLament.min && T::kLament.min > T::kLamentCastMs + 2000,
              "the laments staggered");
        // A heave every 3.5 s over a 2 s cast; five in a burrow of 18 s; the furrow pulses every second.
        check(T::kHeaveEveryMs > 2000 && T::kBurrowMs / T::kHeaveEveryMs == 5, "the burrow's heaves");
        // The hard enrage well after the fight's paper time (~4.5 min, §5a) and its soft enrage.
        check(T::kHardEnrageMs == 480000, "eight minutes to Long Patience Ends");
        // The arena's edge pulses every second; the tithe's fuse and the shelters give time to walk 10 yd and more.
        check(T::kArenaPulseMs == 1000 && T::kTitheFuseMs >= 5000 && T::kHearthLifeMs > T::kLastPyreCastMs, "pulse, tithe, hearths");
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
        auto descending = [](auto const& l) {
            for (std::size_t i = 1; i < l.size(); ++i)
                if (l[i] >= l[i - 1])
                    return false;
            return l.front() < 100 && l.back() > 0;
        };
        check(descending(B::Thresholds::kBurrow) && descending(B::Thresholds::kCinders), "the thresholds descend");
        // The phases' order: B2's shades come after its wake; B5's ward, then the fall, the enrage, the last shrink.
        check(B::Thresholds::kShades[0] < B::Thresholds::kWake[0], "B2: the shades in phase 2");
        check(B::Thresholds::kWard[0] > B::Thresholds::kMoundFalls[0] + 20 && B::Thresholds::kMoundFalls[0] > B::Thresholds::kEnrage + 10 &&
                  B::Thresholds::kEnrage > B::Thresholds::kCloses[0] + 5, "B5: ward 70, fall 40, enrage 25, closes 15");
        check(B::InterredPhaseAfter(B::PHASE_ONE) == B::PHASE_TWO && B::InterredPhaseAfter(B::PHASE_TWO) == B::PHASE_THREE,
              "B5: the ward leads to phase 2, the fall to 3");
    }

    // The toolkit's phases by health (EfBossToolkit.h PhaseFor / NextPhase): never back, a big hit skips ahead.
    void TestPhases()
    {
        constexpr std::array<std::uint8_t, 2> plan{ 70, 40 };
        check(Bosses::PhaseFor(plan, 100.0f) == 1 && Bosses::PhaseFor(plan, 70.0f) == 2 && Bosses::PhaseFor(plan, 39.0f) == 3, "phase for health");
        check(Bosses::NextPhase(plan, 1, 80.0f) == 0 && Bosses::NextPhase(plan, 1, 69.0f) == 2, "phase 1 to 2 at 70 %");
        check(Bosses::NextPhase(plan, 2, 75.0f) == 0, "a heal above 70 % does not go back");
        check(Bosses::NextPhase(plan, 1, 30.0f) == 3, "one big hit from 1 to 3");
        check(Bosses::NextPhase(plan, 3, 10.0f) == 0, "no phase after the last");
    }

    void TestIntermission()
    {
        Bosses::Intermission w;
        check(!Bosses::IntermissionAddDied(w) && !Bosses::StepIntermission(w, 1000), "none: nothing ends");
        // Held by adds (the ward's two bearers, the wake's three mourners), with a safety net.
        Bosses::StartIntermission(w, B::Timers::kWakeLimitMs, B::kMournersPerWake);
        check(w.active && w.addsAlive == 3 && !w.timed, "three mourners hold the wake");
        check(!Bosses::IntermissionAddDied(w) && !Bosses::IntermissionAddDied(w) && w.active, "two down: still on");
        check(Bosses::IntermissionAddDied(w) && !w.active, "the third: it ends");
        check(!Bosses::IntermissionAddDied(w), "and only once");
        Bosses::StartIntermission(w, B::Timers::kWakeLimitMs, 3);
        check(!Bosses::StepIntermission(w, B::Timers::kWakeLimitMs - 1) && Bosses::StepIntermission(w, 1) && !w.active,
              "the safety net ends it at its limit");
        // No limit: only the adds end it (the Grave Ward).
        Bosses::StartIntermission(w, 0, B::kBearersPerWard);
        check(!Bosses::StepIntermission(w, 3600000) && w.active, "the ward has no time limit");
        check(!Bosses::IntermissionAddDied(w) && Bosses::IntermissionAddDied(w), "its second bearer ends it");
        // Timed (the mound falls): the time, not adds.
        Bosses::StartIntermission(w, B::Timers::kMoundFallsMs, 0);
        check(w.timed && !Bosses::IntermissionAddDied(w), "a timed one ignores deaths");
        int ends = 0;
        for (std::uint32_t t = 0; t < 10000; t += 100)
            ends += Bosses::StepIntermission(w, 100) ? 1 : 0;
        check(ends == 1 && !w.active, "a timed intermission ends once");
    }

    void TestEnergy()
    {
        Bosses::EnergyBar e;
        e.perSecond = B::kHeatPerSecond;
        check(std::fabs(Bosses::SecondsToFull(e) - 50.0f) < 0.01f, "the pyre: 50 s from cold to the Last Pyre");
        int full = 0;
        std::uint32_t t = 0;
        for (; t < 120000 && !full; t += 100)
            full += Bosses::StepEnergy(e, 100) ? 1 : 0;
        check(full == 1 && t == 50000 && Bosses::Energy(e) == 100, "full after 50 s: " + std::to_string(t));
        check(!Bosses::StepEnergy(e, 100) && Bosses::Energy(e) == 100, "it stays full until the ultimate");
        Bosses::ResetEnergy(e);
        check(Bosses::Energy(e) == 0, "the ultimate empties it");
        e.paused = true;
        check(!Bosses::StepEnergy(e, 60000) && Bosses::Energy(e) == 0, "paused while the pyre is cast");
        e.paused = false;
        e.perSecond = B::kHeatPerSecondBanked;
        check(Bosses::SecondsToFull(e) < 35.0f && Bosses::SecondsToFull(e) > 30.0f, "banked: ~33 s");
        Bosses::StepEnergy(e, 30000);
        check(Bosses::Energy(e) == 90 && Bosses::AddEnergy(e, B::kHeatPerBurst) && Bosses::Energy(e) == 100, "a cinder's burst fills it");
        check(!Bosses::AddEnergy(e, 10), "no more than full");
        // A slow bar loses nothing between short updates (thousandths).
        Bosses::EnergyBar slow;
        slow.perSecond = 1;
        for (int i = 0; i < 1000; ++i)
            Bosses::StepEnergy(slow, 33);
        check(Bosses::Energy(slow) == 33, "33 ms steps add up");
    }

    void TestChain()
    {
        Bosses::CastChain c;
        check(!Bosses::StepChain(c, 100, false), "no chain: nothing");
        Bosses::StartChain(c, B::Timers::kChorusCasts, B::Timers::kChorusGapMs);
        int starts = 0;
        std::uint32_t castLeft = 0, firstAt = 0, lastAt = 0;
        for (std::uint32_t t = 0; t < 20000; t += 100)
        {
            castLeft = castLeft > 100 ? castLeft - 100 : 0;
            if (Bosses::StepChain(c, 100, castLeft > 0))
            {
                if (!starts)
                    firstAt = t;
                lastAt = t;
                ++starts;
                castLeft = B::Timers::kChorusCastMs;
            }
        }
        check(starts == 3 && c.left == 0, "three keens");
        // Each after the last one's cast and the gap: about 3 x 2 s + 2 x 0.7 s.
        check(firstAt == 0 && lastAt >= 2 * (B::Timers::kChorusCastMs + B::Timers::kChorusGapMs) - 200 &&
                  lastAt <= 2 * (B::Timers::kChorusCastMs + B::Timers::kChorusGapMs) + 400, "the keens one after another: " + std::to_string(lastAt));
        // A kicked keen ends early: the next one comes the gap after the kick.
        Bosses::StartChain(c, 2, 700);
        check(Bosses::StepChain(c, 100, false), "the first at once");
        check(!Bosses::StepChain(c, 500, true), "casting: wait");
        check(!Bosses::StepChain(c, 600, false) && Bosses::StepChain(c, 100, false), "kicked: the gap, then the next");
    }

    void TestSoak()
    {
        // The clutch: one soaker takes it all; two share it; nobody: it hatches.
        Bosses::SoakOutcome o = Bosses::ResolveSoak(1, 1, B::kClutchDamage, B::kClutchFloor);
        check(!o.failed && o.perSoaker == 600, "one stamps the clutch out: 600");
        o = Bosses::ResolveSoak(2, 1, B::kClutchDamage, B::kClutchFloor);
        check(!o.failed && o.perSoaker == 300, "two share it: 300 each");
        check(Bosses::ResolveSoak(0, 1, B::kClutchDamage, B::kClutchFloor).failed, "nobody: it hatches");
        // The tithe: two needed; a third and a fourth lower each one's share to the floor, never below.
        check(Bosses::ResolveSoak(1, B::kTitheNeeded, B::kTitheDamage, B::kTitheFloor).failed, "one alone does not pay the tithe");
        check(Bosses::ResolveSoak(2, B::kTitheNeeded, B::kTitheDamage, B::kTitheFloor).perSoaker == 700, "two pay 700 each");
        check(Bosses::ResolveSoak(4, B::kTitheNeeded, B::kTitheDamage, B::kTitheFloor).perSoaker == 450, "four: the floor");
        // A share never takes a quarter of a cloth wearer's health or more alone than a mechanic stood in (§5a: ~2.6k tank).
        check(B::kClutchDamage <= 700 && B::kTitheDamage / B::kTitheNeeded <= 700, "a soak is a quarter of a cloth wearer's health at most");
        // Needed 0 is a shelter, resolved by its boss (InAnyShelter).
        std::vector<Bosses::Point2> const hearths = Bosses::RingPoints(B::Flat(B::kPyreBarrow), B::kHearthRing, B::kHearths, 0.0f);
        check(hearths.size() == 3 && Bosses::InAnyShelter(hearths[1], hearths, B::kHearthRadius), "in a hearth");
        check(!Bosses::InAnyShelter(B::Flat(B::kPyreBarrow), hearths, B::kHearthRadius), "the barrow's middle is no hearth");
        // Each hearth is inside the room and apart from the others (three places to go, not one crowd).
        for (Bosses::Point2 h : hearths)
            check(std::hypot(h.x - B::kPyreBarrow.x, h.y - B::kPyreBarrow.y) + B::kHearthRadius < B::kPyreRadius, "a hearth inside the barrow");
        check(std::hypot(hearths[0].x - hearths[1].x, hearths[0].y - hearths[1].y) > 4 * B::kHearthRadius, "the hearths apart");
    }

    void TestArena()
    {
        Bosses::Point2 const c = B::Flat(B::kFirstBarrow);
        check(Bosses::ArenaRadius(B::kFirstRadius, B::kArenaStages, 0) == B::kFirstRadius, "stage 0: the whole barrow");
        check(Bosses::ArenaRadius(B::kFirstRadius, B::kArenaStages, 1) == 20.0f && Bosses::ArenaRadius(B::kFirstRadius, B::kArenaStages, 2) == 15.0f,
              "20 yd at 40 %, 15 yd at 15 %");
        check(Bosses::ArenaRadius(B::kFirstRadius, B::kArenaStages, 9) == 15.0f, "never smaller than the last stage");
        check(!Bosses::OutsideArena(Bosses::Point2{ c.x + 19.0f, c.y }, c, 20.0f, 0.5f) && Bosses::OutsideArena(Bosses::Point2{ c.x + 21.0f, c.y }, c, 20.0f, 0.5f),
              "the ring's edge");
        // Kneel (10 yd round it) can still be escaped inside the last ring with the boss in its middle.
        check(B::kArenaStages[1] > 10.0f + 3.0f, "room to run out of Kneel in the last ring");
        // The tithe sits inside the ring, 10 yd from the boss.
        B::Point const boss{ B::kFirstBarrow.x + 5.0f, B::kFirstBarrow.y, 0 };
        B::Point const who{ B::kFirstBarrow.x + 40.0f, B::kFirstBarrow.y, 0 };
        B::Point const t = B::IntoRoom(B::Towards(boss, who, B::kTitheFromBoss), B::kFirstBarrow, B::kArenaStages[1], B::kTitheRadius);
        check(std::hypot(t.x - B::kFirstBarrow.x, t.y - B::kFirstBarrow.y) + B::kTitheRadius <= B::kArenaStages[1] + 0.01f, "the tithe inside the last ring");
        check(std::fabs(B::Towards(boss, boss, 5.0f).x - boss.x) < 0.01f, "towards itself: stays");
    }

    void TestBurrow()
    {
        B::BurrowState s;
        check(B::StepBurrow(s, 100, false).action == B::BurrowAction::None, "above: nothing");
        B::StartBurrow(s);
        check(s.phase == B::BurrowPhase::Burrowed && s.msLeft == B::Timers::kBurrowMs && s.burrows == 1, "burrowed for 18 s");
        int heaves = 0, emerge = 0, surface = 0, churns = 0, retargets = 0;
        std::uint32_t castLeft = 0, t = 0;
        for (; t < 40000 && !surface; t += 100)
        {
            castLeft = castLeft > 100 ? castLeft - 100 : 0;
            B::BurrowTick const k = B::StepBurrow(s, 100, castLeft > 0);
            churns += k.churn ? 1 : 0;
            retargets += k.retarget ? 1 : 0;
            switch (k.action)
            {
                case B::BurrowAction::Heave: ++heaves; castLeft = 2000; break;
                case B::BurrowAction::Emerge: ++emerge; castLeft = B::Timers::kEmergeCastMs; break;
                case B::BurrowAction::Surface: ++surface; break;
                default: break;
            }
        }
        check(heaves >= 4 && heaves <= 5, "four or five heaves under the earth: " + std::to_string(heaves));
        check(churns >= 17 && churns <= 18, "the furrow pulses every second: " + std::to_string(churns));
        check(retargets == 3, "a prey at once, then every 6 s: " + std::to_string(retargets));
        check(emerge == 1 && surface == 1 && s.phase == B::BurrowPhase::Above, "one Emerge, then above again");
        check(t >= B::Timers::kBurrowMs + B::Timers::kEmergeCastMs && t <= B::Timers::kBurrowMs + 4000, "back up after ~20 s: " + std::to_string(t));
        // A heave still being cast when the time is up: Emerge waits for it.
        B::StartBurrow(s);
        check(s.burrows == 2, "the second burrow");
        check(B::StepBurrow(s, B::Timers::kBurrowMs, true).action == B::BurrowAction::None &&
                  B::StepBurrow(s, 0, false).action == B::BurrowAction::Emerge, "Emerge after the last heave's cast");
        // A walking player (7 yd/s x 0.5 at least) keeps ahead of the furrow.
        check(7.0f * B::kFurrowSpeedRate < 3.5f, "the furrow is slower than a walk");
    }

    void TestRooms()
    {
        // The rooms' middles from layout.yaml's design coordinates, as the spawns (dungeon_spawns.yaml) have them.
        check(std::fabs(B::kWidowsBarrow.x - 313.33f) < 0.01f && std::fabs(B::kWidowsBarrow.y - 425.33f) < 0.01f, "the Widow's Barrow");
        check(std::fabs(B::kFirstBarrow.x - 463.33f) < 0.01f && std::fabs(B::kFirstBarrow.y - 268.33f) < 0.01f, "the First Barrow");
        auto ring = B::Ring(B::kFirstBarrow, B::kBearerRadius, 2, 0.0f);
        check(ring.size() == 2 && std::fabs(std::hypot(ring[0].x - ring[1].x, ring[0].y - ring[1].y) - 2 * B::kBearerRadius) < 0.01f,
              "two bearers on opposite sides");
        check(B::kBearerRadius < B::kFirstRadius - 3 && B::kShadeSpawnRadius < B::kWidowsRadius && B::kMournerRadius < B::kWidowsRadius - 3,
              "adds inside the rooms");
        // A shade crosses to her in a few seconds (not at once, not forever): 7 yd/s x the rate.
        float const secs = B::kShadeSpawnRadius / (7.0f * B::kShadeSpeedRate);
        check(secs > 6.0f && secs < 15.0f, "a shade's walk: " + std::to_string(secs) + " s");
        // The shades rise on the far side from her: wherever she is tanked in the barrow, 20 yd or more to walk.
        for (float dx : { -20.0f, 0.0f, 20.0f })
            for (auto const& p : B::FarthestOnRing(B::kWidowsBarrow, B::kShadeSpawnRadius, 8, B::kShadesPerWave,
                                                    B::Point{ B::kWidowsBarrow.x + dx, B::kWidowsBarrow.y - dx * 0.5f, 0 }, 0.3f))
                check(std::hypot(p.x - B::kWidowsBarrow.x - dx, p.y - B::kWidowsBarrow.y + dx * 0.5f) >= 20.0f, "a shade far from her");
        // The three mourners stand apart round her: one lament at a time can be kicked from the middle (30 yd range).
        auto mourners = B::Ring(B::kWidowsBarrow, B::kMournerRadius, B::kMournersPerWake, 0.0f);
        check(mourners.size() == 3 && std::hypot(mourners[0].x - mourners[1].x, mourners[0].y - mourners[1].y) > 30.0f &&
                  B::kMournerRadius < 30.0f, "the mourners apart, each within a kick's reach of the middle");
        // A clutch laid towards the middle stays inside the Brood Barrow.
        B::Point const wall{ B::kBroodBarrow.x + 29.0f, B::kBroodBarrow.y, B::kBroodBarrow.z };
        B::Point const clutch = B::IntoRoom(B::Towards(wall, B::kBroodBarrow, 6.0f), B::kBroodBarrow, B::kBroodRadius, 4.0f);
        check(std::hypot(clutch.x - B::kBroodBarrow.x, clutch.y - B::kBroodBarrow.y) <= B::kBroodRadius - 4.0f + 0.01f, "the clutch inside");
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
    TestPhases();
    TestIntermission();
    TestEnergy();
    TestChain();
    TestSoak();
    TestArena();
    TestBurrow();
    TestRooms();
    std::cout << "dungeon tests: " << (g_checks - g_failed) << " of " << g_checks << " checks passed\n";
    return g_failed ? 1 : 0;
}
