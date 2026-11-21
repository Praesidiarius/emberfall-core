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

#pragma once

// Telegraphed casts on the server (Emberfall docs/design/mob-abilities.md §2, §13): which spells a player sees a marker
// for (the client's own table EfSpellTelegraph.dbc, which the server's dbc folder carries too: read once, at the first
// use), whether a running cast is one the avoidable rule says can be stepped out of, and the area it will hit. One
// reading for the companions (EfCompanionAI.cpp) and the bots (mod-playerbots' src/Emberfall/EmberfallTelegraphs.cpp);
// the geometry is EfTelegraphLogic.h.

#include "Define.h"
#include "EfTelegraphLogic.h"

class Spell;
class SpellInfo;
class Unit;

namespace Emberfall::Telegraphs
{
    // An EfSpellTelegraph row (dbc/tables.py): 1 cone, 2 circle round the caster, 3 circle at the target or point, 4 line.
    struct Marker
    {
        uint32 shape = 0;
        float angle = 0.0f;   // cone: degrees
        float width = 0.0f;   // line: yards
    };

    // The spell's marker, or null (no row, or no table: stock data).
    Marker const* MarkerOf(uint32 spellId);
    // How many spells have a marker and direct damage (the avoidable rule's data; 0 without the table).
    uint32 AvoidableSpells();
    // The rule's data flags for a spell: a marker and a school_damage effect (worked out once per spell at the load).
    bool TelegraphedWithDamage(uint32 spellId);

    // The rule's facts of a running cast (EfTelegraphLogic.h CastFacts); the cheap ones first, the table only for a
    // creature's untriggered cast of 1.5 s or more that is still preparing.
    CastFacts FactsOf(Spell const* spell);
    // The rule (EfTelegraphLogic.h Avoidable) for a running cast.
    inline bool IsAvoidable(Spell const* spell) { return Avoidable(FactsOf(spell)); }

    // The area a hostile's cast will hit (mob-abilities.md: a cone ahead of it, a circle round it, a circle at a point,
    // a line), with the core's own leeway: the effect radius plus the caster's size (a cone, a line) and `victimReach`,
    // the size of whoever asks. False for casts with no such area, or one wider than `maxRadius` (a room-wide pulse is
    // for interrupting, not dodging).
    bool DangerOf(Unit const* caster, Spell const* spell, float victimReach, float maxRadius, Danger& d);
}
