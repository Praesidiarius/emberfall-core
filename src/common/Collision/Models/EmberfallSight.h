/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
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

// EMBERFALL: line of sight by size (PATCHES.md #20). A new file, so it never conflicts with upstream.
//
// Which M2 models block line of sight. Upstream's blizzlike rule (ModelIgnoreFlags::M2, every spell's check) lets no
// M2 block at all, while an M2 game object blocks however small it is: on Emberfall's own data, whose trees, rocks and
// huts are M2s, a quest woodpile stopped a cast and a crag did not (feedback #34, 2026-10-23). The rule instead: an M2
// blocks when its collision, as placed, stands at least MinTop yards above its origin (its foot: taller than a body's
// eyes, so nothing is cast over it) and spans at least MinSpan yards across at its widest (not a post). The movement
// collision (mmaps, heights) keeps every M2. The same numbers are the Emberfall client's (client/src/world/Sight.h,
// the interact key) and its content checks' (tools/py/emberfall_content/sight.py, the doodad table test).

#ifndef EMBERFALL_SIGHT_H
#define EMBERFALL_SIGHT_H

#include <G3D/AABox.h>
#include <algorithm>

namespace VMAP::EmberfallSight
{
    constexpr float MinTop = 2.5f;
    constexpr float MinSpan = 0.85f;

    // `bound`: the model's collision box in model space (z up, the foot at 0); `scale`: its placement's.
    inline bool M2BlocksSight(G3D::AABox const& bound, float scale)
    {
        if (bound.isEmpty())
            return false;
        G3D::Vector3 const size = (bound.high() - bound.low()) * scale;
        return bound.high().z * scale >= MinTop && std::max(size.x, size.y) >= MinSpan;
    }
}

#endif
