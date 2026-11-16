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
// plan for one enemy in the notes' order. src/classkit/EfClassKit.cpp, src/emberfall_classkit.cpp and
// src/emberfall_classkit_frost.cpp use it; tests/classkit_frost_tests.cpp (tests/run_tests.sh) checks it.

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

    // Shattering Overflow: `pct` percent of the shell made beyond the cap becomes frost damage (the notes: 50 %).
    constexpr int32_t OverflowDamage(int64_t overflow, int32_t pct)
    {
        if (overflow <= 0 || pct <= 0)
            return 0;
        return int32_t(std::min<int64_t>(overflow * pct / 100, INT32_MAX));
    }

    // An area burst's damage on each of `struck` enemies: the whole `amount` on each while at most `full` are struck; with
    // more, each takes amount x sqrt(full / struck) (the total still grows with the pack, each enemy's share falls)
    // [proposal: full 1, Shattering Overflow's burst].
    inline int32_t AreaShare(int32_t amount, uint32_t struck, uint32_t full)
    {
        if (amount <= 0)
            return 0;
        uint32_t const k = std::max<uint32_t>(1, full);
        if (struck <= k)
            return amount;
        return int32_t(std::lround(double(amount) * std::sqrt(double(k) / double(struck))));
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
