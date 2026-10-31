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

// The Barrows of Cairnmoor's boss fights without a core (Emberfall docs/design/dungeon-barrows.md §5a, the v2 fights of
// 2026-10-31 [proposal]): the spell, creature and text IDs the boss scripts use (content/data/spells/barrows.yaml,
// content/data/barrows/creatures.yaml), every timer, the health thresholds and phases, the soaks and the arena, and the
// places adds come from. emberfall_barrows_bosses.cpp runs the fights from it (BossAI, EventMap phases, SummonList, the
// toolkit of EfBossKit.h); tests/dungeon_tests.cpp checks the timers and the phase logic. The generic pieces (thresholds,
// phases, intermissions, the energy bar, cast chains, soaks, the arena) are EfBossToolkit.h.

#ifndef EMBERFALL_BARROWS_BOSS_LOGIC_H
#define EMBERFALL_BARROWS_BOSS_LOGIC_H

#include "EfBossToolkit.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Emberfall::Barrows
{
    using Emberfall::Bosses::CrossedThresholds;
    using Timer = Emberfall::Bosses::Timer;

    // --- IDs (content/data/spells/barrows.yaml) ------------------------------------------------------------------------
    enum Spells : std::uint32_t
    {
        // B1 the Post-Keeper
        SPELL_NUMBERED = 97001,             // the run-out mark (6 s)
        SPELL_SHOULDER_THE_POST = 97002,    // knock-back on the victim
        SPELL_TALLY_SWEEP = 97021,          // frontal cone 120 degrees, 10 yd, 2.5 s
        SPELL_STRUCK_FROM_THE_COUNT = 97022, // 8 yd round the numbered one when the mark ends
        SPELL_DRIVEN_POST = 97035,          // phase 2: a 5 yd ground circle (2 s) under three of the group after each sweep
        // B2 the Keening Mother
        SPELL_KEEN = 97003,                 // 3 s, 30 yd, interruptible (phase 1)
        SPELL_GRAVE_CHILL = 97004,          // bolt on the victim, interruptible
        SPELL_MOURNING_VEIL = 97023,        // magic DoT on someone else
        SPELL_DRINK_THE_GRIEF = 97024,      // a shade reached her: heal and +10 % damage (stacks)
        SPELL_SHROUD_OF_THE_MOURNED = 97037, // the wake (intermission): no damage taken
        SPELL_LAMENT = 97038,               // a mourner's cast (2.5 s, interruptible), the whole barrow
        SPELL_GRIEF_SWELLS = 97039,         // a lament heard: +5 % damage on her (stacks, the fight)
        SPELL_CHORUS_KEEN = 97040,          // phase 2: three of these one after another (2 s each, interruptible)
        // B3 the Burrow-Mother
        SPELL_BROOD_BILE = 97005,           // poison DoT
        SPELL_HEAVING_EARTH = 97026,        // ground circle 6 yd at a point, 2 s
        SPELL_BURROWED = 97027,             // under the earth (also the furrow's hazard aura for the companions)
        SPELL_EMERGE = 97028,               // circle 10 yd round her, 2 s
        SPELL_BROOD_CLUTCH = 97041,         // the clutch's look (a soak of one, 8 s)
        SPELL_CLUTCH_CRUSHED = 97042,       // the soak's hit on whoever stamps it out (custom base points)
        SPELL_CHURNING_EARTH = 97043,       // the furrow: 5 yd round her every second while she tunnels
        // B4 the Last Stoker
        SPELL_PYRE_ASH = 97006,             // ground circle 8 yd at a point, 1.5 s, then burning ground
        SPELL_STOKERS_BRAND = 97007,        // 1.5 s on the victim
        SPELL_BANK_THE_FIRES = 97019,       // circle 8 yd round it, 2 s, then burning ground
        SPELL_CINDER_MARKED = 97033,        // the cinder's mark on its prey
        SPELL_CINDER_BURST = 97034,         // the cinder bursts (6 yd)
        SPELL_THE_LAST_PYRE = 97044,        // the ultimate at 100 heat (4 s)
        SPELL_PYRE_ROAR = 97045,            // the ultimate's hit on everyone outside a cold hearth
        SPELL_COLD_HEARTH = 97046,          // a shelter's look
        SPELL_BRANDED = 97047,              // the brand's stacks on the tank (+10 % fire taken each); a hearth clears them
        // B5 the First Interred
        SPELL_BARROW_WEIGHT = 97008,        // 20 yd pulse
        SPELL_PREFECTS_SENTENCE = 97009,    // on the victim
        SPELL_EMPIRE_REMEMBERS = 97010,     // 25 % enrage
        SPELL_LONG_PATIENCE_ENDS = 97020,   // 8 min hard enrage
        SPELL_KNEEL = 97030,                // circle 10 yd round it, 3 s
        SPELL_GRAVE_WARD = 97031,           // the ward (intermission): no damage taken while its bearers live
        SPELL_BEAR_THE_WARD = 97032,        // a bearer's channel on it
        SPELL_IMPERIAL_TITHE = 97048,       // the tithe's look (a soak of two, 7 s)
        SPELL_TITHE_PAID = 97049,           // the soak's hit (custom base points)
        SPELL_TITHE_UNPAID = 97025,         // the tithe unpaid: the whole barrow
        SPELL_THE_MOUND_FALLS = 97051,      // the 40 % transition (5 s)
        SPELL_EARTHEN_MANTLE = 97052,       // no damage taken during the transition
        SPELL_FALLEN_EARTH = 97053,         // outside the arena, every second
        SPELL_CLOSING_BARROW = 97054,       // the arena's ring of 20 yd (the look on its middle)
        SPELL_CLOSING_BARROW_LAST = 97055,  // the ring of 15 yd (from 15 %)
    };

    enum Creatures : std::uint32_t
    {
        NPC_GRUBLING = 97016,
        NPC_RISEN_SERVANT = 97017,
        NPC_MOURNING_SHADE = 97018,
        NPC_PYRE_CINDER = 97019,
        NPC_WARD_BEARER = 97020,
        NPC_KEENING_MOURNER = 97021,
        NPC_BROOD_CLUTCH = 97022,           // npc_ef_soak_marker
        NPC_COLD_HEARTH = 97023,            // npc_ef_boss_marker (a shelter)
        NPC_IMPERIAL_TITHE = 97024,         // npc_ef_soak_marker
        NPC_CLOSING_BARROW = 97025,         // npc_ef_boss_marker (the arena's middle)
        NPC_DRIVEN_POST = 97026,            // npc_ef_boss_marker (a ground caster)
        NPC_POST_KEEPER = 97011,
        NPC_KEENING_MOTHER = 97012,
        NPC_BURROW_MOTHER = 97013,
        NPC_LAST_STOKER = 97014,
        NPC_FIRST_INTERRED = 97015,
    };

    // The scripts' Talk groups (creatures.yaml `texts`).
    namespace Text
    {
        enum PostKeeper : std::uint8_t { KEEPER_AGGRO = 0, KEEPER_DEATH = 1, KEEPER_SWEEP = 2, KEEPER_NUMBERED = 3, KEEPER_NUMBERED_WHISPER = 4,
                                         KEEPER_RECOUNT_YELL = 5, KEEPER_RECOUNT = 6, KEEPER_POSTS = 7 };
        enum Keening : std::uint8_t { KEENING_AGGRO = 0, KEENING_DEATH = 1, KEENING_KEEN = 2, KEENING_SHADES_YELL = 3, KEENING_SHADES = 4, KEENING_DRINK = 5,
                                      KEENING_WAKE_YELL = 6, KEENING_WAKE = 7, KEENING_WAKE_END = 8, KEENING_CHORUS = 9 };
        enum Burrow : std::uint8_t { BURROW_AGGRO = 0, BURROW_BROOD = 1, BURROW_DOWN = 2, BURROW_UP = 3, BURROW_CLUTCH = 4, BURROW_HATCH = 5,
                                     BURROW_FURROW_WHISPER = 6, BURROW_FRENZY = 7 };
        enum Stoker : std::uint8_t { STOKER_AGGRO = 0, STOKER_HALF = 1, STOKER_DEATH = 2, STOKER_CINDERS = 3, STOKER_CINDER_WHISPER = 4, STOKER_BANK = 5,
                                     STOKER_PYRE = 6, STOKER_PYRE_YELL = 7 };
        enum Interred : std::uint8_t { INTERRED_WAKE = 0, INTERRED_AGGRO = 1, INTERRED_WARD_YELL = 2, INTERRED_ENRAGE = 3, INTERRED_DEATH = 4,
                                       INTERRED_KNEEL_YELL = 5, INTERRED_KNEEL = 6, INTERRED_WARD = 7, INTERRED_WARD_DOWN = 8,
                                       INTERRED_TITHE = 9, INTERRED_TITHE_UNPAID = 10, INTERRED_FALL_YELL = 11, INTERRED_FALL = 12,
                                       INTERRED_CLOSE = 13 };
    }

    // The boss IDs BossAI is told (instance_ef_barrows keeps the seals itself: no boss table, the boss states are no-ops).
    enum Bosses : std::uint32_t { BOSS_POST_KEEPER = 0, BOSS_KEENING_MOTHER = 1, BOSS_BURROW_MOTHER = 2, BOSS_LAST_STOKER = 3, BOSS_FIRST_INTERRED = 4 };

    // The EventMap phases (events.SetPhase; an event scheduled with a phase runs only in it). 0 = every phase.
    enum Phases : std::uint8_t { PHASE_ONE = 1, PHASE_TWO = 2, PHASE_THREE = 3, PHASE_INTERMISSION = 4 };

    // --- timers (milliseconds): first after the pull (or the phase's start), then every [min, max] ---------------------
    namespace Timers
    {
        // B1
        constexpr Timer kTallySweep{ 8000, 15000, 18000 };
        constexpr Timer kNumbered{ 12000, 20000, 24000 };
        constexpr std::uint32_t kNumberedFuse = 6000;          // the mark's duration: struck from the count when it ends
        constexpr Timer kShoulder{ 16000, 18000, 22000 };
        constexpr std::uint32_t kPostsAfterSweepMs = 2700;     // phase 2: the posts as the sweep lands (2.5 s cast)
        constexpr std::uint32_t kPostCastMs = 2000;
        // B2
        constexpr Timer kKeen{ 9000, 14000, 18000 };
        constexpr Timer kGraveChill{ 3000, 6000, 8000 };
        constexpr Timer kMourningVeil{ 6000, 15000, 18000 };
        constexpr Timer kWakeVeil{ 4000, 10000, 12000 };        // the veil during the wake (the healer's job meanwhile)
        constexpr Timer kLament{ 2000, 6000, 8000 };            // each mourner; the three start 1.5 s apart
        constexpr std::uint32_t kLamentCastMs = 2500;
        constexpr std::uint32_t kLamentStaggerMs = 1500;
        constexpr std::uint32_t kWakeLimitMs = 60000;           // the wake's safety net: mourners left are drunk
        constexpr Timer kChorus{ 8000, 22000, 26000 };          // phase 2: three Chorus Keens
        constexpr std::uint32_t kChorusCasts = 3;
        constexpr std::uint32_t kChorusCastMs = 2000;
        constexpr std::uint32_t kChorusGapMs = 700;
        // B3
        constexpr Timer kBrood{ 8000, 20000, 20000 };
        constexpr Timer kBroodFrenzied{ 6000, 14000, 14000 };   // below 33 %
        constexpr Timer kBroodBile{ 4000, 12000, 15000 };
        constexpr Timer kClutch{ 14000, 24000, 28000 };
        constexpr Timer kClutchFrenzied{ 10000, 18000, 20000 };
        constexpr std::uint32_t kClutchFuseMs = 8000;
        constexpr std::uint32_t kBurrowMs = 18000;              // under the earth
        constexpr std::uint32_t kHeaveEveryMs = 3500;           // one Heaving Earth (2 s cast) after another
        constexpr std::uint32_t kEmergeCastMs = 2000;
        constexpr std::uint32_t kChurnEveryMs = 1000;           // the furrow's pulse
        constexpr std::uint32_t kFurrowRetargetMs = 6000;       // a new prey for the furrow
        // B4
        constexpr Timer kPyreAsh{ 6000, 10000, 12000 };
        constexpr Timer kBrand{ 4000, 9000, 11000 };
        constexpr Timer kBank{ 3000, 15000, 15000 };            // from 50 %
        constexpr std::uint32_t kLastPyreCastMs = 4000;
        constexpr std::uint32_t kHearthLifeMs = 6000;           // the shelters: up from the cast's start to after its end
        // B5
        constexpr Timer kWeight{ 2000, 4000, 4000 };
        constexpr std::uint32_t kWeightWardedMs = 3000;         // faster while the ward is up
        constexpr std::uint32_t kWeightArenaMs = 5000;          // phase 3: the arena is inside its reach, slower
        constexpr Timer kSentence{ 6000, 9000, 12000 };
        constexpr Timer kKneel{ 14000, 20000, 24000 };
        constexpr Timer kTithe{ 8000, 25000, 30000 };           // phases 2 and 3
        constexpr std::uint32_t kTitheFuseMs = 7000;
        constexpr std::uint32_t kMoundFallsMs = 5000;           // the transition: immune, then the arena
        constexpr std::uint32_t kArenaPulseMs = 1000;
        constexpr std::uint32_t kHardEnrageMs = 480000;         // Long Patience Ends (8 min; the fight's paper time ~4.5)
    }

    // --- health thresholds and phases ----------------------------------------------------------------------------------
    namespace Thresholds
    {
        constexpr std::array<std::uint8_t, 1> kRecount{ 50 };            // B1: phase 2
        constexpr std::array<std::uint8_t, 1> kWake{ 65 };               // B2: the wake, then phase 2
        constexpr std::array<std::uint8_t, 1> kShades{ 35 };             // B2: three mourning shades (phase 2)
        constexpr std::array<std::uint8_t, 2> kBurrow{ 66, 33 };         // B3: she burrows (after the second: frenzied)
        constexpr std::array<std::uint8_t, 3> kCinders{ 75, 50, 25 };    // B4: two cinders
        constexpr std::uint8_t kBank = 50;                               // B4: phase 2, Bank the Fires and a faster pyre
        constexpr std::array<std::uint8_t, 1> kWard{ 70 };               // B5: the Grave Ward (intermission), then phase 2
        constexpr std::array<std::uint8_t, 1> kMoundFalls{ 40 };         // B5: the arena (phase 3)
        constexpr std::uint8_t kEnrage = 25;                             // B5: The Empire Remembers
        constexpr std::array<std::uint8_t, 1> kCloses{ 15 };             // B5: the arena's second shrink
    }

    // --- B1 ----------------------------------------------------------------------------------------------------------
    constexpr std::uint32_t NumberedPerMark(std::uint8_t phase) { return phase >= PHASE_TWO ? 2 : 1; }
    constexpr std::uint32_t kPostsPerSweep = 3;
    constexpr float kPostRadius = 5.0f;

    // --- B2 ----------------------------------------------------------------------------------------------------------
    constexpr std::uint32_t kMournersPerWake = 3;
    constexpr float kMournerRadius = 22.0f;            // round the Widow's Barrow's middle, inside its wall

    // --- B3: above the earth (the brood, the bile, the clutch), under it (the furrow, the heaves), coming up ------------
    enum class BurrowPhase : std::uint8_t { Above, Burrowed, Emerging };

    struct BurrowState
    {
        BurrowPhase phase = BurrowPhase::Above;
        std::uint32_t msLeft = 0;     // in the current timed phase
        std::uint32_t heaveIn = 0;    // until the next heave (Burrowed)
        std::uint32_t churnIn = 0;    // until the furrow's next pulse (Burrowed)
        std::uint32_t retargetIn = 0; // until the furrow picks its next prey (Burrowed)
        std::uint32_t heaves = 0;     // this burrow's heaves
        std::uint32_t burrows = 0;    // so far
    };

    // What the Burrow-Mother does this tick (the furrow's pulse and retarget are flags beside it).
    enum class BurrowAction : std::uint8_t { None, Heave, Emerge, Surface };

    struct BurrowTick
    {
        BurrowAction action = BurrowAction::None;
        bool churn = false;       // the furrow's pulse round her
        bool retarget = false;    // a new prey for the furrow
    };

    inline void StartBurrow(BurrowState& s)
    {
        s.phase = BurrowPhase::Burrowed;
        s.msLeft = Timers::kBurrowMs;
        s.heaveIn = 2000;
        s.churnIn = Timers::kChurnEveryMs;
        s.retargetIn = 0;             // a prey at once
        s.heaves = 0;
        ++s.burrows;
    }

    // Advances the burrow by `diff` ms: the furrow pulses and changes prey on its own clock (casting or not); a heave
    // every kHeaveEveryMs; Emerge's cast when the time is up; back above when it has been cast. `casting`: she is still
    // casting (a heave or Emerge): time runs on, no new cast starts.
    inline BurrowTick StepBurrow(BurrowState& s, std::uint32_t diff, bool casting)
    {
        BurrowTick t;
        auto run = [diff](std::uint32_t& ms) { ms = ms > diff ? ms - diff : 0; };
        switch (s.phase)
        {
            case BurrowPhase::Above:
                return t;
            case BurrowPhase::Burrowed:
                run(s.msLeft);
                run(s.heaveIn);
                run(s.churnIn);
                if (!s.churnIn)
                {
                    s.churnIn = Timers::kChurnEveryMs;
                    t.churn = true;
                }
                if (!s.retargetIn)
                {
                    s.retargetIn = Timers::kFurrowRetargetMs;
                    t.retarget = true;
                }
                else
                    run(s.retargetIn);
                if (casting)
                    return t;
                if (!s.msLeft)
                {
                    s.phase = BurrowPhase::Emerging;
                    s.msLeft = Timers::kEmergeCastMs + 500;
                    t.churn = t.retarget = false;
                    t.action = BurrowAction::Emerge;
                    return t;
                }
                if (!s.heaveIn)
                {
                    s.heaveIn = Timers::kHeaveEveryMs;
                    ++s.heaves;
                    t.action = BurrowAction::Heave;
                }
                return t;
            case BurrowPhase::Emerging:
                run(s.msLeft);
                if (s.msLeft || casting)
                    return t;
                s.phase = BurrowPhase::Above;
                t.action = BurrowAction::Surface;
                return t;
        }
        return t;
    }

    // The furrow's speed (x the run speed of 7 yd/s): slower than a walking player, so its prey keeps ahead of it.
    constexpr float kFurrowSpeedRate = 0.4f;
    constexpr float kFurrowRadius = 5.0f;
    // The clutch: a soak of one (4 yd); stamped out it hurts its soakers 600 shared (300 each at least), left alone it
    // hatches three grublings.
    constexpr float kClutchRadius = 4.0f;
    constexpr std::uint32_t kClutchDamage = 600;
    constexpr std::uint32_t kClutchFloor = 300;
    constexpr std::uint32_t kGrublingsPerHatch = 3;

    // --- B4: the pyre's heat (an energy bar) -----------------------------------------------------------------------------
    constexpr std::uint32_t kHeatPerSecond = 2;             // phase 1: ~50 s to the Last Pyre
    constexpr std::uint32_t kHeatPerSecondBanked = 3;       // from 50 %: ~33 s
    constexpr std::uint32_t kHeatPerBurst = 10;             // a cinder that reaches its prey stokes the pyre
    constexpr std::uint32_t kHearths = 3;
    constexpr float kHearthRadius = 4.0f;
    constexpr float kHearthRing = 14.0f;                    // round the Pyre Barrow's middle
    constexpr std::uint32_t kBrandedMaxStacks = 8;

    // --- B5 --------------------------------------------------------------------------------------------------------------
    // The tithe: a soak of two (5 yd); 1400 shared (450 each at least); unpaid, the whole barrow is hurt and a servant rises.
    constexpr float kTitheRadius = 5.0f;
    constexpr std::uint32_t kTitheNeeded = 2;
    constexpr std::uint32_t kTitheDamage = 1400;
    constexpr std::uint32_t kTitheFloor = 450;
    constexpr float kTitheFromBoss = 10.0f;                 // between the boss and someone of the group, never under the tank
    // The arena: the First Barrow (radius 34) falls in to 20 yd at 40 %, to 15 yd at 15 %.
    constexpr std::array<float, 2> kArenaStages{ 20.0f, 15.0f };

    // The First Interred's phases: 1 (100-70), the ward (an intermission), 2 (70-40), the mound falls (a timed
    // intermission), 3 (40-0). The ward and the fall are the toolkit's Intermission; this is which phase follows.
    constexpr std::uint8_t InterredPhaseAfter(std::uint8_t intermissionOf)
    {
        return intermissionOf == PHASE_ONE ? PHASE_TWO : PHASE_THREE;
    }

    // --- the rooms (design (u, v) -> world: x = 513.33 - v, y = 513.33 - u; layout.yaml) ---------------------------------
    struct Point
    {
        float x, y, z;
    };
    constexpr float kDesignOrigin = 513.33f;
    constexpr Point FromDesign(float u, float v, float z) { return Point{ kDesignOrigin - v, kDesignOrigin - u, z }; }

    constexpr Point kKeepersRing = FromDesign(245.0f, 279.0f, 27.1f);   // B1's room, radius 24
    constexpr float kKeepersRadius = 24.0f;
    constexpr Point kWidowsBarrow = FromDesign(88.0f, 200.0f, 21.2f);   // B2's room, radius 30
    constexpr float kWidowsRadius = 30.0f;
    constexpr Point kBroodBarrow = FromDesign(402.0f, 200.0f, 21.2f);   // B3's room, radius 30
    constexpr float kBroodRadius = 30.0f;
    constexpr Point kPyreBarrow = FromDesign(133.0f, 90.0f, 21.2f);     // B4's room, radius 30
    constexpr float kPyreRadius = 30.0f;
    constexpr Point kFirstBarrow = FromDesign(245.0f, 50.0f, 12.1f);    // B5's room, radius 34
    constexpr float kFirstRadius = 34.0f;

    inline Emberfall::Bosses::Point2 Flat(Point p) { return Emberfall::Bosses::Point2{ p.x, p.y }; }

    // `count` points on a circle of `radius` round `c`, the first at `start` radians, evenly spaced.
    inline std::vector<Point> Ring(Point c, float radius, std::uint32_t count, float start)
    {
        std::vector<Point> out;
        for (std::uint32_t i = 0; i < count; ++i)
        {
            float const a = start + 6.2831853f * float(i) / float(count ? count : 1);
            out.push_back(Point{ c.x + std::cos(a) * radius, c.y + std::sin(a) * radius, c.z });
        }
        return out;
    }

    // The `count` of `slots` evenly spaced places on a circle round `c` that are farthest from `from`: adds that rise
    // at the room's wall on the far side from the boss, so their walk to her is long wherever she is tanked.
    inline std::vector<Point> FarthestOnRing(Point c, float radius, std::uint32_t slots, std::uint32_t count, Point from, float start)
    {
        std::vector<Point> ring = Ring(c, radius, slots, start);
        std::sort(ring.begin(), ring.end(), [&](Point const& a, Point const& b) {
            return std::hypot(a.x - from.x, a.y - from.y) > std::hypot(b.x - from.x, b.y - from.y);
        });
        if (ring.size() > count)
            ring.resize(count);
        return ring;
    }

    // `p` pulled towards the room's middle until it is `margin` yards inside its wall.
    inline Point IntoRoom(Point p, Point centre, float radius, float margin)
    {
        float const dx = p.x - centre.x, dy = p.y - centre.y;
        float const d = std::hypot(dx, dy);
        float const max = std::max(radius - margin, 1.0f);
        if (d <= max)
            return p;
        return Point{ centre.x + dx / d * max, centre.y + dy / d * max, p.z };
    }

    // The point `along` yards from `from` towards `to` (`from` itself if they are one point).
    inline Point Towards(Point from, Point to, float along)
    {
        float const dx = to.x - from.x, dy = to.y - from.y;
        float const d = std::hypot(dx, dy);
        if (d < 0.01f)
            return from;
        float const k = std::min(along, d) / d;
        return Point{ from.x + dx * k, from.y + dy * k, from.z };
    }

    // Adds: how many, how far out.
    constexpr std::uint32_t kShadesPerWave = 3;
    constexpr float kShadeSpawnRadius = 25.0f;         // round the Widow's Barrow's middle, inside its wall: the far side from her
    constexpr float kShadeSpeedRate = 0.25f;           // x the run speed (7 yd/s): 1.75 yd/s, ~13 s from the wall to her
    constexpr float kShadeReach = 3.5f;                // drunk when this close
    constexpr std::uint32_t kGrublingsPerBrood = 2;
    constexpr std::uint32_t kGrublingsPerBurrow = 4;
    constexpr std::uint32_t kCindersPerWave = 2;
    constexpr float kCinderRunSpeed = 0.55f;           // x the run speed (7 yd/s): ~3.9 yd/s, a player outruns it
    constexpr float kCinderReach = 2.5f;               // bursts when this close to its prey
    constexpr std::uint32_t kBearersPerWard = 2;
    constexpr float kBearerRadius = 26.0f;             // at the First Barrow's edge, either side of the prefect
    constexpr std::uint32_t kServantsPerWard = 2;
}

#endif
