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

#ifndef EMBERFALL_CAST_WHILE_MOVING_H
#define EMBERFALL_CAST_WHILE_MOVING_H

// EMBERFALL: casting on the move (PATCHES.md #25; Emberfall docs/design/classes/druid-stormbringer.md §13): 3.3.5a has no
// aura that lets a player cast a spell with a cast time while moving. Spell::prepare refuses such a cast to a moving
// player (SPELL_FAILED_MOVING) and Spell::update cancels one when the player moves; both first ask one check, set by a
// module (mod-emberfall's class kits: the Druid's Windborne while Windstride holds). Without a check (the default) each
// site costs one relaxed atomic load and behaves as upstream.

#include <atomic>

class SpellInfo;
class Unit;

namespace Emberfall::CastWhileMoving
{
    using Check = bool (*)(Unit const* caster, SpellInfo const* spell);

    inline std::atomic<Check>& CheckSlot()
    {
        static std::atomic<Check> slot{ nullptr };
        return slot;
    }

    inline void SetCheck(Check check) { CheckSlot().store(check, std::memory_order_relaxed); }

    // Whether `caster` may cast `spell` (a cast time or a channel that movement interrupts) while moving.
    inline bool Allowed(Unit const* caster, SpellInfo const* spell)
    {
        Check const check = CheckSlot().load(std::memory_order_relaxed);
        return check && check(caster, spell);
    }
}

#endif
