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

// Emberfall: the extra action button (Emberfall docs/contributing/quests.md "Moments: the extra action button"). Pure:
// which aura effects grant which spell. No core types, so tests/extraaction_tests.cpp runs it without a server.
//
// The protocol, server to client, is an aura and nothing else: a marker aura on the player whose effect is a dummy aura
// (SPELL_AURA_DUMMY, 4) with the misc value kMarker carries the granted spell as its trigger spell. A quest script grants
// it (SmartAI casts the marker on the players: content pipeline, quests/escorts.py `moment`) and takes it (removes the
// aura; the marker's duration ends it anyway). The Emberfall client reads the player's auras, finds a marker (Spell.dbc:
// the same three fields; client/src/data/ExtraAction.h) and shows the granted spell on its extra action button; casting
// it sends an ordinary CMSG_CAST_SPELL, which the core lets through for a spell the player holds this way (the core's
// EmberfallExtraAction.h, PATCHES.md #34). The spell itself does what it does (a dummy on the escortee: its SmartAI's
// spellhit row).

#ifndef EF_EXTRA_ACTION_LOGIC_H
#define EF_EXTRA_ACTION_LOGIC_H

#include <cstdint>

namespace Emberfall::ExtraAction
{
    // "EFEA": the misc value that makes a dummy aura an extra action marker. The same number in the client
    // (client/src/data/ExtraAction.h kExtraActionMarker) and the content pipeline (quests/escorts.py MARKER_MISC);
    // Emberfall's tools/py/tests/test_quest_moments.py checks the three agree.
    constexpr int32_t kMarker = 0x45464541;
    constexpr uint32_t kAuraDummy = 4; // SPELL_AURA_DUMMY

    // One aura effect the player carries (the core's AuraEffect: its aura type, misc value and its spell's trigger spell).
    struct MarkerEffect
    {
        uint32_t auraType = 0;
        int32_t miscValue = 0;
        uint32_t triggerSpell = 0;
    };

    // The spell an aura effect grants as an extra action (0: not a marker).
    constexpr uint32_t GrantedSpell(MarkerEffect const& e)
    {
        return e.auraType == kAuraDummy && e.miscValue == kMarker ? e.triggerSpell : 0;
    }

    // Whether any of the effects grants `spellId` (0 is never granted).
    template <class Effects>
    bool Grants(Effects const& effects, uint32_t spellId)
    {
        if (!spellId)
            return false;
        for (MarkerEffect const& e : effects)
            if (GrantedSpell(e) == spellId)
                return true;
        return false;
    }
}

#endif
