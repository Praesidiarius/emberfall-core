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

// The arithmetic of the Pyromancer's DEV handoff (ankarna, 2026-11-15, revised 2026-11-20; Emberfall
// docs/design/classes/mage-pyromancer.md §16, §18) that needs no core: Undying Flame's cooldown cut for the Heat really
// spent (in steps of 10, the remainder carried), a tier read before the cost, the field's bonuses set when it is placed,
// the second field's own instance, Burning Chains' once per enemy, Eternal Ember's floor out of a fight, Falling Sun's
// "consumes all" and Endless Inferno's lengthening.
// src/emberfall_classkit_heat.cpp and src/classkit/EfSpecPower.cpp use it; tests/classkit_heat_tests.cpp
// (tests/run_tests.sh) checks it.

#ifndef EMBERFALL_HEAT_LOGIC_H
#define EMBERFALL_HEAT_LOGIC_H

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Emberfall::ClassKit::Heat
{
    // Undying Flame is the Barbarian's spend rule (spell_ef_kit_rule_spend_cut, EfLustLogic.h SpendCutMs) at -1000 ms a
    // point, counted in steps (the rule's misc B: 10, the handoff's "for every 10 Heat actually spent ... 10 seconds"):
    // only whole steps count, the rest is carried to the next spend (2026-11-20 [proposal], the handoff's open remainder
    // rule), so 137 Heat take 130 s and the 7 left over count with the next spend. A free cast in Overheat spent 0.
    struct StepCount
    {
        int32_t points = 0;   // the points that count now (whole steps)
        int32_t carry = 0;    // the remainder carried to the next spend
    };

    // `points` spent now with `carry` left over from before, counted in steps of `step` points (1 or less: every point,
    // nothing carried; the Barbarian's rule).
    constexpr StepCount StepPoints(int32_t points, int32_t carry, int32_t step)
    {
        int32_t const now = std::max(0, points);
        if (step <= 1)
            return { now, 0 };
        int32_t const total = std::max(0, carry) + now;
        return { total / step * step, total % step };
    }

    // What one cast really spends: its cost as paid (0 under Overheat's spell mod) plus what a "consumes all" spender
    // took beyond it (Falling Sun: the rest of the bar once its 100 were paid).
    constexpr int32_t PointsSpent(int32_t costPaid, int32_t extraTaken)
    {
        return std::max(0, costPaid) + std::max(0, extraTaken);
    }

    // Falling Sun's "consumes all": what is taken beyond the cost, the bar as it stands after the cost was paid; nothing
    // when the cast paid nothing (Overheat: Heat is neither consumed nor lost).
    constexpr int32_t SpendAllExtra(int32_t costPaid, int32_t leftAfterCost)
    {
        return costPaid > 0 ? std::max(0, leftAfterCost) : 0;
    }

    // A threshold is read before the cast's own cost (the handoff §1: Supernova, Sunpiercer, Solar Collapse): `before`
    // the bar as the cast began. The module's tier auras follow the bar on the player tick, after the cast, so a spell
    // reads exactly this.
    constexpr bool ReachedBeforeCost(int32_t before, int32_t at)
    {
        return at > 0 && before >= at;
    }

    // A field's bonuses, set when it is placed and kept for its life (Burning Sigil: Inferno Sigil's percent, Burning
    // Chains' root): whatever the bar does afterwards changes nothing.
    struct FieldSnapshot
    {
        int32_t pct = 0;                 // percent more on every tick
        std::vector<uint32_t> triggers;  // cast once at each enemy the field reaches
        std::vector<uint64_t> reached;   // the enemies a trigger already went to
    };

    // Whether the trigger goes to `enemy` now: the first time this field reaches it (Burning Chains: at most once per enemy
    // per sigil, also when it walks out and back in).
    inline bool FirstReach(FieldSnapshot& field, uint64_t enemy)
    {
        if (std::find(field.reached.begin(), field.reached.end(), enemy) != field.reached.end())
            return false;
        field.reached.push_back(enemy);
        return true;
    }

    // Which instance of the field's self aura a new field takes (each field makes its own Heat): 0 the first when it is
    // not running, else 1 the second when that is not running, else the one with less time left. (Twin Sigils' two
    // charges used it until 2026-11-20; with one charge, 25 s, and a sigil of at most 10 s the first is always free.)
    constexpr int32_t FieldInstance(bool firstRuns, int32_t firstLeftMs, bool secondRuns, int32_t secondLeftMs)
    {
        if (!firstRuns)
            return 0;
        if (!secondRuns)
            return 1;
        return secondLeftMs < firstLeftMs ? 1 : 0;
    }

    // Eternal Ember (corrected 2026-11-20, the handoff's "Heat at or below 70 never naturally decays outside combat"):
    // where natural cooling out of a fight leaves the bar (stored units): never below the floor, never above what it
    // holds: 20 stays 20, 50 stays 50, 70 stays 70, 100 and 150 cool to 70; without the rule (floor 0), to 0. Not a
    // fight-end condition: any Heat at or below the floor is kept, also Heat gained out of a fight.
    constexpr int32_t CooledTo(int32_t power, int32_t floor)
    {
        return floor > 0 ? std::clamp(power, 0, floor) : 0;
    }

    // The drain out of a fight that keeps the bar at or above the floor (above it, it cools at the normal rate down to it;
    // at or below it, nothing).
    constexpr int32_t DrainAboveFloor(int32_t power, int32_t drain, int32_t floor)
    {
        if (drain <= 0 || power <= floor)
            return 0;
        return std::min(drain, power - std::max(0, floor));
    }

    // Endless Inferno: a Sunlance channelled to its last tick while Overheat holds makes it `addMs` longer, no cap: the
    // time left and the aura's whole duration both grow.
    struct Lengthened
    {
        int32_t leftMs = 0;
        int32_t maxMs = 0;
    };

    constexpr Lengthened Lengthen(int32_t leftMs, int32_t maxMs, int32_t addMs)
    {
        if (addMs <= 0 || leftMs <= 0)
            return { leftMs, maxMs };
        return { leftMs + addMs, maxMs + addMs };
    }

    // Whether a channel's tick is its last (the channel ran to its end: Endless Inferno counts it).
    constexpr bool LastTick(uint32_t tick, int32_t totalTicks)
    {
        return totalTicks > 0 && tick >= uint32_t(totalTicks);
    }
}

#endif
