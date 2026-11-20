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

// The arithmetic of the Corsair's scripts (Emberfall docs/design/classes/warrior-corsair.md; src/emberfall_classkit_flow.cpp),
// free of the core so tests/classkit_tests.cpp can run it: Flow's step, the split damage over time, the instance pool,
// the conditional crit, the hook's travel.

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Emberfall::ClassKit::Flow
{
    // What a builder does to Flow (spell_ef_kit_flow).
    enum class Step : uint8_t
    {
        Nothing,        // a finisher is prepared: the builder strikes and changes nothing
        Override,       // No Quarter: the builder prepares its own finisher at once
        Combine,        // a partner's mark is held: both marks go, the pair's finisher is prepared
        Mark,           // no partner: the builder's mark is laid (or refreshed: the same builder twice combines nothing)
    };

    // `gate`: a finisher is prepared; `override`: an aura of the caster's names this builder's mark (No Quarter);
    // `partner`: the caster holds a partner's mark.
    constexpr Step NextStep(bool gate, bool override, bool partner)
    {
        if (gate)
            return Step::Nothing;
        if (override)
            return Step::Override;
        if (partner)
            return Step::Combine;
        return Step::Mark;
    }

    // The ticks of a periodic aura over its base duration (at least one).
    constexpr int32_t Ticks(int32_t durationMs, int32_t periodMs)
    {
        if (durationMs <= 0 || periodMs <= 0)
            return 1;
        return std::max<int32_t>(1, durationMs / periodMs);
    }

    // One tick of a split damage over time: `pct` percent of the strike's `base` over `ticks` ticks, doubled by a crit
    // (Saltwound: 100 % over 12; Razorstorm: 233 % over 5); at least 1 while there is a base.
    constexpr int32_t SplitTick(int32_t base, int32_t pct, int32_t ticks, bool doubled)
    {
        if (base <= 0 || pct <= 0)
            return 0;
        int64_t total = int64_t(base) * pct / 100;
        if (doubled)
            total *= 2;
        return int32_t(std::max<int64_t>(1, total / std::max<int32_t>(1, ticks)));
    }

    // The instance of a pool to cast (Saltwound's six): the first free one (left < 0), else the one with the least time
    // left (it is replaced).
    inline size_t PickInstance(std::vector<int32_t> const& left)
    {
        size_t best = 0;
        for (size_t i = 0; i < left.size(); ++i)
        {
            if (left[i] < 0)
                return i;
            if (left[i] < left[best])
                best = i;
        }
        return best;
    }

    // The chance in percent that a strike which rolled no crit becomes one, so its whole chance rises from `basePct` by
    // `extraPct` (Blood in the Water: +25 %); 0..100.
    constexpr float ExtraCritChance(float basePct, float extraPct)
    {
        if (extraPct <= 0.0f)
            return 0.0f;
        float const base = std::clamp(basePct, 0.0f, 100.0f);
        if (base >= 100.0f)
            return 0.0f;
        return std::clamp(extraPct * 100.0f / (100.0f - base), 0.0f, 100.0f);
    }

    // How far the hook hauls an enemy `dist` yards away so it lands `stopShort` yards from the caster (0: already there).
    constexpr float HookTravel(float dist, float stopShort)
    {
        return dist > stopShort ? dist - stopShort : 0.0f;
    }

    // ---- v2.0 (ankarna's Corsair Talent Tree v2.0, 2026-10-27; warrior-corsair.md §16) ----------------------------------

    // The instances of a split damage over time's pool: the dummy's own (misc B; Deep Wounds 1: a new one replaces the
    // old), or a passive's count while it holds (Open Wounds: 6, every application its own); 1..8.
    constexpr int32_t PoolInstances(int32_t own, int32_t overrideCount)
    {
        return std::clamp<int32_t>(overrideCount > 0 ? overrideCount : own, 1, 8);
    }

    // Percent more for `stacks` stacks at `perStack` percent each (Crimson Undertow: 2 % a stack, 20 stacks: 40 %).
    constexpr int32_t StackBonusPct(int32_t stacks, int32_t perStack)
    {
        return stacks > 0 && perStack > 0 ? stacks * perStack : 0;
    }

    // `amount` raised by `pct` percent (0: as it is).
    constexpr int32_t WithPct(int32_t amount, int32_t pct)
    {
        return pct > 0 ? int32_t(int64_t(amount) * (100 + pct) / 100) : amount;
    }

    // `pct` percent of `amount`, at least 1 while both are above 0 (Relentless Strike's 50 %, Vicious Follow-Through's 30 %).
    constexpr int32_t ShareOf(int32_t amount, int32_t pct)
    {
        if (amount <= 0 || pct <= 0)
            return 0;
        return int32_t(std::max<int64_t>(1, int64_t(amount) * pct / 100));
    }

    // A buff by the enemies struck (Evasive Sweep): `per` for each, at most `cap` (0: no cap).
    constexpr int32_t CountAmount(uint32_t struck, int32_t per, int32_t cap)
    {
        int64_t const total = int64_t(struck) * std::max<int32_t>(0, per);
        return int32_t(cap > 0 ? std::min<int64_t>(total, cap) : total);
    }

    // Perfect Rhythm: whether a landed finisher `current` differs from the last landed one `previous` (0: none yet, it
    // only starts the tracking).
    constexpr bool RhythmChanged(uint32_t previous, uint32_t current)
    {
        return previous != 0 && current != 0 && previous != current;
    }

    // Cutlass Barrage: when strike `index` (0-based) of a barrage lands, `intervalMs` apart, the first at once.
    constexpr int32_t BarrageDelay(int32_t index, int32_t intervalMs)
    {
        return index <= 0 ? 0 : index * std::max<int32_t>(0, intervalMs);
    }

    // ---- ankarna's "Gameplay & Survivability Rework" (2026-11-19; warrior-corsair.md §21) -----------------------------

    // Flow as the scripts keep it in auras, for the tests (spell_ef_kit_flow, spell_ef_kit_flow_clear, spell_ef_kit_faces
    // act on the auras the same way): the builders' marks held (a bit each), the prepared finisher (0: none) and whether
    // No Quarter holds. The finishers a pair and a builder under No Quarter prepare are the data's (the marks' and No
    // Quarter's effects), given here as tables: pair[i][j] for builders i and j, own[i] for builder i alone.
    struct FlowState
    {
        uint8_t marks = 0;
        uint8_t ready = 0;
        bool noQuarter = false;
    };

    struct FlowTable
    {
        uint8_t pair[3][3] = {};
        uint8_t own[3] = {};
    };

    // A builder `b` (0..2) cast at an enemy it reached (NextStep: a prepared finisher holds; No Quarter prepares the
    // builder's own; a partner's mark prepares the pair's; else the mark).
    constexpr FlowState AfterBuilder(FlowState s, FlowTable const& t, int b)
    {
        uint8_t const bit = uint8_t(1u << b);
        int partner = -1;
        for (int i = 0; i < 3; ++i)
            if (i != b && (s.marks & (1u << i)))
                partner = i;
        switch (NextStep(s.ready != 0, s.noQuarter && t.own[b] != 0, partner >= 0))
        {
            case Step::Nothing:
                break;
            case Step::Override:
                s.ready = t.own[b];
                break;
            case Step::Combine:
                s.marks = 0;
                s.ready = t.pair[b][partner];
                break;
            case Step::Mark:
                s.marks = uint8_t(s.marks | bit);
                break;
        }
        return s;
    }

    // No Quarter's cast: an incomplete Flow (the marks) is reset, a prepared finisher stays (spell_ef_kit_flow_clear).
    constexpr FlowState AtNoQuarterCast(FlowState s)
    {
        s.marks = 0;
        s.noQuarter = true;
        return s;
    }

    // No Quarter's end (2026-11-19): a prepared finisher stays prepared; only the marks could go (No Quarter never lets
    // one stand). Its effects name the marks as their triggers, so the core's clean-up of an ending aura's triggered
    // auras (AuraEffect::CleanupTriggeredSpells) takes those and never the ready aura (it took it before the rework).
    constexpr FlowState AtNoQuarterEnd(FlowState s)
    {
        s.marks = 0;
        s.noQuarter = false;
        return s;
    }

    // The Flow Finisher: the prepared finisher is unleashed and spent; nothing prepared, nothing happens.
    constexpr FlowState AtFinisher(FlowState s)
    {
        s.ready = 0;
        return s;
    }

    // The parry a character gets the moment it can parry at all (Player::UpdateParryPercentage's 5 % that does not
    // diminish): a parry window for a character who could not parry before counts it in (Defensive Momentum: +20 % is
    // 20 % in all, the aura's own amount then 15).
    constexpr int32_t kEngineBaseParryPct = 5;

    // The amount of a parry window's aura (spell_ef_kit_parry_window): `wanted` percent more parry; for a character who
    // cannot parry without it, less the engine's base (never below 0).
    constexpr int32_t ParryWindowAmount(int32_t wanted, bool parriesWithout)
    {
        if (wanted <= 0)
            return 0;
        return parriesWithout ? wanted : std::max<int32_t>(0, wanted - kEngineBaseParryPct);
    }

    // The reach a channel at one enemy keeps (Spell::UpdateChanneledTargetList: its range and 10 %, at most 3 yd more):
    // spell_ef_kit_channel_reach refuses a cast beyond it (Cutlass Barrage: 5 yd -> 5.5), where the core's melee leeway for
    // a moving caster (up to ~2.7 yd) would start a channel that ends at once, its cooldown spent and no strike struck.
    constexpr float ChannelHoldRange(float range)
    {
        return range <= 0.0f ? 0.0f : range + std::min(3.0f, range * 0.1f);
    }

    // A linked aura (spell_ef_kit_aura_link): on while its carrier holds and the caster holds the rule that names it.
    constexpr bool LinkedAuraOn(bool carrierUp, bool ruleHeld)
    {
        return carrierUp && ruleHeld;
    }
}
