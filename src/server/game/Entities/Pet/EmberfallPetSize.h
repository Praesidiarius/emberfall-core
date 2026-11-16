/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef EMBERFALL_PET_SIZE_H
#define EMBERFALL_PET_SIZE_H

// EMBERFALL: the hunter companion's size (PATCHES.md #41; Emberfall docs/design/hunter-taming.md §18, feedback #571 and
// #572). Upstream sizes a hunter pet by CreatureFamily.dbc's min/max scale over its level (Pet::GetNativeObjectScale), the
// same for every beast of a family whatever its model, and gives it a combat reach of 1.5 yd and a bounding radius of 1
// yd times that scale (Creature::SetObjectScale). Emberfall sizes a companion by its kind and its growth instead: a
// module sets one size function (mod-emberfall's pet size: the object scale that draws the pet's display at its kind's
// size for its stage) and one reach function (the reach and radius that follow the size within limits). Every path that
// scales a pet (its creation, a level up, a scale aura coming or going, a transform ending) goes through these two, so
// the size holds. Without a function set (the default) both sites cost one relaxed atomic load and behave as upstream.

#include <atomic>

class Pet;

namespace Emberfall::PetSize
{
    // The pet's native object scale; false: upstream's.
    using ScaleFn = bool (*)(Pet const* pet, float& scale);
    // The pet's combat reach and bounding radius at object scale `scale`; false: upstream's.
    using ReachFn = bool (*)(Pet const* pet, float scale, float& reach, float& radius);

    inline std::atomic<ScaleFn>& ScaleSlot()
    {
        static std::atomic<ScaleFn> slot{ nullptr };
        return slot;
    }

    inline std::atomic<ReachFn>& ReachSlot()
    {
        static std::atomic<ReachFn> slot{ nullptr };
        return slot;
    }

    inline void Set(ScaleFn scale, ReachFn reach)
    {
        ScaleSlot().store(scale, std::memory_order_relaxed);
        ReachSlot().store(reach, std::memory_order_relaxed);
    }

    inline bool Scale(Pet const* pet, float& scale)
    {
        ScaleFn const fn = ScaleSlot().load(std::memory_order_relaxed);
        return fn && fn(pet, scale);
    }

    inline bool Reach(Pet const* pet, float scale, float& reach, float& radius)
    {
        ReachFn const fn = ReachSlot().load(std::memory_order_relaxed);
        return fn && fn(pet, scale, reach, radius);
    }
}

#endif
