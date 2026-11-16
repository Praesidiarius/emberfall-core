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

#ifndef EMBERFALL_PET_SIZE_LOGIC_H
#define EMBERFALL_PET_SIZE_LOGIC_H

// The companion's size and growth (Emberfall docs/design/hunter-taming.md §18; feedback #571, #572), pure rules, no core
// types (tests/petsize_tests.cpp):
//
//   - A companion's size comes from its kind (CreatureFamily: hound, lynx, boar, hawk, lizard), not from the wild beast
//     it was: one full size per kind in yards (ef_pet_size_kind), the "side-on size" sqrt(height x length) of the model's
//     rest pose; every display a tamed beast may wear has its own side-on size at object scale 1 (ef_pet_size_display), so
//     the object scale is the wanted size over the display's (ScaleFor).
//   - It grows in stages (ef_pet_size_stage): stage n is reached after `seconds` out with its hunter (counted while the
//     companion is out and the hunter online, per pet: the characters DB's ef_character_pet_growth) and stands at
//     `share` of its kind's full size; the last stage is the full size.
//   - Growth is looks only: its combat reach and bounding radius follow the size within the rule's limits (ReachFor,
//     RadiusFor), so a big companion can still be hit and hit back from where it stands.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Emberfall::PetSize
{
    struct Stage
    {
        std::uint32_t seconds = 0;     // out with its hunter to reach it (the first stage: 0)
        std::uint32_t sharePermil = 1000;   // of the kind's full size
    };

    struct Rule
    {
        std::vector<Stage> stages;     // ascending seconds; the first at 0, the last at 1000 permil
        float reachPerYard = 1.0f, reachMin = 1.5f, reachMax = 3.0f;     // combat reach: the size x this, within
        float radiusPerYard = 0.4f, radiusMin = 0.3f, radiusMax = 1.2f;  // bounding radius: the size x this, within
        float scaleMin = 0.25f, scaleMax = 4.0f;                         // the object scale's limits
    };

    // The stages make sense: at least one, the first at 0 s, seconds rising, shares in (0, 2].
    inline bool Valid(Rule const& r)
    {
        if (r.stages.empty() || r.stages.front().seconds != 0)
            return false;
        for (std::size_t i = 0; i < r.stages.size(); ++i)
        {
            if (r.stages[i].sharePermil == 0 || r.stages[i].sharePermil > 2000)
                return false;
            if (i && r.stages[i].seconds <= r.stages[i - 1].seconds)
                return false;
        }
        return r.reachMin > 0.0f && r.reachMin <= r.reachMax && r.radiusMin > 0.0f && r.radiusMin <= r.radiusMax &&
               r.scaleMin > 0.0f && r.scaleMin <= r.scaleMax;
    }

    // The stage (0-based) a companion out `seconds` stands at.
    inline std::uint32_t StageOf(Rule const& r, std::uint32_t seconds)
    {
        std::uint32_t stage = 0;
        for (std::size_t i = 1; i < r.stages.size(); ++i)
            if (seconds >= r.stages[i].seconds)
                stage = std::uint32_t(i);
        return stage;
    }

    // Seconds out still needed for the next stage; 0 at the last.
    inline std::uint32_t SecondsToNext(Rule const& r, std::uint32_t seconds)
    {
        std::uint32_t const stage = StageOf(r, seconds);
        if (stage + 1 >= r.stages.size())
            return 0;
        return r.stages[stage + 1].seconds - seconds;
    }

    // Whether `before` -> `after` seconds crosses into a new stage (the chat line, the new size).
    inline bool Grew(Rule const& r, std::uint32_t before, std::uint32_t after)
    {
        return StageOf(r, after) > StageOf(r, before);
    }

    // The seconds that put a companion at `stage` (the GM command; past the last: the last).
    inline std::uint32_t SecondsFor(Rule const& r, std::uint32_t stage)
    {
        if (r.stages.empty())
            return 0;
        return r.stages[std::min<std::size_t>(stage, r.stages.size() - 1)].seconds;
    }

    // The side-on size in yards of a companion of a kind of `kindFull` yards at `stage`.
    inline float SizeOf(Rule const& r, float kindFull, std::uint32_t stage)
    {
        if (r.stages.empty())
            return kindFull;
        Stage const& s = r.stages[std::min<std::size_t>(stage, r.stages.size() - 1)];
        return kindFull * float(s.sharePermil) / 1000.0f;
    }

    // The object scale that draws a display of side-on size `displayBase` (at scale 1) at `size` yards.
    inline float ScaleFor(Rule const& r, float size, float displayBase)
    {
        if (!(displayBase > 0.0f) || !(size > 0.0f))
            return 1.0f;
        return std::clamp(size / displayBase, r.scaleMin, r.scaleMax);
    }

    // Combat reach and bounding radius for a companion drawn `size` yards (its display's side-on size x its scale).
    inline float ReachFor(Rule const& r, float size) { return std::clamp(size * r.reachPerYard, r.reachMin, r.reachMax); }
    inline float RadiusFor(Rule const& r, float size) { return std::clamp(size * r.radiusPerYard, r.radiusMin, r.radiusMax); }
}

#endif
