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

// The escort rule (Emberfall feedback #564, #567; docs/contributing/quests.md "Escorts"), with no core types so that
// tests/escort_tests.cpp checks it on its own. An escort is its taker's: the escortee's spawn never leaves its place;
// taking the quest (or speaking to them with the walk still to do) summons a copy of them for the taker, and that copy
// walks the path (its SmartAI: quests/escorts.py). Who sees what:
//   - the copy: its summoner and the summoner's group, each only while the quest is theirs (taken, done, or handed in
//     to the copy at the walk's end); nobody else, so nobody is on an escort without the quest and a stranger never
//     meets a walking one;
//   - the spawn at home: everyone, except a player whose own (or whose group's) copy is walking while their quest is
//     still to do: they see the copy instead of a second one standing at the start. Once the walk is done (or the copy
//     gone) they see the spawn again (a zone's `seen` rows may hide it then: quest-phased NPCs, conditions table);
//   - one copy per group: a player whose copy or whose group's copy is out starts no second one.

#ifndef EF_ESCORT_LOGIC_H
#define EF_ESCORT_LOGIC_H

#include <cstdint>

namespace Emberfall::Escort
{
    // A player's state of the escort's quest (QuestStatus with the rewarded flag folded in).
    enum class QuestState : uint8_t
    {
        None,        // not in the log (never taken, abandoned)
        ToDo,        // in the log, the walk not done
        Done,        // in the log, the walk done (complete, not handed in)
        Rewarded,    // handed in
    };

    // Who a copy's summoner is to the viewer.
    enum class Relation : uint8_t
    {
        Stranger,    // neither the summoner nor in their group
        Summoner,    // the viewer took the quest and the copy is theirs
        Group,       // in the summoner's group
    };

    // Does the viewer see a walking copy? Its own players, from taking the quest until the walk is handed in; an
    // escortee who takes the quest back (`takesBack`) is handed it where the walk ends and stays a little, so they do
    // not vanish at the hand-in. Otherwise the hand-in is elsewhere and the escortee's next place is a quest-phased
    // spawn of the zone's (npcs.yaml `seen`), so the walk goes.
    constexpr bool SeesCopy(Relation who, QuestState state, bool takesBack = false)
    {
        return who != Relation::Stranger && state != QuestState::None && (state != QuestState::Rewarded || takesBack);
    }

    // Is the spawn at home hidden from the viewer? `ownCopyOut`: a copy of this escortee walks for the viewer or their
    // group (Relation Summoner or Group to one of its copies).
    constexpr bool HidesHome(bool ownCopyOut, QuestState state)
    {
        return ownCopyOut && state == QuestState::ToDo;
    }

    // May the viewer start a walk (summon a copy)? Only with the walk to do and no copy of theirs or their group's out.
    constexpr bool MayStart(bool ownCopyOut, QuestState state)
    {
        return !ownCopyOut && state == QuestState::ToDo;
    }

    // The copy's lifetime as a summon (ms): it ends its own walk (the end, a give-up, a death), this is only the net
    // for a copy whose players all left (a give-up needs a viewer gone, and nobody sees it then).
    constexpr uint32_t kCopyLifetimeMs = 45u * 60u * 1000u;
}

#endif
