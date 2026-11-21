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

// Emberfall class kits: the Hunter Ranger's v1.0 rules without the core (Emberfall docs/design/classes/hunter-ranger.md,
// ankarna's and sonaetiger's "Developer Handoff v1.0" of 2026-11-19, and its "Rework Update" of 2026-11-20: the delayed
// out-of-combat reset, Quick Nock), so tests/classkit_ranger_tests.cpp can check them against the hand-off's acceptance tests. src/emberfall_classkit_ranger.cpp uses them. Generic: the numbers come from
// the spells' own effects.

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Emberfall::ClassKit::Ranger
{
    // ---- the stance rest (spell_ef_kit_stance_rest) ----

    // The Rework Update of 2026-11-20: the reset waits this long out of a fight (3 uninterrupted seconds).
    constexpr int32_t kRestDelayMs = 3000;

    // One tick of the stance rest: what to change on the owner. Out of a fight, once `outMs` reaches `delayMs`, the other
    // stance comes off, the resting stance goes on and the spec power is filled (every fight starts in Falcon's Eye with
    // 100 Energy); before that the stance and the Energy stay as they are. With no stance at all (a login, a death that
    // took Bear's Claw) the resting one comes at once, out of a fight with the Energy too, as before the delay. In a fight
    // only a missing stance is mended (the resting one, when neither holds), never the Energy.
    struct RestAction
    {
        bool addRest = false;
        bool removeOther = false;
        bool fill = false;
    };

    constexpr RestAction Rest(bool inCombat, int32_t outMs, bool hasRest, bool hasOther, int32_t power, int32_t maxPower,
                              int32_t delayMs = kRestDelayMs)
    {
        if (inCombat)
            return { !hasRest && !hasOther, false, false };
        if (!hasRest && !hasOther)
            return { true, false, power < maxPower };
        if (outMs < delayMs)
            return {};
        return { !hasRest, hasOther, power < maxPower };
    }

    // The rest's clock: how long out of a fight, by the game clock. `sinceMs` is when the rest first saw the owner out of
    // the fight (0: in a fight, or not seen out of one yet); the first tick out of a fight starts it. A fight resets it.
    struct RestClock
    {
        uint64_t sinceMs = 0;
        int32_t outMs = 0;
    };

    constexpr RestClock RestTick(uint64_t sinceMs, bool inCombat, uint64_t nowMs)
    {
        if (inCombat)
            return { 0, 0 };
        uint64_t const since = sinceMs ? sinceMs : (nowMs ? nowMs : 1);
        uint64_t const out = nowMs > since ? nowMs - since : 0;
        return { since, int32_t(std::min<uint64_t>(out, uint64_t(INT32_MAX))) };
    }

    // ---- Quick Nock (spell_ef_kit_spent_at_start) ----

    // A buff that shortens the next cast of the spells it names (its cast-time modifier: the engine applies it as the cast
    // begins) is spent as that cast BEGINS: an interrupted or cancelled cast has spent it too. A cast of a spell it does not
    // name, or a triggered cast (never the player's own), leaves it.
    constexpr bool SpentAtStart(bool held, bool names, bool triggered)
    {
        return held && names && !triggered;
    }

    // The cast it leaves: `baseMs` less `cutMs`, never below 0 (Falcon Shot 1.5 s - 0.5 s = 1.0 s).
    constexpr int32_t ShortenedCastMs(int32_t baseMs, int32_t cutMs)
    {
        return std::max<int32_t>(0, baseMs - std::max<int32_t>(0, cutMs));
    }

    // ---- Energy: the forced loop ----

    // A transition (Savage Leap, Retreat) is open only at exactly 0 (the spec power's tier at 0 keeps the gate aura).
    constexpr bool TransitionOpen(int32_t points)
    {
        return points == 0;
    }

    // The Energy after a spend: never below 0 (the core refuses a cast that costs more than is there).
    constexpr int32_t AfterSpend(int32_t points, int32_t cost)
    {
        return points >= cost ? points - cost : points;
    }

    // Predator's Rhythm: a transition sets the Energy to `restore`, never above the maximum (no partial refund).
    constexpr int32_t AfterTransition(int32_t restore, int32_t maxPoints)
    {
        return std::min(restore, maxPoints);
    }

    // The reachability proof of the hand-off: from `full`, every amount the stance's costs can reach can itself be spent
    // to exactly 0 by them (no stranded residual). `costs` are the stance's spending actions' costs (0s ignored).
    inline bool EverySpendable(int32_t full, std::vector<int32_t> const& costs)
    {
        if (full < 0)
            return false;
        // the amounts reachable from full
        std::vector<bool> reach(size_t(full) + 1, false);
        reach[size_t(full)] = true;
        for (int32_t e = full; e >= 0; --e)
            if (reach[size_t(e)])
                for (int32_t c : costs)
                    if (c > 0 && c <= e)
                        reach[size_t(e - c)] = true;
        // from which amounts 0 can be reached
        std::vector<bool> zero(size_t(full) + 1, false);
        zero[0] = true;
        for (int32_t e = 1; e <= full; ++e)
            for (int32_t c : costs)
                if (c > 0 && c <= e && zero[size_t(e - c)])
                    zero[size_t(e)] = true;
        for (int32_t e = 0; e <= full; ++e)
            if (reach[size_t(e)] && !zero[size_t(e)])
                return false;
        return true;
    }

    // ---- Retreat's blow (spell_ef_kit_primary_area) ----

    // Which of the enemies in the area (by index, nearest first) take the blow: the selected one (`primary`, -1: none or
    // not in the area) and up to `cap - 1` others, nearest first. Without a selected enemy in the area also only `cap - 1`
    // (the hand-off's no-target mode: up to four at half, never five).
    inline std::vector<size_t> AreaPick(size_t inArea, int32_t primary, int32_t cap)
    {
        std::vector<size_t> out;
        int32_t const others = std::max<int32_t>(0, cap - 1);
        if (primary >= 0 && size_t(primary) < inArea)
            out.push_back(size_t(primary));
        int32_t taken = 0;
        for (size_t i = 0; i < inArea && taken < others; ++i)
        {
            if (int32_t(i) == primary)
                continue;
            out.push_back(i);
            ++taken;
        }
        return out;
    }

    // The blow on one enemy: the primary whole, the others `othersPct` percent.
    constexpr int32_t AreaShare(int32_t damage, bool primary, int32_t othersPct)
    {
        return primary ? damage : int32_t(int64_t(damage) * std::max<int32_t>(0, othersPct) / 100);
    }

    // ---- the strikes' rules (spell_ef_kit_hunt_strike) ----

    // A strike that takes the caster's stacks of an aura from its target (Hunter's Opening): `pctPerStack` more for each,
    // added (one stack x 1.5, two x 2.0, never 1.5 x 1.5).
    constexpr int32_t StackedStrike(int32_t damage, int32_t stacks, int32_t pctPerStack)
    {
        int64_t const pct = 100 + int64_t(std::max<int32_t>(0, stacks)) * std::max<int32_t>(0, pctPerStack);
        return int32_t(int64_t(damage) * pct / 100);
    }

    // A stacking mark after one more application (each critical Heartseeker): one more, at most `cap` (2); the shared
    // timer renews at the cap too (the engine's stacking aura renews its duration on every application).
    constexpr int32_t StacksAfterAdd(int32_t have, int32_t cap)
    {
        return std::min(std::max<int32_t>(0, have) + 1, std::max<int32_t>(1, cap));
    }

    // A sure critical strike by the target's health as the strike lands (Fresh Prey: at or above 80; Final Prey: at or
    // below 20): mode 1 at or above `threshold` percent, mode 2 at or below. Exactly at the threshold counts.
    constexpr bool HealthCrit(float healthPct, int32_t threshold, int32_t mode)
    {
        if (mode == 1)
            return healthPct >= float(threshold);
        if (mode == 2)
            return healthPct <= float(threshold);
        return false;
    }

    // The stacks a cast gives for the enemies it struck (Sweeping Momentum: one an enemy, at most `cap`).
    constexpr int32_t HitStacks(uint32_t struck, int32_t cap)
    {
        return std::min<int32_t>(int32_t(std::min<uint32_t>(struck, 1000u)), std::max<int32_t>(0, cap));
    }

    // A share of the damage dealt as a damage over time of its own (Raking Wounds): `pct` percent of `dealt` over
    // `ticks` ticks; a tick of at least 1 when anything was dealt.
    constexpr int32_t EchoTick(int32_t dealt, int32_t pct, int32_t ticks)
    {
        if (dealt <= 0 || pct <= 0 || ticks <= 0)
            return 0;
        int64_t const total = int64_t(dealt) * pct / 100;
        return int32_t(std::max<int64_t>(1, total / ticks));
    }

    // Which instance of a pool (consecutive IDs) a new application takes: the first free one (-1: free), else the one
    // with the least time left. Every application its own clock: the others are never touched.
    inline size_t PickInstance(std::vector<int32_t> const& leftMs)
    {
        size_t best = 0;
        int32_t bestLeft = INT32_MAX;
        for (size_t i = 0; i < leftMs.size(); ++i)
        {
            if (leftMs[i] < 0)
                return i;
            if (leftMs[i] < bestLeft)
            {
                bestLeft = leftMs[i];
                best = i;
            }
        }
        return best;
    }

    // The slow an enemy has from several auras of it (Hamstring Sweep on every Raking Wounds bleed): the strongest one,
    // never their sum (the engine takes the largest movement decrease).
    inline int32_t SlowOf(std::vector<int32_t> const& slows)
    {
        int32_t most = 0;
        for (int32_t s : slows)
            most = std::max(most, std::max<int32_t>(0, s));
        return most;
    }

    // ---- the companion's command (spell_ef_kit_pet_command) ----

    enum class PetStep
    {
        Lapse,   // the command ends with nothing (no companion, no target, too late): the cooldown stays spent
        Wait,    // the companion cannot act now (stunned, confused, fleeing): it waits and the clock runs
        Run,     // farther than the charge's reach (or out of sight): it runs at the enemy
        Charge,  // within reach and in sight: it charges and attacks
    };

    constexpr PetStep PetCommand(bool petAlive, bool targetValid, bool petCanAct, float distance, float reach,
                                 bool inSight, uint32_t elapsedMs, uint32_t timeoutMs)
    {
        if (!petAlive || !targetValid || elapsedMs > timeoutMs)
            return PetStep::Lapse;
        if (!petCanAct)
            return PetStep::Wait;
        if (distance <= reach && inSight)
            return PetStep::Charge;
        return PetStep::Run;
    }

    // ---- the companion's swings (spell_ef_kit_swing_bonus) ----

    struct Swing
    {
        int32_t damage = 0;
        int32_t stacksLeft = 0;
    };

    // One auto attack under a swing bonus (Pack Ferocity: the next 2 swings +30 %): `pct` percent more while a stack is
    // left, a stack used by the swing.
    constexpr Swing SwingBonus(int32_t damage, int32_t pct, int32_t stacks)
    {
        if (stacks <= 0)
            return { damage, 0 };
        return { int32_t(int64_t(damage) * (100 + std::max<int32_t>(0, pct)) / 100), stacks - 1 };
    }

    // The swing time under attack speed raised by `hastePct` percent (Hunting Frenzy +100: twice as often).
    constexpr int32_t HastedSwingMs(int32_t baseMs, int32_t hastePct)
    {
        return int32_t(int64_t(baseMs) * 100 / (100 + std::max<int32_t>(0, hastePct)));
    }
}
