// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 the Emberfall authors.
//
// EfFishRules.h: the living waters' rules (docs/design/fishing.md §3-§5) as small pure functions: the habitat bits,
// the time-of-day and weather buckets, which species may bite in a cell now, its weight, the size of a hooked fish, the
// safe band, and the honest water clues (a clue only shows where its species could bite now). Header-only, standard
// library only, integer arithmetic only (the client and the server get the same numbers on every compiler).
//
// This one file is shared: the client (client/src/data/) draws the clues with it, and mod-emberfall carries a verbatim
// copy (src/fishing/EfFishRules.h) for the bite roll. tools/py/tests/test_fishing.py checks that the copies are equal;
// both test suites run the same vectors. Change both together, as TalentRules.h.

#ifndef EMBERFALL_FISH_RULES_H
#define EMBERFALL_FISH_RULES_H

#include <array>
#include <cmath>
#include <cstdint>

namespace effish
{
    // --- habitats (fishing.md §3.1): a cell stores a bitmask ------------------------------------------------------
    enum Habitat : std::uint8_t
    {
        kShallows = 1,
        kDeep = 2,
        kInlet = 4,
        kReeds = 8,
        kRocks = 16,
        kRuins = 32,
    };
    constexpr int kHabitatCount = 6;

    // The cell grid: 8-yd cells in world yards per map (fishing.md §3.2), floor(x / 8), floor(y / 8).
    constexpr float kCellYd = 8.0f;
    inline std::int32_t CellOf(float world) { return static_cast<std::int32_t>(std::floor(world / kCellYd)); }
    inline std::uint64_t CellKey(std::uint32_t map, std::int32_t cx, std::int32_t cy)
    {
        return (static_cast<std::uint64_t>(map & 0xFFFFu) << 48) |
               (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx) & 0xFFFFFFu) << 24) |
               (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cy) & 0xFFFFFFu));
    }

    // --- species data (ef_fish_species on the server, EfFishSpecies.dbc on the client) ----------------------------
    enum class Rarity : std::uint8_t { Common = 0, Uncommon = 1, Rare = 2, Legendary = 3 };

    enum TimeBit : std::uint8_t { kDawn = 1, kDay = 2, kDusk = 4, kNight = 8 };
    constexpr std::uint8_t kAnyTime = 15;

    enum WeatherBit : std::uint8_t { kClear = 1, kRain = 2, kSnow = 4, kStorm = 8, kAfterRain = 16 };
    constexpr std::uint8_t kAnyWeather = 31;

    enum class Clue : std::uint8_t { None = 0, Rings = 1, Swirl = 2, Bubbles = 3, Wake = 4, Leap = 5, Circle = 6 };

    // A multiplier that applies when its time and weather match (0 = any).
    struct Boost
    {
        std::uint8_t timeMask = 0;
        std::uint8_t weatherMask = 0;
        std::uint16_t permil = 0;   // 0: unused slot
    };

    // A phase of a many-phase fight (the legendary): this style until the fish's strength falls to `untilPermil`.
    struct Phase
    {
        std::uint32_t style = 0;    // 0: unused slot
        std::uint32_t untilPermil = 0;
    };

    struct Species
    {
        std::uint32_t id = 0;
        std::uint32_t item = 0;
        Rarity rarity = Rarity::Common;
        std::uint32_t style = 0;            // EfFishStyle / ef_fish_style; 0 for commons (no fight)
        std::uint32_t speedPermil = 1000;   // a quicker fish (the char: 1300)
        std::uint32_t sizeTypMm = 0;
        std::uint32_t sizeMaxMm = 0;
        std::uint32_t weightK = 1000;       // grams = weightK / 100000 x cm^3
        Clue clue = Clue::None;
        std::uint8_t timeMask = kAnyTime;
        std::uint8_t weatherMask = kAnyWeather;
        std::array<std::uint16_t, kHabitatCount> habitatPermil{};   // by habitat bit index (shallows first)
        std::array<std::uint16_t, 4> waters{};                      // ef_fish_water ids; 0 = unused
        std::array<Boost, 3> boosts{};
        std::array<Phase, 3> phases{};      // the legendary; empty: one phase of `style`
        std::uint32_t gateQuest = 0;        // the legendary: bites only while this quest is in the log or done
        bool needsGroundBait = false;       // the legendary: bites only near the angler's ground-bait
    };

    // --- the conditions (fishing.md §4.2) ----------------------------------------------------------------------------
    // The server's local hour (0-23) to its time bucket: dawn 05-08, day 08-18, dusk 18-21, night 21-05.
    inline TimeBit TimeOfHour(int hour)
    {
        hour = ((hour % 24) + 24) % 24;
        if (hour >= 5 && hour < 8)
            return kDawn;
        if (hour >= 8 && hour < 18)
            return kDay;
        if (hour >= 18 && hour < 21)
            return kDusk;
        return kNight;
    }

    // 3.3.5's WeatherState (SMSG_WEATHER's type, the core's Weather::GetWeatherState) to its class.
    inline WeatherBit WeatherClassOf(std::uint32_t state)
    {
        switch (state)
        {
            case 3: case 4: case 5: case 90:      // light, medium, heavy rain; black rain
                return kRain;
            case 6: case 7: case 8: case 106:     // snow; black snow
                return kSnow;
            case 22: case 41: case 42: case 86:   // sandstorms, thunders
                return kStorm;
            default:                              // fine, fog
                return kClear;
        }
    }

    // How long after rain the water still counts as "after rain" (fishing.md §4.2).
    constexpr std::uint32_t kAfterRainSeconds = 30 * 60;

    // The weather bits a condition is checked against: the current class, plus kAfterRain when it is clear and rain
    // ended within kAfterRainSeconds.
    inline std::uint8_t WeatherBits(WeatherBit current, std::uint32_t secondsSinceRain)
    {
        std::uint8_t bits = current;
        if (current == kClear && secondsSinceRain <= kAfterRainSeconds)
            bits = static_cast<std::uint8_t>(bits | kAfterRain);
        return bits;
    }

    inline bool InWater(Species const& s, std::uint32_t water)
    {
        for (std::uint16_t w : s.waters)
            if (w != 0 && w == water)
                return true;
        return false;
    }

    // The best habitat weight of the species among the cell's bits (fishing.md §3.1: a cell may hold several).
    inline std::uint32_t HabitatPermil(Species const& s, std::uint8_t cellMask)
    {
        std::uint32_t best = 0;
        for (int i = 0; i < kHabitatCount; ++i)
            if ((cellMask & (1u << i)) && s.habitatPermil[static_cast<std::size_t>(i)] > best)
                best = s.habitatPermil[static_cast<std::size_t>(i)];
        return best;
    }

    inline bool ConditionsHold(Species const& s, std::uint8_t timeBit, std::uint8_t weatherBits)
    {
        return (s.timeMask & timeBit) != 0 && (s.weatherMask & weatherBits) != 0;
    }

    // The product of the boosts that match now, x1000.
    inline std::uint32_t BoostPermil(Species const& s, std::uint8_t timeBit, std::uint8_t weatherBits)
    {
        std::uint64_t out = 1000;
        for (Boost const& b : s.boosts)
        {
            if (b.permil == 0)
                continue;
            if (b.timeMask != 0 && (b.timeMask & timeBit) == 0)
                continue;
            if (b.weatherMask != 0 && (b.weatherMask & weatherBits) == 0)
                continue;
            out = out * b.permil / 1000;
        }
        return static_cast<std::uint32_t>(out);
    }

    // Base weight by rarity (fishing.md §4.3): common 100, uncommon 30, rare 8; the legendary, once its chain is done,
    // as a rare of weight 2 (§7.4).
    inline std::uint32_t BaseWeight(Rarity r)
    {
        switch (r)
        {
            case Rarity::Common: return 100;
            case Rarity::Uncommon: return 30;
            case Rarity::Rare: return 8;
            case Rarity::Legendary: return 2;
        }
        return 0;
    }

    // The natural activity of a species in a cell now (no bait, no skill, no shoal): its base weight x habitat x
    // boosts, x1,000,000 (a common in its best habitat with no boost: 100,000,000). 0: it does not live here now.
    // The clues and the bite roll both start from this number.
    inline std::uint64_t Activity(Species const& s, std::uint32_t water, std::uint8_t cellMask, std::uint8_t timeBit,
                                  std::uint8_t weatherBits)
    {
        if (!InWater(s, water) || !ConditionsHold(s, timeBit, weatherBits))
            return 0;
        std::uint64_t const habitat = HabitatPermil(s, cellMask);
        if (habitat == 0)
            return 0;
        return std::uint64_t{BaseWeight(s.rarity)} * habitat * BoostPermil(s, timeBit, weatherBits);
    }

    // The bait multiplier x1000 (fishing.md §4.3, §4.5). `likePermil` is the species' value for the bait on the hook
    // (ef_fish_species_bait; 1000 when the species does not list it, 0 when it refuses it); -1: no bait at all, which
    // halves uncommons and refuses rares and the legendary.
    inline std::uint32_t BaitPermil(Rarity r, int likePermil)
    {
        if (likePermil < 0)
        {
            if (r == Rarity::Common)
                return 1000;
            return r == Rarity::Uncommon ? 500u : 0u;
        }
        return static_cast<std::uint32_t>(likePermil);
    }

    // The skill gates (fishing.md §4.3, §7.3): an uncommon bites from the zone's base + 45, a rare or the legendary
    // from base + 95 (where the casts never miss).
    inline bool PassesGate(Rarity r, int skill, int zoneBase)
    {
        switch (r)
        {
            case Rarity::Common: return true;
            case Rarity::Uncommon: return skill >= zoneBase + 45;
            default: return skill >= zoneBase + 95;
        }
    }

    // --- a deterministic generator (splitmix64), shared by the roll, the sizes, the clues and the fight -------------
    struct Rng
    {
        std::uint64_t state;
        explicit Rng(std::uint64_t seed) : state(seed) {}
        std::uint64_t Next()
        {
            std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            return z ^ (z >> 31);
        }
        // [0, n)
        std::uint32_t Below(std::uint32_t n) { return n == 0 ? 0u : static_cast<std::uint32_t>(Next() % n); }
        // [lo, hi]
        std::int32_t Range(std::int32_t lo, std::int32_t hi)
        {
            if (hi <= lo)
                return lo;
            return lo + static_cast<std::int32_t>(Below(static_cast<std::uint32_t>(hi - lo + 1)));
        }
    };

    inline std::uint64_t Mix(std::uint64_t a, std::uint64_t b)
    {
        Rng r(a * 0x100000001B3ull ^ (b + 0x9E3779B97F4A7C15ull));
        return r.Next();
    }

    // --- the size (fishing.md §4.3 step 5) ---------------------------------------------------------------------------
    // A normal spread round the typical size (sd 18 %, the sum of four draws), kept between 55 % of it and 90 % of the
    // record; 2 % come from the record's top tenth.
    inline std::uint32_t PickSizeMm(Rng& rng, std::uint32_t typMm, std::uint32_t maxMm)
    {
        if (maxMm <= typMm)
            return typMm;
        if (rng.Below(1000) < 20)
            return static_cast<std::uint32_t>(rng.Range(static_cast<std::int32_t>(maxMm * 9 / 10),
                                                         static_cast<std::int32_t>(maxMm)));
        std::int64_t sum = 0;
        for (int i = 0; i < 4; ++i)
            sum += rng.Below(1001);                        // mean 2000, sd ~577
        std::int64_t const z1000 = (sum - 2000) * 1000 / 577; // ~ a standard normal x1000
        std::int64_t size = static_cast<std::int64_t>(typMm) + static_cast<std::int64_t>(typMm) * 180 * z1000 / 1000000;
        std::int64_t const lo = static_cast<std::int64_t>(typMm) * 55 / 100;
        std::int64_t const hi = static_cast<std::int64_t>(maxMm) * 9 / 10;
        if (size < lo)
            size = lo;
        if (size > hi)
            size = hi;
        return static_cast<std::uint32_t>(size);
    }

    // The fish's weight in grams: weightK / 100000 x cm^3.
    inline std::uint32_t WeightGrams(std::uint32_t weightK, std::uint32_t sizeMm)
    {
        std::uint64_t const mm = sizeMm;
        // cm^3 = mm^3 / 1000
        return static_cast<std::uint32_t>(std::uint64_t{weightK} * mm * mm * mm / 1000 / 100000);
    }

    // Size scales the fight (fishing.md §6.3): x0.8 at the small end to x1.4 at the record, x1000.
    inline std::uint32_t SizeFactorPermil(std::uint32_t sizeMm, std::uint32_t typMm, std::uint32_t maxMm)
    {
        std::int64_t const lo = static_cast<std::int64_t>(typMm) * 55 / 100;
        std::int64_t const hi = maxMm;
        if (hi <= lo)
            return 1000;
        std::int64_t t = (static_cast<std::int64_t>(sizeMm) - lo) * 1000 / (hi - lo);
        if (t < 0)
            t = 0;
        if (t > 1000)
            t = 1000;
        return static_cast<std::uint32_t>(800 + 600 * t / 1000);
    }

    // The tension safe band (fishing.md §4.4, §6.2): 350-650 of the bar, every 25 skill over the zone's base widens it
    // by 20 (2 % of the bar), at most 150.
    struct Band
    {
        std::uint32_t lo;
        std::uint32_t hi;
    };
    inline Band SafeBand(int skill, int zoneBase)
    {
        int over = skill - zoneBase;
        int widen = over > 0 ? (over / 25) * 20 : 0;
        if (widen > 150)
            widen = 150;
        return Band{static_cast<std::uint32_t>(350 - widen / 2), static_cast<std::uint32_t>(650 + widen / 2)};
    }

    // --- the clues (fishing.md §5) -----------------------------------------------------------------------------------
    // The hour index both sides share: the server's unix time / 3600.
    inline std::uint64_t HourIndex(std::uint64_t unixSeconds) { return unixSeconds / 3600; }

    // Per mille of a species' cells that show its clue this hour: activity / 200,000 (a common in its best habitat half
    // its cells, an uncommon 15 %, a rare 4 %), at most 900; the legendary's circle in all of its cells.
    inline std::uint32_t ClueShowPermil(Species const& s, std::uint64_t activity)
    {
        if (activity == 0 || s.clue == Clue::None)
            return 0;
        if (s.rarity == Rarity::Legendary)
            return 1000;
        std::uint64_t p = activity / 200000;
        if (p > 900)
            p = 900;
        if (p == 0)
            p = 1;
        return static_cast<std::uint32_t>(p);
    }

    // Whether the species' clue shows in this cell this hour: honest (only where it could bite now) and the same for
    // every client.
    inline bool ClueShows(Species const& s, std::uint64_t cellKey, std::uint64_t hourIndex, std::uint64_t activity)
    {
        std::uint32_t const p = ClueShowPermil(s, activity);
        if (p == 0)
            return false;
        return Mix(Mix(cellKey, s.id), hourIndex) % 1000 < p;
    }

    // Seconds between two clues of a showing cell (20-40 s, its own phase per cell and species).
    inline std::uint32_t ClueEverySeconds(std::uint64_t cellKey, std::uint32_t species)
    {
        return 20 + static_cast<std::uint32_t>(Mix(cellKey, species + 7777u) % 21);
    }
}

#endif
