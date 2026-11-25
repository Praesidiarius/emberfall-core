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

#ifndef EMBERFALL_PET_PRIORITY_H
#define EMBERFALL_PET_PRIORITY_H

// EMBERFALL: a companion's spells by a fixed priority (PATCHES.md #46; Emberfall docs/design/hunter-beasts.md §3).
// Upstream's PetAI autocasts the spells the player toggled on and picks among the ready ones at random, so a companion's
// damage depends on the player's toggles and hand casts. A module sets one list function (mod-emberfall's beasts: the
// spells of the pet's kind in the order of the world's ef_pet_priority rows, whatever the toggles, the taunt only while
// it is toggled on); the AI then walks that list instead of its autocast list and casts the FIRST spell that can go.
// Without a function set (the default), or when it answers false (other pets, stock data), one relaxed atomic load and
// upstream's behaviour. No core header: modules/mod-emberfall/tests/beasts_tests.cpp tests the pick on its own.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

class Unit;

namespace Emberfall::PetPriority
{
    // Fills `out` with the spells the pet AI tries this tick, best first; false: upstream's autocast list and random pick.
    using ListFn = bool (*)(Unit const* pet, std::vector<std::uint32_t>& out);

    inline std::atomic<ListFn>& Slot()
    {
        static std::atomic<ListFn> slot{ nullptr };
        return slot;
    }

    inline void Set(ListFn fn) { Slot().store(fn, std::memory_order_relaxed); }

    inline bool List(Unit const* pet, std::vector<std::uint32_t>& out)
    {
        out.clear();
        ListFn const fn = Slot().load(std::memory_order_relaxed);
        return fn && fn(pet, out);
    }

    // Which of the gathered candidates the AI casts: the first (the list's order) when ordered, else upstream's random
    // index `roll` (drawn by the caller in [0, count)).
    constexpr std::size_t Pick(bool ordered, std::size_t roll) { return ordered ? 0 : roll; }
}

#endif // EMBERFALL_PET_PRIORITY_H
