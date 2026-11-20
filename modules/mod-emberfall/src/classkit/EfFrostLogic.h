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

// The arithmetic of the Mage Frostguard's rework of 2026-11-15 (ankarna's implementation notes; Emberfall
// docs/design/classes/mage-frostguard.md §13) that needs no core: the Frost Shell's share of a blow and what a gain makes
// beyond its cap, Shattering Overflow's damage and its share over many enemies, the stacks under a cap a talent raises
// (Deep Winter), First Contact, Frostbound Orb's first enemy on its line and its flight, Twin Orbs' roll, and Stillpoint's
// plan for one enemy in the notes' order; since ankarna's AoE and threat hand-off of 2026-11-19 (§14) Shattering
// Overflow's one pulse an activation, Frigid Pulse's hit without damage and Echoing Frost's roll. src/classkit/EfClassKit.cpp,
// src/emberfall_classkit.cpp and src/emberfall_classkit_frost.cpp use it; tests/classkit_frost_tests.cpp
// (tests/run_tests.sh) checks it.

#ifndef EMBERFALL_FROST_LOGIC_H
#define EMBERFALL_FROST_LOGIC_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Emberfall::ClassKit::FrostLogic
{
    // What the shell takes of a blow of `damage` with `pool` left: `pct` percent of the blow (the rest passes to health),
    // never more than the pool. A share of 0 or less, or 100 and more, takes the whole blow as far as the pool goes (a
    // shell without the share: the rule before 2026-11-15).
    constexpr int64_t ShellTake(int64_t damage, int64_t pool, int32_t pct)
    {
        if (damage <= 0 || pool <= 0)
            return 0;
        int64_t const share = (pct <= 0 || pct >= 100) ? damage : damage * pct / 100;
        return std::min(share, pool);
    }

    // What a gain of `gain` stacks makes beyond the shell's cap (`maxStacks` x `perStack`) with `left` in the pool: the part
    // EfClassKitLogic.h ShellAfterGain cuts off; 0 when it fits.
    constexpr int64_t ShellOverflow(int64_t left, int64_t perStack, int32_t gain, int32_t maxStacks)
    {
        if (perStack <= 0 || maxStacks <= 0 || gain <= 0)
            return 0;
        int64_t const cap = perStack * maxStacks;
        int64_t const from = std::max<int64_t>(0, left);
        int64_t const now = from + perStack * gain;
        int64_t const room = std::max(cap, from);   // a pool above a cap that fell (less max health) keeps its amount
        return now > room ? now - room : 0;
    }

    // Shattering Overflow since ankarna's hand-off of 2026-11-19 (mage-frostguard.md §14): one ability activation sends at
    // most ONE pulse. The activation's shell gain is summed over every enemy it shattered first (Logic::ShellGain), the cap
    // is looked at once (ShellOverflow), and anything made beyond it sends one pulse however much was over, however many
    // enemies or stacks were shattered. Two activations, two pulses (no internal cooldown). (Before: `pct` % of the
    // amount over as the pulse's damage, 50 % and then 10 % since feedback #619.)
    constexpr int32_t OverflowPulses(int64_t overflow)
    {
        return overflow > 0 ? 1 : 0;
    }

    // An area burst's damage on each of `struck` enemies, soft-capped at `full`: the whole `amount` on each while at most
    // `full` are struck; with more, each takes amount x full / struck, so the total stays amount x full however many more
    // there are (feedback #619, 2026-11-17: Shattering Overflow's burst, full 5; amount x sqrt(full / struck) with full 1
    // before, whose total kept growing with the pack).
    inline int32_t AreaShare(int32_t amount, uint32_t struck, uint32_t full)
    {
        if (amount <= 0)
            return 0;
        uint32_t const k = std::max<uint32_t>(1, full);
        if (struck <= k)
            return amount;
        return int32_t(std::lround(double(amount) * double(k) / double(struck)));
    }

    // What one Shattering Overflow pulse deals to each of `struck` enemies: the burst's own amount (`amount`: its spell's
    // level and spell power numbers, never a share of the shell over the cap), shared out over a big pack as AreaShare
    // does (`full` enemies take it whole); nothing without a pulse. `overflow` only decides whether there is one.
    inline int32_t OverflowHit(int64_t overflow, int32_t amount, uint32_t struck, uint32_t full)
    {
        return OverflowPulses(overflow) ? AreaShare(amount, struck, full) : 0;
    }

    // Frigid Pulse since the hand-off of 2026-11-19: no damage; every enemy it strikes gets its own flat threat (the
    // engine's SPELL_EFFECT_THREAT: the effect's points by level, not shared between the enemies, without threat
    // modifiers, so the spec's x 3.5 does not touch it) and its stacks: `baseStacks` (1) + Gathering Frost's
    // `extraStacks` (0 / 1 / 2).
    struct PulseHit
    {
        int32_t damage;
        int32_t threat;
        int32_t stacks;
    };

    constexpr PulseHit FrigidPulseHit(int32_t threatPoints, int32_t baseStacks, int32_t extraStacks)
    {
        return PulseHit{ 0, std::max(0, threatPoints), std::max(0, baseStacks) + std::max(0, extraStacks) };
    }

    // Echoing Frost: one roll a cast, never one an enemy: with the talent, `roll` (0-99) below `chance` percent sends the
    // second pulse.
    constexpr bool EchoPulse(bool talent, int32_t chance, int32_t roll)
    {
        return talent && chance > 0 && roll < chance;
    }

    // The second pulse's hit on each enemy: exactly `echoStacks` (1), whatever the cast's own stacks were (Gathering
    // Frost does not touch it); no damage, no threat.
    constexpr PulseHit EchoHit(int32_t echoStacks)
    {
        return PulseHit{ 0, 0, std::max(0, echoStacks) };
    }

    // The stacks on an enemy after adding `add` under the cap `cap` (Deep Winter raises the Frost Stacks' 6 to 8): never
    // above the cap, and stacks above a cap that fell stay (nothing is taken).
    constexpr uint32_t StacksAfterAdd(uint32_t have, int32_t add, uint32_t cap)
    {
        if (add <= 0 || have >= cap)
            return have;
        return std::min<uint32_t>(cap, have + uint32_t(add));
    }

    // First Contact: the damage of a hit on an enemy the mage had not hurt before the cast (`fresh`) `pct` percent more.
    constexpr int32_t FirstContactDamage(int32_t damage, bool fresh, int32_t pct)
    {
        if (!fresh || pct <= 0 || damage <= 0)
            return damage;
        return int32_t(std::min<int64_t>(int64_t(damage) * (100 + pct) / 100, INT32_MAX));
    }

    // An enemy seen from the caster along the orb's line: `along` yards ahead, `across` yards to the side, `reach` its
    // combat reach.
    struct LineCandidate
    {
        float along;
        float across;
        float reach;
    };

    // Frostbound Orb: the enemy the orb stops at, the first it meets on its line (`length` yards ahead, `half` yards to each
    // side, an enemy's reach counted): the one nearest along the line; -1: none (the orb flies its whole length).
    inline int FirstOnLine(std::vector<LineCandidate> const& candidates, float half, float length)
    {
        int best = -1;
        for (int i = 0; i < int(candidates.size()); ++i)
        {
            LineCandidate const& c = candidates[i];
            if (c.along < -c.reach || c.along > length + c.reach || std::fabs(c.across) > half + c.reach)
                continue;
            if (best < 0 || c.along < candidates[best].along)
                best = i;
        }
        return best;
    }

    // The orb's flight in ms to a spot `dist` yards away at `speed` yards a second (its missile: 9).
    constexpr int32_t FlightMs(float dist, float speed)
    {
        if (speed <= 0.0f || dist <= 0.0f)
            return 0;
        return int32_t(dist / speed * 1000.0f);
    }

    // Twin Orbs: whether a cast sends a second orb: only the pressed cast rolls (a second orb never summons another), with
    // the talent known, `roll` (0-99) below `chance` percent.
    constexpr bool TwinOrb(bool isTwin, bool talent, int32_t chance, int32_t roll)
    {
        return !isTwin && talent && chance > 0 && roll < chance;
    }

    // Twin Orbs: the spell the second orb is cast with: its own (`twin`, the orb's twin under another id), never the first
    // orb's spell: one caster's periodic ground auras of one spell refresh each other on an enemy instead of stacking
    // (Aura::CanStackWith), so the first orb's spell again strikes nobody the first already does (live, 2026-11-17: two
    // orbs over two enemies, 9-10 ticks each in 10 s, as one orb). 0: no second orb (no twin spell in the data).
    constexpr uint32_t TwinOrbSpell(uint32_t orb, uint32_t twin)
    {
        return twin && twin != orb ? twin : 0;
    }

    // Stillpoint on one enemy, in the notes' order (§6): 1. its Frost Stacks are read once, before anything changes;
    // 2. frozen when it has at least `need` and is neither a boss nor immune to stuns (a freeze is a stun on creatures);
    // 3. the Glacial Rupture mark on every enemy struck, bosses too; 4. Frost Accumulation's `gain` stacks (with the
    // talent) only on an enemy that had fewer than `need` at step 1.
    struct StillPlan
    {
        bool freeze;
        bool mark;
        int32_t addStacks;
    };

    constexpr StillPlan Stillpoint(uint32_t stacks, uint32_t need, bool boss, bool stunImmune, bool accumulation, int32_t gain)
    {
        bool const enough = stacks >= std::max<uint32_t>(1, need);
        return StillPlan{ enough && !boss && !stunImmune, true, (!enough && accumulation && gain > 0) ? gain : 0 };
    }
}

#endif
