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

// Emberfall: one per-viewer visibility filter for creatures (WorldObject::CanSeeOrDetect). mod-emberfall's world editor
// (Emberfall docs/design/world-editor.md §19) sets it on the normal realm: a release spawn that a contributor's draft
// removes is hidden from the editors whose editor is open, and stays for every other player until the draft is
// accepted. Null (the default, and whenever the editor is off) costs one pointer test.

#ifndef EMBERFALL_VISIBILITY_H
#define EMBERFALL_VISIBILITY_H

class Creature;
class Player;

namespace Emberfall::Visibility
{
    // true: the viewer does not see the creature
    using CreatureFilter = bool (*)(Creature const* creature, Player const* viewer);

    inline CreatureFilter s_creatureFilter = nullptr;

    inline void SetCreatureFilter(CreatureFilter filter) { s_creatureFilter = filter; }

    inline bool HiddenFrom(Creature const* creature, Player const* viewer)
    {
        return s_creatureFilter && s_creatureFilter(creature, viewer);
    }
}

#endif
