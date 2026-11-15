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

// The module's users of the core's one per-viewer creature filter (src/server/game/Entities/Object/EmberfallVisibility.h):
// the world editor's drafts (editor/EfEditor.cpp) and the escorts' copies (emberfall_escorts.cpp). Each sets its own slot;
// the core gets one function that asks both (or none while both are off, which costs the core one pointer test).

#ifndef EF_CREATURE_VISIBILITY_H
#define EF_CREATURE_VISIBILITY_H

#include "EmberfallVisibility.h"

namespace Emberfall::ModuleVisibility
{
    inline Emberfall::Visibility::CreatureFilter s_editor = nullptr;
    inline Emberfall::Visibility::CreatureFilter s_escorts = nullptr;

    inline bool Either(Creature const* creature, Player const* viewer)
    {
        return (s_editor && s_editor(creature, viewer)) || (s_escorts && s_escorts(creature, viewer));
    }

    inline void Install()
    {
        Emberfall::Visibility::SetCreatureFilter(s_editor || s_escorts ? &Either : nullptr);
    }

    inline void SetEditor(Emberfall::Visibility::CreatureFilter filter) { s_editor = filter; Install(); }
    inline void SetEscorts(Emberfall::Visibility::CreatureFilter filter) { s_escorts = filter; Install(); }
}

#endif
