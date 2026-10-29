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

#ifndef EMBERFALL_MISS_RULES_H
#define EMBERFALL_MISS_RULES_H

// EMBERFALL: the miss rules (PATCHES.md #26; Emberfall alpha1 feedback #142 and #146, docs/design/class-kit-pipeline.md
// "Misses"). On our own data (Emberfall.OwnData) a miss, dodge or parry is the player's loss, never a free try:
//   - a spender takes its whole cost (upstream 3.3.5 takes only 0 to a quarter of a rage, energy or runic power cost and
//     gives the runes back when the cast's target avoided it); combo points are spent the same;
//   - a strike (a spell with an effect aimed at anyone but its caster) gives its caster power (an energize effect on
//     the caster: Cross Cut's rage, Patient Shot's Focus) only when it struck someone; upstream gives it on a miss, at
//     nobody in reach, at empty air.
// The class kits' own builders and procs follow the same rules in mod-emberfall (src/classkit/EfClassKit.h, "the miss
// rules"). No core header: modules/mod-emberfall/tests/missrules_tests.cpp tests this file on its own. Without own data
// every rule answers as upstream.

#include <cstdint>

namespace Emberfall::MissRules
{
    // Whether a cast whose target avoided it pays less (upstream: a share of the cost, the runes back, the combo points
    // kept). On own data: never.
    constexpr bool PaysLessOnMiss(bool ownData)
    {
        return !ownData;
    }

    // Whether an energize effect on the caster gives its power. `playerSpell`: a player's cast; `strikesOthers`: the spell
    // has an effect aimed at someone other than its caster; `struck`: the units other than the caster the cast struck
    // (SPELL_MISS_NONE). A player's strike that struck nobody gives nothing on own data; a self spell (no other target:
    // a cooldown that gives power) always gives.
    constexpr bool EnergizesCaster(bool ownData, bool playerSpell, bool strikesOthers, uint32_t struck)
    {
        return !ownData || !playerSpell || !strikesOthers || struck > 0;
    }
}

#endif // EMBERFALL_MISS_RULES_H
