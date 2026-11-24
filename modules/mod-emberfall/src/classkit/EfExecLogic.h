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

// The Warrior Executioner v4.0's queued swings as pure logic (Emberfall docs/design/classes/warrior-executioner.md §21;
// ankarna's "Executioner v4.0" of 2026-11-22): no core types, so tests/classkit_exec_tests.cpp runs them without a
// server. src/emberfall_classkit_exec.cpp feeds them the server's swing timer and the casts.
//
// The engine carries most of it (an on-next-swing spell, SPELL_ATTR0_ON_NEXT_SWING_NO_DAMAGE: the core's
// CURRENT_MELEE_SPELL slot, one at a time, cast by the main hand's swing, cost and cooldown at that cast, a cancel free).
// What it lacks, and these rules add:
//   - the sweet spot: a queue that arrives while the main hand's running swing is between a window's start and end
//     (percent of its time, both ends in) gets its talent's bonus, decided once, when the queue arrives (SweetSpot); a
//     talent may carry several windows (one rule effect each: the Executioner's 60-80 % and, feedback #717-#720's
//     #718, the swing's last 1 %, 99-100 %, which a swing that ran out or none running is in: the first swing of a
//     fight), its bonus given once when any of them holds (AnyWindow);
//   - a second press of the swing that is queued does nothing: the first stays with its sweet-spot result (RefusesPress);
//   - any other ability of the kit the player uses cancels the queued swing, unless the rule names it (Killing Instinct;
//     CancelsQueue).

#ifndef EMBERFALL_CLASSKIT_EXEC_LOGIC_H
#define EMBERFALL_CLASSKIT_EXEC_LOGIC_H

#include <cstdint>

namespace Emberfall::ClassKit::Exec
{
    // How far the main hand's running swing is, in percent of its time: the core's attack timer counts down from the
    // swing's time (UNIT_FIELD_BASEATTACKTIME, haste in: a haste change rescales the rest, Unit::ApplyAttackTimePercentMod)
    // to 0, where the swing is due. A timer at 0 (the swing ran out and waits for the attack) or a swing of no time: 100.
    inline float SwingProgressPct(int32_t timerLeftMs, float swingMs)
    {
        if (swingMs <= 0.0f || timerLeftMs <= 0)
            return 100.0f;
        float const left = float(timerLeftMs) >= swingMs ? swingMs : float(timerLeftMs);
        return 100.0f * (1.0f - left / swingMs);
    }

    // The sweet spot: [startPct, endPct] of the swing, both ends in (ankarna: "60-80 %"). An empty window never holds.
    inline bool InWindow(float progressPct, int32_t startPct, int32_t endPct)
    {
        if (startPct >= endPct)
            return false;
        return progressPct >= float(startPct) && progressPct <= float(endPct);
    }

    inline bool SweetSpot(int32_t timerLeftMs, float swingMs, int32_t startPct, int32_t endPct)
    {
        return InWindow(SwingProgressPct(timerLeftMs, swingMs), startPct, endPct);
    }

    // A talent's windows ({start, end} pairs, its rule effects that share one caught buff): whether any of them holds.
    struct Window
    {
        int32_t startPct = 0;
        int32_t endPct = 0;
    };

    template <typename Windows>
    inline bool AnyWindow(int32_t timerLeftMs, float swingMs, Windows const& windows)
    {
        float const progress = SwingProgressPct(timerLeftMs, swingMs);
        for (Window const& w : windows)
            if (InWindow(progress, w.startPct, w.endPct))
                return true;
        return false;
    }

    // A press of a queued swing (`pressedSpell`) while the player has `queuedSpell` waiting (0: none): refused when it is
    // the same spell in another cast (pressed again: nothing happens, the first press and its sweet spot stay); another
    // queued swing replaces it (the engine's rule), and the queued cast itself (the swing casting it) is never refused.
    inline bool RefusesPress(uint32_t queuedSpell, uint32_t pressedSpell, bool sameCast)
    {
        return queuedSpell != 0 && queuedSpell == pressedSpell && !sameCast;
    }

    // Whether a cast of the player's cancels the queued swing: an ability the player uses (not a triggered spell, not a
    // passive, not another queued swing: that replaces it), of the kit's own family (the class's abilities: no item, no
    // mount), that the rule does not name (`exempt`: Killing Instinct). Nothing to cancel without a queued swing.
    struct CastFacts
    {
        bool queued = false;        // a swing waits (CURRENT_MELEE_SPELL)
        bool triggered = false;     // cast by a script or a proc
        bool passive = false;
        bool nextSwing = false;     // the cast is itself a queued swing (a new queue, or the swing casting it)
        bool kitFamily = false;     // the cast's spell family is the rule's (the class kit's)
        bool exempt = false;        // the rule names it
    };

    inline bool CancelsQueue(CastFacts const& f)
    {
        return f.queued && !f.triggered && !f.passive && !f.nextSwing && f.kitFamily && !f.exempt;
    }
}

#endif
