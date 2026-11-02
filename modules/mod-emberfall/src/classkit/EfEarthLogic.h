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

// The arithmetic of the Druid Earthwarden's scripts (Emberfall docs/design/classes/druid-earthwarden.md, ankarna's v1.0-v1.2;
// src/emberfall_classkit_earth.cpp), free of the core so tests/classkit_tests.cpp can run it: the strike's shield gain
// (Deep Restoration, Mountain's Momentum, Relentless Fury; ankarna's rework of 2026-11-02), the pool's cap and its
// out-of-combat trim, the growth stage,
// the roll's speed and reach (Gathering Momentum, Long Roll), the every-Nth counter (Titan's Retribution), the reflected
// share (Reflective Stone) and the channel time the spell mods give (Brutal Follow-Up flat, Tectonic Fury percent: (base +
// flat) x (1 + pct)).

#include <algorithm>
#include <cstdint>

namespace Emberfall::ClassKit::Earth
{
    // The strike's shield gain in percent of max health (ankarna's rework of 2026-11-02: Mountainbreaker no longer heals,
    // every hit adds to Stone Shell's pool): the base (5), plus the low-health ranks' percentage points while the owner
    // is below `lowAt` percent health (Deep Restoration: +5 / +10 below 50, so 10 / 15), times (100 + `stacksPct`) / 100
    // for the ready aura the strike spends (Mountain's Momentum: +100, so double), times (100 + `duringPct`) / 100 while
    // the fury holds (Relentless Fury: +50). They multiply: 5 -> 7.5 (Relentless Fury), 15 -> 22.5 (with Deep Restoration
    // 2 below half health), 45 % (and Mountain's Momentum). Percent x 100, so 7.5 % and 22.5 % stay exact.
    constexpr int32_t ShieldPct(int32_t base, float healthPct, int32_t lowAt, int32_t lowBonus, int32_t duringPct,
                                int32_t stacksPct = 0)
    {
        int64_t pct = std::max<int32_t>(0, base);
        if (lowAt > 0 && healthPct < float(lowAt))
            pct += std::max<int32_t>(0, lowBonus);
        return int32_t(pct * (100 + std::max<int32_t>(0, stacksPct)) * (100 + std::max<int32_t>(0, duringPct)) / 100);
    }

    // ShieldPct's result (percent x 100) of `maxHealth` as an amount, rounded down.
    constexpr int64_t ShieldAmount(int64_t maxHealth, int32_t pctTimes100)
    {
        if (maxHealth <= 0 || pctTimes100 <= 0)
            return 0;
        return maxHealth * pctTimes100 / 10000;
    }

    // What a pooled shield holding `left` holds after a gain of `add`: never above `cap`, never less than it had (a shield
    // above a cap that fell keeps what it holds). Mountainbreaker's gain into Stone Shell, at most 80 %.
    constexpr int64_t ShieldGain(int64_t left, int64_t add, int64_t cap)
    {
        if (add <= 0 || left >= cap)
            return left;
        return std::min(cap, left + add);
    }

    // The out-of-combat trim (Stone Shell: 10 s after leaving combat anything above 40 % of max health goes): what a
    // shield holding `left` keeps once its owner has been out of combat `outMs` of the `afterMs` needed; in combat
    // (`outMs` 0) or before the time, all of it; at or below `cap`, all of it.
    constexpr int64_t Trimmed(int64_t left, int64_t cap, int64_t outMs, int64_t afterMs)
    {
        if (outMs <= 0 || outMs < afterMs || cap < 0 || left <= cap)
            return left;
        return cap;
    }

    // The growth stage of the form: the points in the form's row (each growth talent's misc B), at most `stages`.
    constexpr int32_t GrowthStage(int32_t points, int32_t stages)
    {
        return std::clamp<int32_t>(points, 0, std::max<int32_t>(0, stages));
    }

    // The creature the form shows at `stage`: `stage` after the stage-0 creature, which is the form's own (`formBase`,
    // the transform's misc) unless the owner's look rule names another (`lookBase` > 0: the stoneborn's smaller
    // elementals, the same model at 0.7 of every display scale).
    constexpr int32_t FormCreature(int32_t formBase, int32_t lookBase, int32_t stage)
    {
        return (lookBase > 0 ? lookBase : formBase) + std::max<int32_t>(0, stage);
    }

    // The roll's speed bonus in percent: the base (+100, so 200 % since 2026-11-02; +50 before) plus `stepPct` for every whole `stepYards` travelled
    // (Gathering Momentum: +10 every 5 yd; 0: none).
    constexpr int32_t RollSpeed(int32_t basePct, float travelled, int32_t stepPct, int32_t stepYards)
    {
        if (stepPct <= 0 || stepYards <= 0 || travelled <= 0.0f)
            return basePct;
        return basePct + stepPct * int32_t(travelled / float(stepYards));
    }

    // Whether the roll has run its course: `travelled` yards of at most `reach` (30, Long Roll 35 / 40).
    constexpr bool RollDone(float travelled, int32_t reach)
    {
        return reach > 0 && travelled >= float(reach);
    }

    // Every Nth (Titan's Retribution: 3): the count after one more, and whether this one fires (the count starts over).
    struct Count
    {
        uint32_t count = 0;
        bool fires = false;
    };

    constexpr Count EveryNth(uint32_t count, int32_t n)
    {
        if (n <= 0)
            return { 0, false };
        uint32_t const next = count + 1;
        if (next >= uint32_t(n))
            return { 0, true };
        return { next, false };
    }

    // Reflective Stone: the share of what the shell absorbed that hits back (10, Reflective Fury +10: 20 %).
    constexpr int32_t Reflected(int32_t absorbed, int32_t pct)
    {
        if (absorbed <= 0 || pct <= 0)
            return 0;
        return int32_t(int64_t(absorbed) * pct / 100);
    }

    // The channel time a flat (Brutal Follow-Up: -500 ms) and a percent (Tectonic Fury: -50) spell mod give a base: the
    // core's SPELLMOD_DURATION rule, (base + flat) x (100 + pct) / 100, so the two multiply (ankarna's 0.75 s).
    constexpr int32_t ChannelMs(int32_t base, int32_t flat, int32_t pct)
    {
        int32_t const sum = base + flat;
        if (sum <= 0)
            return 0;
        return std::max<int32_t>(0, sum * (100 + pct) / 100);
    }

    // The pull of a field (Gravitational Tremors): the yards a unit at `distance` from the centre is moved this tick, at
    // most `step`, never past `keep` yards from the centre (0 when it is already that close).
    constexpr float PullStep(float distance, float step, float keep)
    {
        if (step <= 0.0f || distance <= keep)
            return 0.0f;
        return std::min(step, distance - keep);
    }
}
