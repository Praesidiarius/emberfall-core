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

// The Barrows of Cairnmoor's boss fights without a core (Emberfall docs/design/dungeon-barrows.md §5a): the spell,
// creature and text IDs the boss scripts use (content/data/spells/barrows.yaml, content/data/barrows/creatures.yaml),
// every timer, the health thresholds, the phases and the places adds come from. emberfall_barrows_bosses.cpp runs the
// fights from it (BossAI, EventMap, SummonList); tests/dungeon_tests.cpp checks the timers and the phase logic.

#ifndef EMBERFALL_BARROWS_BOSS_LOGIC_H
#define EMBERFALL_BARROWS_BOSS_LOGIC_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Emberfall::Barrows
{
    // --- IDs (content/data/spells/barrows.yaml) ------------------------------------------------------------------------
    enum Spells : std::uint32_t
    {
        // B1 the Post-Keeper
        SPELL_NUMBERED = 97001,             // the run-out mark (6 s)
        SPELL_SHOULDER_THE_POST = 97002,    // knock-back on the victim
        SPELL_TALLY_SWEEP = 97021,          // frontal cone 120 degrees, 10 yd, 2.5 s
        SPELL_STRUCK_FROM_THE_COUNT = 97022, // 8 yd round the numbered one when the mark ends
        // B2 the Keening Mother
        SPELL_KEEN = 97003,                 // 3 s, 30 yd, interruptible
        SPELL_GRAVE_CHILL = 97004,          // bolt on the victim, interruptible
        SPELL_MOURNING_VEIL = 97023,        // magic DoT on someone else
        SPELL_DRINK_THE_GRIEF = 97024,      // a shade reached her: heal and +10 % damage (stacks)
        // B3 the Burrow-Mother
        SPELL_BROOD_BILE = 97005,           // poison DoT
        SPELL_HEAVING_EARTH = 97026,        // ground circle 6 yd at a point, 2 s
        SPELL_BURROWED = 97027,             // the look of being under the earth
        SPELL_EMERGE = 97028,               // circle 10 yd round her, 2 s
        // B4 the Last Stoker
        SPELL_PYRE_ASH = 97006,             // ground circle 8 yd at a point, 1.5 s, then burning ground
        SPELL_STOKERS_BRAND = 97007,        // 1.5 s on the victim
        SPELL_BANK_THE_FIRES = 97019,       // circle 8 yd round it, 2 s, then burning ground
        SPELL_CINDER_MARKED = 97033,        // the cinder's mark on its prey
        SPELL_CINDER_BURST = 97034,         // the cinder bursts (6 yd)
        // B5 the First Interred
        SPELL_BARROW_WEIGHT = 97008,        // 20 yd pulse
        SPELL_PREFECTS_SENTENCE = 97009,    // on the victim
        SPELL_EMPIRE_REMEMBERS = 97010,     // 25 % enrage
        SPELL_LONG_PATIENCE_ENDS = 97020,   // 6 min hard enrage
        SPELL_KNEEL = 97030,                // circle 10 yd round it, 3 s
        SPELL_GRAVE_WARD = 97031,           // 90 % less damage taken while its bearers live
        SPELL_BEAR_THE_WARD = 97032,        // a bearer's channel on it
    };

    enum Creatures : std::uint32_t
    {
        NPC_GRUBLING = 97016,
        NPC_RISEN_SERVANT = 97017,
        NPC_MOURNING_SHADE = 97018,
        NPC_PYRE_CINDER = 97019,
        NPC_WARD_BEARER = 97020,
        NPC_POST_KEEPER = 97011,
        NPC_KEENING_MOTHER = 97012,
        NPC_BURROW_MOTHER = 97013,
        NPC_LAST_STOKER = 97014,
        NPC_FIRST_INTERRED = 97015,
    };

    // The scripts' Talk groups (creatures.yaml `texts`).
    namespace Text
    {
        enum PostKeeper : std::uint8_t { KEEPER_AGGRO = 0, KEEPER_DEATH = 1, KEEPER_SWEEP = 2, KEEPER_NUMBERED = 3, KEEPER_NUMBERED_WHISPER = 4 };
        enum Keening : std::uint8_t { KEENING_AGGRO = 0, KEENING_DEATH = 1, KEENING_KEEN = 2, KEENING_SHADES_YELL = 3, KEENING_SHADES = 4, KEENING_DRINK = 5 };
        enum Burrow : std::uint8_t { BURROW_AGGRO = 0, BURROW_BROOD = 1, BURROW_DOWN = 2, BURROW_UP = 3 };
        enum Stoker : std::uint8_t { STOKER_AGGRO = 0, STOKER_HALF = 1, STOKER_DEATH = 2, STOKER_CINDERS = 3, STOKER_CINDER_WHISPER = 4, STOKER_BANK = 5 };
        enum Interred : std::uint8_t { INTERRED_WAKE = 0, INTERRED_AGGRO = 1, INTERRED_WARD_YELL = 2, INTERRED_ENRAGE = 3, INTERRED_DEATH = 4,
                                       INTERRED_KNEEL_YELL = 5, INTERRED_KNEEL = 6, INTERRED_WARD = 7, INTERRED_WARD_DOWN = 8 };
    }

    // The boss IDs BossAI is told (instance_ef_barrows keeps the seals itself: no boss table, the boss states are no-ops).
    enum Bosses : std::uint32_t { BOSS_POST_KEEPER = 0, BOSS_KEENING_MOTHER = 1, BOSS_BURROW_MOTHER = 2, BOSS_LAST_STOKER = 3, BOSS_FIRST_INTERRED = 4 };

    // --- timers (milliseconds): first after the pull, then every [min, max] ----------------------------------------------
    struct Timer
    {
        std::uint32_t first;
        std::uint32_t min;
        std::uint32_t max;
    };

    namespace Timers
    {
        // B1
        constexpr Timer kTallySweep{ 8000, 15000, 18000 };
        constexpr Timer kNumbered{ 12000, 20000, 24000 };
        constexpr std::uint32_t kNumberedFuse = 6000;          // the mark's duration: struck from the count when it ends
        constexpr Timer kShoulder{ 16000, 18000, 22000 };
        // B2
        constexpr Timer kKeen{ 9000, 14000, 18000 };
        constexpr Timer kGraveChill{ 3000, 6000, 8000 };
        constexpr Timer kMourningVeil{ 6000, 15000, 18000 };
        // B3
        constexpr Timer kBrood{ 8000, 20000, 20000 };
        constexpr Timer kBroodBile{ 4000, 12000, 15000 };
        constexpr std::uint32_t kBurrowMs = 15000;             // under the earth
        constexpr std::uint32_t kHeaveEveryMs = 2500;          // one Heaving Earth (2 s cast) after another
        constexpr std::uint32_t kEmergeCastMs = 2000;
        // B4
        constexpr Timer kPyreAsh{ 6000, 10000, 12000 };
        constexpr Timer kBrand{ 4000, 9000, 11000 };
        constexpr Timer kBank{ 3000, 15000, 15000 };            // from 50 %
        constexpr std::uint32_t kCinderReachMs = 1000;          // how often a cinder looks whether it has reached its prey
        // B5
        constexpr Timer kWeight{ 2000, 4000, 4000 };
        constexpr std::uint32_t kWeightWardedMs = 3000;         // faster while the ward is up
        constexpr Timer kSentence{ 6000, 9000, 12000 };
        constexpr Timer kKneel{ 14000, 20000, 24000 };
        constexpr std::uint32_t kHardEnrageMs = 360000;         // Long Patience Ends
    }

    // --- health thresholds ------------------------------------------------------------------------------------------
    namespace Thresholds
    {
        constexpr std::array<std::uint8_t, 2> kShades{ 70, 35 };         // B2: three mourning shades
        constexpr std::array<std::uint8_t, 2> kBurrow{ 66, 33 };         // B3: she burrows
        constexpr std::array<std::uint8_t, 3> kCinders{ 75, 50, 25 };    // B4: two cinders
        constexpr std::uint8_t kBank = 50;                               // B4: Bank the Fires from here
        constexpr std::array<std::uint8_t, 2> kWard{ 70, 45 };           // B5: the Grave Ward
        constexpr std::uint8_t kEnrage = 25;                             // B5: The Empire Remembers
    }

    // The thresholds of `list` (descending) crossed going from health `from` to `to` (percent) that have not fired yet
    // (`fired`: a bit per entry), in order; marks them fired. A big hit across two thresholds fires both, once each.
    template <std::size_t N>
    inline std::vector<std::size_t> CrossedThresholds(std::array<std::uint8_t, N> const& list, float to, std::uint32_t& fired)
    {
        std::vector<std::size_t> out;
        for (std::size_t i = 0; i < N; ++i)
            if (!(fired & (1u << i)) && to <= float(list[i]))
            {
                fired |= 1u << i;
                out.push_back(i);
            }
        return out;
    }

    // --- the fights' phases ------------------------------------------------------------------------------------------
    // B3: above the earth (the brood, the bile), under it (the heaves), coming up (Emerge's cast).
    enum class BurrowPhase : std::uint8_t { Above, Burrowed, Emerging };

    struct BurrowState
    {
        BurrowPhase phase = BurrowPhase::Above;
        std::uint32_t msLeft = 0;     // in the current timed phase
        std::uint32_t heaveIn = 0;    // until the next heave (Burrowed)
        std::uint32_t heaves = 0;     // this burrow's heaves
    };

    // What the Burrow-Mother does this tick.
    enum class BurrowAction : std::uint8_t { None, Heave, Emerge, Surface };

    inline void StartBurrow(BurrowState& s)
    {
        s.phase = BurrowPhase::Burrowed;
        s.msLeft = Timers::kBurrowMs;
        s.heaveIn = 1500;
        s.heaves = 0;
    }

    // Advances the burrow by `diff` ms: a heave every kHeaveEveryMs while under; Emerge's cast when the time is up; back
    // above when it has been cast. `casting`: she is still casting (a heave or Emerge); time runs on, nothing new starts.
    inline BurrowAction StepBurrow(BurrowState& s, std::uint32_t diff, bool casting)
    {
        switch (s.phase)
        {
            case BurrowPhase::Above:
                return BurrowAction::None;
            case BurrowPhase::Burrowed:
                s.msLeft = s.msLeft > diff ? s.msLeft - diff : 0;
                s.heaveIn = s.heaveIn > diff ? s.heaveIn - diff : 0;
                if (casting)
                    return BurrowAction::None;
                if (!s.msLeft)
                {
                    s.phase = BurrowPhase::Emerging;
                    s.msLeft = Timers::kEmergeCastMs + 500;
                    return BurrowAction::Emerge;
                }
                if (!s.heaveIn)
                {
                    s.heaveIn = Timers::kHeaveEveryMs;
                    ++s.heaves;
                    return BurrowAction::Heave;
                }
                return BurrowAction::None;
            case BurrowPhase::Emerging:
                s.msLeft = s.msLeft > diff ? s.msLeft - diff : 0;
                if (s.msLeft || casting)
                    return BurrowAction::None;
                s.phase = BurrowPhase::Above;
                return BurrowAction::Surface;
        }
        return BurrowAction::None;
    }

    // B5: the ward is up while any of its bearers lives; it drops with the last one.
    struct WardState
    {
        std::uint32_t bearersAlive = 0;
        std::uint32_t wards = 0;      // raised so far
        bool up = false;
    };
    inline void RaiseWard(WardState& w, std::uint32_t bearers)
    {
        w.bearersAlive = bearers;
        w.up = bearers > 0;
        ++w.wards;
    }
    // A bearer died: true when that drops the ward.
    inline bool BearerDied(WardState& w)
    {
        if (!w.up || !w.bearersAlive)
            return false;
        --w.bearersAlive;
        if (w.bearersAlive)
            return false;
        w.up = false;
        return true;
    }

    // --- the rooms (design (u, v) -> world: x = 513.33 - v, y = 513.33 - u; layout.yaml) ---------------------------------
    struct Point
    {
        float x, y, z;
    };
    constexpr float kDesignOrigin = 513.33f;
    constexpr Point FromDesign(float u, float v, float z) { return Point{ kDesignOrigin - v, kDesignOrigin - u, z }; }

    constexpr Point kWidowsBarrow = FromDesign(88.0f, 200.0f, 21.2f);   // B2's room, radius 30
    constexpr float kWidowsRadius = 30.0f;
    constexpr Point kBroodBarrow = FromDesign(402.0f, 200.0f, 21.2f);   // B3's room, radius 30
    constexpr Point kPyreBarrow = FromDesign(133.0f, 90.0f, 21.2f);     // B4's room, radius 30
    constexpr float kPyreRadius = 30.0f;
    constexpr Point kFirstBarrow = FromDesign(245.0f, 50.0f, 12.1f);    // B5's room, radius 34
    constexpr float kFirstRadius = 34.0f;

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

    // Adds: how many, how far out.
    constexpr std::uint32_t kShadesPerWave = 3;
    constexpr float kShadeSpawnRadius = 25.0f;         // round the Widow's Barrow's middle, inside its wall: the far side from her
    constexpr float kShadeSpeedRate = 0.25f;           // x the run speed (7 yd/s): 1.75 yd/s, ~13 s from the wall to her
    constexpr float kShadeReach = 3.5f;                // drunk when this close
    constexpr std::uint32_t kGrublingsPerBrood = 2;
    constexpr std::uint32_t kGrublingsPerBurrow = 4;
    constexpr std::uint32_t kCindersPerWave = 2;
    constexpr float kCinderSpawnRadius = 22.0f;
    constexpr float kCinderRunSpeed = 0.55f;           // x the run speed (7 yd/s): ~3.9 yd/s, a player outruns it
    constexpr float kCinderReach = 2.5f;               // bursts when this close to its prey
    constexpr std::uint32_t kBearersPerWard = 2;
    constexpr float kBearerRadius = 26.0f;             // at the First Barrow's edge, either side of the prefect
    constexpr std::uint32_t kServantsPerWard = 2;
}

#endif
