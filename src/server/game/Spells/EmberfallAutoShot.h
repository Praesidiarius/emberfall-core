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

#ifndef EMBERFALL_AUTO_SHOT_H
#define EMBERFALL_AUTO_SHOT_H

// EMBERFALL: the Hunter's auto shot on the move (PATCHES.md #42; Emberfall alpha1 feedback #650, 2026-11-18, and
// docs/design/classes/hunter-*.md Q15: hunters kite). Upstream refuses every auto-repeat spell to a moving player in
// Spell::CheckCast (SPELL_FAILED_MOVING), and Unit::_UpdateAutoRepeatSpell asks that check before each shot: the
// hunter's auto shot (spell 75, HUNTER_AUTOSHOOT there, which movement already does not cancel) held its fire for as
// long as the hunter walked, ran, strafed or jumped. On our own data (Emberfall.OwnData) exactly this spell shoots on
// the move, at its normal speed; a wand's Shoot and every spell a seat interrupts are refused as upstream. Casts with a
// cast time are not judged here: Spell::prepare and Spell::update plant them by their movement interrupt flag (and
// #25's check) as before, and while one runs the auto shot waits (_UpdateAutoRepeatSpell's IsNonMeleeSpellCast), as
// upstream. No core header: modules/mod-emberfall/tests/autoshot_tests.cpp tests this file on its own. Without own
// data the rule answers as upstream.

#include <cstdint>

namespace Emberfall::AutoShot
{
    // The Hunter's auto shot (Unit::_UpdateAutoRepeatSpell's HUNTER_AUTOSHOOT; Emberfall's Loose).
    constexpr uint32_t kSpellId = 75;

    // Whether Spell::CheckCast refuses this spell to a moving player (SPELL_FAILED_MOVING). Upstream: an auto-repeat
    // spell, or one a seat interrupts (AURA_INTERRUPT_FLAG_NOT_SEATED). On own data the hunter's auto shot is not.
    constexpr bool MovingRefuses(bool ownData, uint32_t spellId, bool autoRepeat, bool interruptedBySeat)
    {
        if (ownData && autoRepeat && spellId == kSpellId && !interruptedBySeat)
            return false;
        return autoRepeat || interruptedBySeat;
    }
}

#endif // EMBERFALL_AUTO_SHOT_H
