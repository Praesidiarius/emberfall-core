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

// Veins worked in swings (Emberfall feedback #622, docs/design/professions.md §7 c, §30), with no core types so
// tests/veins_tests.cpp runs it standalone.
//
// A chest whose template has a max_opens (data5, maxSuccessOpens: the 3.3.5 core reads neither, "pre 3.0 used for
// mining nodes") above one is a vein: each mining cast at it is one swing with its own loot (the loot template rolled
// again), and the vein stays until its last swing, then goes and respawns as any node. How many swings a fresh vein
// holds is rolled when its first swing ends, evenly between min_opens and max_opens.

#ifndef EMBERFALL_VEIN_SWINGS_LOGIC_H
#define EMBERFALL_VEIN_SWINGS_LOGIC_H

#include <cstdint>
#include <vector>

namespace Emberfall::VeinSwings
{
    // a chest of this max_opens is worked in swings
    constexpr bool IsVein(uint32_t maxOpens) { return maxOpens > 1; }

    // the swings a vein of (min_opens, max_opens) can hold: at least one, the top never under the bottom
    struct Range
    {
        uint32_t lo, hi;
    };
    constexpr Range Swings(uint32_t minOpens, uint32_t maxOpens)
    {
        uint32_t const lo = minOpens ? minOpens : 1;
        return {lo, maxOpens > lo ? maxOpens : lo};
    }

    // What a vein does when its loot state turns "just deactivated" (the core: the loot of a swing was taken and the
    // window closed, or the object is despawned for another reason):
    // - PassOn: not the end of a swing (no swing's loot was out: a despawn, a window on a loot already taken): the core
    //   goes on as for any chest
    // - Keep:   a swing ended and the vein has more: it is counted and the vein is ready for the next swing
    // - Spent:  the last swing ended: it is counted and the core despawns the vein as any looted node
    enum class Release
    {
        PassOn,
        Keep,
        Spent
    };
    // `armed`: a swing's loot was out (the vein was opened by a player since its last release); `swingsBefore`: the
    // swings counted before this one (the object's use count); `total`: the swings the vein holds
    constexpr Release AfterRelease(bool armed, uint32_t swingsBefore, uint32_t total)
    {
        if (!armed)
            return Release::PassOn;
        return swingsBefore + 1 >= total ? Release::Spent : Release::Keep;
    }

    // One loot slot, as far as the merge cares: the vein's template has the ore twice (the sure one and the chance of
    // a second, a reference to a table of the same ore), and a swing that rolls both shows one stack of two, not two
    // slots of one.
    struct Stack
    {
        uint32_t item = 0;
        int32_t property = 0;
        uint32_t count = 0;
        bool plain = true;    // no quest, free-for-all or condition flag: one player's ordinary item
        bool looted = false;
    };

    // Moves every plain, not yet looted stack onto the first stack of the same item and property while it stays at or
    // under `maxStack(item)` (the merged ones are marked looted); returns how many stacks were folded away.
    template <class MaxStack>
    uint32_t Merge(std::vector<Stack>& stacks, MaxStack maxStack)
    {
        uint32_t folded = 0;
        for (size_t i = 0; i < stacks.size(); ++i)
        {
            Stack& a = stacks[i];
            if (!a.plain || a.looted)
                continue;
            for (size_t j = i + 1; j < stacks.size(); ++j)
            {
                Stack& b = stacks[j];
                if (!b.plain || b.looted || b.item != a.item || b.property != a.property ||
                    a.count + b.count > maxStack(a.item))
                    continue;
                a.count += b.count;
                b.count = 0;
                b.looted = true;
                ++folded;
            }
        }
        return folded;
    }
}

#endif
