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

#ifndef EMBERFALL_EXTRA_ACTION_H
#define EMBERFALL_EXTRA_ACTION_H

// EMBERFALL: the extra action button (PATCHES.md #34; Emberfall docs/contributing/quests.md "Moments: the extra action
// button"). A quest moment gives a player one spell for a while: a marker aura carries it, and the Emberfall client shows
// the spell on a big button while the aura holds. The player never learned that spell, so CMSG_CAST_SPELL's spellbook
// check (WorldSession::HandleCastSpellOpcode) would drop the cast; it first asks one check, set by a module
// (mod-emberfall's extra action: the player carries a marker aura granting that spell). Without a check (the default)
// the site costs one relaxed atomic load and behaves as upstream.

#include <atomic>
#include <cstdint>

class Player;

namespace Emberfall::ExtraAction
{
    using Check = bool (*)(Player const* player, uint32_t spellId);

    inline std::atomic<Check>& CheckSlot()
    {
        static std::atomic<Check> slot{ nullptr };
        return slot;
    }

    inline void SetCheck(Check check) { CheckSlot().store(check, std::memory_order_relaxed); }

    // Whether `player` may cast `spellId` without knowing it: an extra action they hold now.
    inline bool Granted(Player const* player, uint32_t spellId)
    {
        Check const check = CheckSlot().load(std::memory_order_relaxed);
        return check && check(player, spellId);
    }
}

#endif
