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

// Emberfall class kits: the arithmetic of the kits' mechanics without the core, so tests/classkit_tests.cpp can check it
// (self stacks: the Paladin's Crown of Blades, once its Sunblades; the Frostguard's shell; the Druid's Static; the
// Warlock's prices; the Wrathbringer's cards, Emberfall docs/design/classes/paladin-wrathbringer.md). EfClassKit.cpp and
// the spell scripts (src/emberfall_classkit*.cpp) use it.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace Emberfall::ClassKit::Logic
{
    // The cap of a self-stacking aura: its effect 1 value with the caster's spell mods (Sunblades 5; Crown of Blades'
    // +3 makes it 8), never above the aura's own stack maximum (Spell.dbc StackAmount) nor below 1. A value of 0 or less
    // means the stack maximum.
    constexpr int32_t StackCap(int32_t value, int32_t stackMax)
    {
        int32_t const hard = std::max<int32_t>(1, stackMax);
        if (value <= 0)
            return hard;
        return std::clamp<int32_t>(value, 1, hard);
    }

    // The stacks a builder raises. `value` is its stack effect's value (spell mods in: Crown of Blades +1, Marked by
    // Heaven +1, Dawn's Return +1/+2); `perEnemyMax` its misc B: above 0 one stack per enemy struck, at most that many,
    // plus value - 1 (Sunwheel: 1 per enemy, at most 3; under Crown one more). A builder whose enemy effects struck
    // nobody (a miss, a dodge, nobody in reach) raises nothing; one without enemy effects (`needsHit` false) always does.
    constexpr int32_t BuilderGain(int32_t value, int32_t perEnemyMax, uint32_t hits, bool needsHit)
    {
        if (needsHit && hits == 0)
            return 0;
        if (perEnemyMax > 0)
            return std::max<int32_t>(0, std::min<int32_t>(int32_t(hits), perEnemyMax) + value - 1);
        return std::max<int32_t>(0, value);
    }

    // The stacks after a gain: at most the cap, but never fewer than were held (blades raised under Crown of Blades stay
    // when it ends, until they are launched or lapse).
    constexpr int32_t StacksAfter(int32_t have, int32_t gain, int32_t cap)
    {
        if (gain <= 0 || have >= cap)
            return have;
        return std::min<int32_t>(cap, have + gain);
    }

    // How much longer (ms) an aura may still become: `want`, but at most `maxExtraMs` beyond its own duration `baseMs`
    // (spell mods in), counting what it was lengthened already (its max duration now: `currentMaxMs`).
    constexpr int32_t LengthenBy(int32_t want, int32_t maxExtraMs, int32_t currentMaxMs, int32_t baseMs)
    {
        int32_t const room = maxExtraMs - std::max<int32_t>(0, currentMaxMs - baseMs);
        return std::max<int32_t>(0, std::min<int32_t>(want, room));
    }

    // Percent of `max` per stack, for `stacks` stacks (the spenders' mana back, Wrath's Mercy's heal).
    constexpr int64_t PctPerStack(int64_t max, int32_t pctPerStack, uint32_t stacks)
    {
        if (max <= 0 || pctPerStack <= 0)
            return 0;
        return max * pctPerStack * int64_t(stacks) / 100;
    }

    // ---- the stacked shell (the Frostguard's Frost Shell, Emberfall docs/design/classes/mage-frostguard.md) ----
    // One absorb pool shown as stacks: each stack is worth `perStack` (a share of max health), at most `maxStacks`.

    // The stacks a pool of `left` shows: every started stack counts (ceil), at most `maxStacks`; 0 when empty.
    constexpr int32_t ShellStacks(int64_t left, int64_t perStack, int32_t maxStacks)
    {
        if (left <= 0 || perStack <= 0 || maxStacks <= 0)
            return 0;
        int64_t const stacks = (left + perStack - 1) / perStack;
        return int32_t(std::min<int64_t>(stacks, maxStacks));
    }

    // The pool after gaining `gain` stacks: `gain` x `perStack` more, at most `maxStacks` x `perStack` (a stack worn
    // down by a blow is topped up by the next gain, not lost).
    constexpr int64_t ShellAfterGain(int64_t left, int64_t perStack, int32_t gain, int32_t maxStacks)
    {
        if (perStack <= 0 || maxStacks <= 0)
            return std::max<int64_t>(0, left);
        int64_t const cap = perStack * maxStacks;
        int64_t const now = std::max<int64_t>(0, left) + perStack * std::max<int32_t>(0, gain);
        return std::min<int64_t>(cap, now);
    }

    // The shell stacks a shatter gives: one per `stacksPerShell` stacks shattered, summed over every enemy and rounded
    // down, times `shellsPerUnit` (Icebreak and Glacial Rupture: 1 per 2 Frost Stacks).
    constexpr int32_t ShellGain(uint32_t shattered, int32_t stacksPerShell, int32_t shellsPerUnit)
    {
        if (stacksPerShell <= 0 || shellsPerUnit <= 0)
            return 0;
        return int32_t(shattered / uint32_t(stacksPerShell)) * shellsPerUnit;
    }

    // A cooldown shortened by stacks consumed (Winter's Embrace on Hoarstorm): `consumed` x `msPerStack`, but at most
    // what is left of `maxMs` for this cooldown (`alreadyMs` taken off it so far; 0: no limit) and never more than the
    // `remainingMs`.
    constexpr int32_t StackCooldownCut(uint32_t consumed, int32_t msPerStack, int32_t alreadyMs, int32_t maxMs,
                                       int32_t remainingMs)
    {
        if (msPerStack <= 0 || remainingMs <= 0 || consumed == 0)
            return 0;
        int64_t want = int64_t(consumed) * msPerStack;
        if (maxMs > 0)
            want = std::min<int64_t>(want, std::max<int32_t>(0, maxMs - alreadyMs));
        return int32_t(std::min<int64_t>(want, remainingMs));
    }

    // ---- a spec's own power (the Druid's Stormbringer: Static, Emberfall docs/design/classes/druid-stormbringer.md) ----

    // The points a builder builds (spell_ef_kit_build_power). `value` is its build effect's value (spell mods in:
    // Supercell +50 %, Hailwind Surge +2/+4); `perEnemyMax` its misc B: above 0 the value per enemy struck, at most that
    // many enemies (Hailwind: 8 each, at most 4); a builder whose enemy effects struck nobody builds nothing; `first`
    // doubles it (First Flash: the first build of a fight).
    constexpr int32_t PowerGain(int32_t value, int32_t perEnemyMax, uint32_t hits, bool needsHit, bool first)
    {
        if (value <= 0 || (needsHit && hits == 0))
            return 0;
        int32_t const gain = perEnemyMax > 0 ? value * std::min<int32_t>(int32_t(hits), perEnemyMax) : value;
        return first ? gain * 2 : gain;
    }

    // Heavy Rend's overspend: of `want` more points, what the caster still holds beyond the spell's own cost (`have` and
    // `cost` in stored units, `scale` stored units per point): whole points only.
    constexpr int32_t Overspend(int32_t want, int32_t have, int32_t cost, int32_t scale)
    {
        if (want <= 0 || scale <= 0)
            return 0;
        int32_t const spare = (have - cost) / scale;
        return std::clamp<int32_t>(spare, 0, want);
    }

    // The damage after an overspend of `points` at `perPointTenths` tenths of a percent each (Heavy Rend: 15, +1.5 %).
    constexpr int32_t OverspendDamage(int32_t damage, int32_t points, int32_t perPointTenths)
    {
        if (damage <= 0 || points <= 0 || perPointTenths <= 0)
            return damage;
        return int32_t(int64_t(damage) * (1000 + int64_t(points) * perPointTenths) / 1000);
    }

    // An echo's damage (Overcast, Thunder Roll): `pct` percent of the damage the spell dealt, at least 1.
    constexpr int32_t EchoDamage(int32_t damage, int32_t pct)
    {
        if (damage <= 0 || pct <= 0)
            return 0;
        return std::max<int32_t>(1, int32_t(int64_t(damage) * pct / 100));
    }

    // The drain rate of a spec power whose pool is `max` (stored units) against the spec's own maximum `baseMax`: in
    // proportion, so a larger pool (Brooding Sky: 1500, 2000 of 1000) empties from full in the same time; never below
    // the spec's own rate.
    constexpr uint32_t DrainRate(uint32_t perSecond, uint32_t max, uint32_t baseMax)
    {
        if (baseMax == 0 || max <= baseMax)
            return perSecond;
        return uint32_t(uint64_t(perSecond) * max / baseMax);
    }

    // The out-of-combat drain of a spec power during `diffMs`: `perSecond` stored units a second times `multiplier` (the
    // regen-percent auras: Static Memory 0.5 / 0.25), the fraction carried in `carry` (thousandths of a unit); nothing
    // before `outOfCombatMs` passed `delayMs`. Returns the whole units to drain now.
    inline int32_t Drain(uint32_t perSecond, float multiplier, uint32_t diffMs, uint32_t outOfCombatMs, uint32_t delayMs,
                         int64_t& carry)
    {
        if (outOfCombatMs <= delayMs || perSecond == 0 || multiplier <= 0.0f)
            return 0;
        uint32_t const counted = std::min(diffMs, outOfCombatMs - delayMs);
        carry += int64_t(double(perSecond) * double(multiplier) * double(counted));
        int32_t const whole = int32_t(carry / 1000);
        carry -= int64_t(whole) * 1000;
        return whole;
    }

    // A class's own power (the Hunter's Focus, Emberfall docs/design/classes/hunter-marksman.md): the regeneration during
    // `diffMs` at `perSecond` points a second times `multiplier` (in a fight: the haste factor and the regen-percent
    // auras; out of one the plain rate), the fraction carried in `carry` (millionths of a point). Returns the whole points
    // to add now.
    inline int32_t Regen(float perSecond, float multiplier, uint32_t diffMs, int64_t& carry)
    {
        if (perSecond <= 0.0f || multiplier <= 0.0f)
            return 0;
        carry += int64_t(double(perSecond) * double(multiplier) * double(diffMs) * 1000.0);
        int32_t const whole = int32_t(carry / 1000000);
        carry -= int64_t(whole) * 1000000;
        return whole;
    }

    // The haste factor of a regeneration (Keen Draw: 2 / 4 % faster): from the unit's cast speed field
    // (UNIT_MOD_CAST_SPEED, 1 / (1 + haste)); a bad field is no haste.
    constexpr float HasteFactor(float castSpeed)
    {
        return castSpeed > 0.01f ? 1.0f / castSpeed : 1.0f;
    }

    // The Marksman's spender of self stacks (spell_ef_kit_spend_self_stacks: Full Draw): `base` damage `pctPerStack`
    // percent more for each of `stacks` stacks.
    constexpr int32_t StackedDamage(int32_t base, int32_t pctPerStack, uint32_t stacks)
    {
        return base <= 0 ? base : int32_t(int64_t(base) * (100 + int64_t(pctPerStack) * stacks) / 100);
    }

    // The Warlock's Blood Price (spell_ef_kit_health_price): `pct` percent (spell mods in: Thinned Blood) of the caster's
    // `maxHealth`, at least 1; the cast is refused unless the caster keeps at least 1 health after paying it.
    constexpr int32_t HealthPrice(uint32_t maxHealth, int32_t pct)
    {
        if (pct <= 0 || maxHealth == 0)
            return 0;
        return std::max<int32_t>(1, int32_t(int64_t(maxHealth) * pct / 100));
    }
    constexpr bool CanPay(uint32_t health, int32_t price) { return int64_t(health) > int64_t(price); }

    // A share of damage handed on as healing (spell_ef_kit_transfuse: Overflow 15 %, Harvest Tide 40 %); 0 below one
    // point (nothing to hand on).
    constexpr int32_t Transfused(int32_t damage, int32_t pct)
    {
        if (damage <= 0 || pct <= 0)
            return 0;
        return int32_t(int64_t(damage) * pct / 100);
    }

    // Of allies with `health` of `maxHealth`, the most injured one by the share of health missing; only allies below
    // full count. Returns the index or -1 (everyone full). Ties: the first.
    template <typename It>
    int32_t MostInjured(It healthBegin, It healthEnd, It maxBegin)
    {
        int32_t best = -1;
        double bestShare = 1.0;
        int32_t i = 0;
        for (It h = healthBegin, m = maxBegin; h != healthEnd; ++h, ++m, ++i)
        {
            if (*m == 0 || *h >= *m)
                continue;
            double const share = double(*h) / double(*m);
            if (best < 0 || share < bestShare)
            {
                best = i;
                bestShare = share;
            }
        }
        return best;
    }

    // Charges: a cast with `recharging` charges already recharging (the charge aura's stacks) of `max`: how many recharge
    // after it, and whether one is still left (then the spell's cooldown is cleared).
    struct ChargeSpend
    {
        uint32_t recharging;
        bool left;
    };
    constexpr ChargeSpend SpendCharge(uint32_t recharging, uint32_t max)
    {
        uint32_t const cap = std::max<uint32_t>(1, max);
        uint32_t const now = std::min<uint32_t>(recharging + 1, cap);
        return { now, now < cap };
    }
    // ---- the Wrathbringer's cards (Emberfall docs/design/classes/paladin-wrathbringer.md §7, 2026-10-21) ----

    // Last Sentence (spell_ef_kit_execute): the bonus in percent for a target at `healthPct` of its health: none at full
    // health, rising with the health missing to `maxBonus` at `fullAtPct` or below (100 % at 20 %).
    constexpr int32_t ExecuteBonusPct(float healthPct, int32_t maxBonus, int32_t fullAtPct)
    {
        if (maxBonus <= 0)
            return 0;
        int32_t const full = std::clamp<int32_t>(fullAtPct, 0, 99);
        if (healthPct <= float(full))
            return maxBonus;
        if (healthPct >= 100.0f)
            return 0;
        return int32_t(float(maxBonus) * (100.0f - healthPct) / float(100 - full));
    }

    // A share of an amount (Last Sentence's refund on a kill: 50 % of its cost; Sacred Brand's splash: 50 %).
    constexpr int32_t PctOf(int32_t amount, int32_t pct)
    {
        if (amount <= 0 || pct <= 0)
            return 0;
        return int32_t(int64_t(amount) * pct / 100);
    }

    // A player's main stat for Solenne's Blessing (spell_ef_kit_main_stat), by class (SharedDefines.h Classes): strength
    // (0) for warriors, paladins and death knights, agility (1) for hunters and rogues, intellect (3) for the casters
    // (priest, shaman, mage, warlock, druid: Emberfall's Druid is the Stormbringer, a caster).
    constexpr int32_t MainStat(uint8_t classId)
    {
        switch (classId)
        {
            case 1: case 2: case 6: return 0;
            case 3: case 4: return 1;
            default: return 3;
        }
    }

    // An amount `pct` percent stronger (Solenne's Wrath doubles Solenne's Blessing: +100 %).
    constexpr int32_t Empowered(int32_t amount, int32_t pct)
    {
        if (pct <= 0)
            return amount;
        return int32_t(int64_t(amount) * (100 + pct) / 100);
    }

    // How many swords a launch takes from `stacks` held (spell_ef_kit_launch_stack): `perCast` (1: Sacred Brand, Edict,
    // Oathblade), all of them for 0 (Solenne's Wrath).
    constexpr int32_t LaunchCount(int32_t perCast, int32_t stacks)
    {
        if (stacks <= 0)
            return 0;
        return perCast > 0 ? std::min(perCast, stacks) : stacks;
    }

    // A cooldown cut (spell_ef_kit_cooldown_cut: Oathbound Rush on Oathblade, Endless Crown on Crown of Blades): `cutMs`
    // off, never more than the `remainingMs`.
    constexpr int32_t CooldownCut(int32_t cutMs, int32_t remainingMs)
    {
        if (cutMs <= 0 || remainingMs <= 0)
            return 0;
        return std::min(cutMs, remainingMs);
    }
    // ---- the Mage's Pyromancer (Emberfall docs/design/classes/mage-pyromancer.md: Heat and its tiers) ----

    // Whether a spec power of `stored` units reaches a tier at `atShown` points as shown (`scale` stored units a point):
    // Blazing at 30, Inferno at 70, Maximum at 100 Heat (EfSpecPower.cpp keeps the tier auras).
    constexpr bool TierReached(int32_t stored, int32_t atShown, int32_t scale)
    {
        return atShown > 0 && scale > 0 && int64_t(stored) >= int64_t(atShown) * scale;
    }

    // A ramping channel's tick (Sunlance, spell_ef_kit_ramp_channel): tick n (1-based) deals `base` x (1 + rampPct /
    // 100)^(n - 1), each tick rampPct percent more than the last; the final tick `finalPct` percent more on top (Blazing).
    inline int32_t RampTick(int32_t base, int32_t rampPct, uint32_t tick, bool final, int32_t finalPct)
    {
        if (base <= 0)
            return 0;
        double amount = double(base);
        for (uint32_t i = 1; i < tick; ++i)
            amount *= 1.0 + double(std::max(0, rampPct)) / 100.0;
        if (final && finalPct > 0)
            amount *= 1.0 + double(finalPct) / 100.0;
        return int32_t(amount + 0.5);
    }

    // Whether a point (px, py) lies behind the target (tx, ty) on the line from the caster (cx, cy) through it: farther
    // along than the target, at most `length` yards beyond it, at most `halfWidth` (plus the unit's reach) to the side
    // (Sunlance at 100 Heat pierces: the enemies behind its target).
    inline bool BehindOnLine(float cx, float cy, float tx, float ty, float px, float py, float length, float halfWidth,
                             float reach)
    {
        float dx = tx - cx, dy = ty - cy;
        float const len = dx * dx + dy * dy;
        if (len <= 0.0001f)
            return false;
        float const inv = 1.0f / std::sqrt(len);
        dx *= inv;
        dy *= inv;
        float const rx = px - tx, ry = py - ty;
        float const along = rx * dx + ry * dy;
        float const across = -rx * dy + ry * dx;
        return along > 0.0f && along <= length + reach && (across < 0.0f ? -across : across) <= halfWidth + reach;
    }

    // ---- the Paladin's Oathkeeper (Emberfall docs/design/classes/paladin-oathkeeper.md: the chain, Bastion's Resolve) ----

    // A link of the chain (spell_ef_kit_cooldown_cut_pct): `pct` percent of the spell's BASE cooldown (`baseMs`, as
    // Spell.dbc has it; ankarna's decision of 2026-10-23, Q4: Shieldbrand -1.2 s on Aegis Ricochet's 6 s, Aegis of
    // Judgment -36 s on the Retribution's 180 s) off a running cooldown, never more than is left (`remainingMs`; 100:
    // all of it, Judgment's Echo's reset). With `floorMs` above 0 the cooldown as a whole (`fullMs` as it started, spell
    // mods in, less `alreadyCutMs` taken off it before) never falls below the floor (Solenne's Retribution: never below
    // 60 s). Nothing while the cooldown is not running.
    constexpr int32_t CutBasePct(int32_t remainingMs, int32_t baseMs, int32_t pct, int32_t fullMs = 0,
                                 int32_t floorMs = 0, int32_t alreadyCutMs = 0)
    {
        if (remainingMs <= 0 || pct <= 0 || baseMs <= 0)
            return 0;
        int32_t take = int32_t(std::min<int64_t>(int64_t(baseMs) * std::min<int32_t>(pct, 100) / 100, remainingMs));
        if (floorMs > 0 && fullMs > 0)
            take = std::min<int32_t>(take, std::max<int32_t>(0, fullMs - std::max<int32_t>(0, alreadyCutMs) - floorMs));
        return std::max<int32_t>(0, take);
    }

    // Bastion's Resolve (spell_ef_kit_capped_extend): a cast with `leftMs` of the aura still to run adds its `addMs` to
    // it, at most `capMs` in all (20 s; Long Resolve 24/28); the first cast is simply `addMs`.
    constexpr int32_t ExtendCapped(int32_t leftMs, int32_t addMs, int32_t capMs)
    {
        int64_t const want = int64_t(std::max<int32_t>(0, leftMs)) + std::max<int32_t>(0, addMs);
        if (capMs <= 0)
            return int32_t(want);
        return int32_t(std::min<int64_t>(want, std::max<int32_t>(capMs, addMs)));
    }

    // Solenne's Favor (spell_ef_kit_add_charge): the charges after a gain, at most `max` (2).
    constexpr int32_t ChargesAfter(int32_t have, int32_t max)
    {
        return std::max<int32_t>(1, std::min<int32_t>(std::max<int32_t>(0, have) + 1, std::max<int32_t>(1, max)));
    }

    // Solenne's Aegis's bursts (spell_ef_kit_absorb_burst): an absorbed blow at `nowMs` bursts when at least `gapMs` have
    // passed since the last burst (`lastMs`, 0: none yet).
    constexpr bool BurstDue(uint64_t nowMs, uint64_t lastMs, int32_t gapMs)
    {
        return lastMs == 0 || gapMs <= 0 || nowMs >= lastMs + uint64_t(gapMs);
    }

    // ---- the Warrior's Executioner v2.0 (Emberfall docs/design/classes/warrior-executioner.md: personal Openings) ----

    // Openings are the Executioner's spec power (0-5, stored x 10 in the runic power slot): a gain never takes them above
    // `cap`, the excess is lost (Sundering Blow, Cleaving Edge, Steel Throw +1; Deathblow +2, +3 in Killing Instinct;
    // Earthbreaker +3).
    constexpr int32_t OpeningsAfterGain(int32_t have, int32_t gain, int32_t cap)
    {
        return std::clamp<int32_t>(std::max<int32_t>(0, have) + std::max<int32_t>(0, gain), 0, std::max<int32_t>(0, cap));
    }

    // A finisher (Final Verdict, Whirling Steel) needs at least `cost` Openings (3), with Perfect Execution too.
    constexpr bool FinisherCastable(int32_t have, int32_t cost)
    {
        return cost > 0 && have >= cost;
    }

    // The Openings after a finisher (spell_ef_kit_finisher): exactly `cost` spent (3 -> 0, 4 -> 1, 5 -> 2), none while
    // Perfect Execution makes it free, then `giveBack` (Headsman's Due: 1) after it struck, never above `cap`.
    constexpr int32_t FinisherOpeningsLeft(int32_t have, int32_t cost, bool free, int32_t giveBack, int32_t cap)
    {
        if (!FinisherCastable(have, cost))
            return std::clamp<int32_t>(have, 0, std::max<int32_t>(0, cap));
        return OpeningsAfterGain(free ? have : have - cost, giveBack, cap);
    }

    // Sweeping Deathblow (spell_ef_kit_sweep): of `found` other enemies near the target, at most `most` (4; Sweeping
    // Reach 5/6) take the sweep.
    constexpr int32_t SweepCount(int32_t found, int32_t most)
    {
        return std::clamp<int32_t>(found, 0, std::max<int32_t>(0, most));
    }

    // A sweep hit (spell_ef_kit_sweep_hit): `pct` percent (50; Red Harvest's spell mod on the helper's damage makes it
    // 75 of Deathblow's base) of the hit the helper rolled with Deathblow's own numbers.
    constexpr int32_t SweepDamage(int32_t damage, int32_t pct)
    {
        if (damage <= 0 || pct <= 0)
            return 0;
        return int32_t(std::min<int64_t>(int64_t(damage) * pct / 100, INT32_MAX));
    }

    // Killing Instinct's +50 % critical strike damage (SPELL_AURA_MOD_CRIT_DAMAGE_BONUS, Unit::SpellCriticalDamageBonus):
    // the crit's whole damage times (100 + pct) / 100, a 200 crit 300.
    constexpr int32_t CritWithBonus(int32_t critDamage, int32_t pct)
    {
        if (critDamage <= 0)
            return 0;
        return int32_t(std::min<int64_t>(int64_t(critDamage) * (100 + std::max<int32_t>(0, pct)) / 100, INT32_MAX));
    }

    // ---- a spec power's own maximum and its keeping (EfSpecPower.cpp) ----

    // A spec power with a maximum other than its slot's (Openings: 50 stored of runic power's 1000; the Pyromancer's Heat:
    // 1500): what the hook after Player::UpdateMaxPower adds (negative or positive), so the slot's max-power auras still
    // add on top. 0 when the spec power has no maximum of its own or the slot's.
    constexpr int32_t SpecPowerMaxDelta(int32_t specMax, int32_t slotMax)
    {
        return specMax > 0 ? specMax - slotMax : 0;
    }

    // A refund's health gate (Headsman's Due, playtest #69): below `belowPct` % health; 0: no gate. A struck enemy's
    // health is read after the blow (a killing blow counts: 0 %).
    constexpr bool BelowHealth(float healthPct, int32_t belowPct)
    {
        return belowPct <= 0 || healthPct < float(belowPct);
    }

    // A kept spec power (Openings: `start: keep`) the core emptied (a level up: Player::InitStatsForLevel zeroes the
    // runic power slot) comes back as it was before; anything else stays as it is.
    constexpr int32_t KeptPower(int32_t now, int32_t before, bool keep)
    {
        return keep && now == 0 && before > 0 ? before : now;
    }

    // ---- the Druid's Bloomtender (Emberfall docs/design/classes/druid-bloomtender.md: Lifesprout, Verdant Ancient) ----

    // A growth plant's clock (spell_ef_kit_growth_stage): a stage that began with `leftMs` of the cycle still to run (the
    // first stage: the whole cycle, 18 s; Lasting Renewal +3/6 s) and ran `elapsedMs` hands the next stage what is left
    // (Seed -> Sprout under the fixed 18 s clock: a Seed pushed at 2 s leaves a Sprout of 16 s). Never below 0.
    constexpr int32_t GrowthLeft(int32_t leftMs, int32_t elapsedMs)
    {
        return std::max<int32_t>(0, std::max<int32_t>(0, leftMs) - std::max<int32_t>(0, elapsedMs));
    }

    // How long the next stage lasts: what is left of the cycle for the last stage (the one that blooms), at most its own
    // duration for a stage between (a chain of three or more stages).
    constexpr int32_t StageMs(int32_t leftMs, int32_t ownMs, bool last)
    {
        if (last || ownMs <= 0)
            return std::max<int32_t>(0, leftMs);
        return std::max<int32_t>(0, std::min<int32_t>(leftMs, ownMs));
    }

    // Rising Sap (spell_ef_kit_mana_back): `tenths` tenths of a percent of `maxMana` (20: 2 %; Returning Essence 25/30).
    constexpr int32_t ManaBack(uint32_t maxMana, int32_t tenths)
    {
        if (tenths <= 0)
            return 0;
        return int32_t(std::min<int64_t>(int64_t(maxMana) * tenths / 1000, INT32_MAX));
    }

    // Verdant Ancient's store (spell_ef_kit_stored_heal): `pct` percent (30; Ancient Reservoir 35/40) of a raw heal,
    // overhealing included, added to what it holds (kept in hundredths so small ticks still count).
    constexpr uint64_t StoreHundredths(uint32_t heal, int32_t pct)
    {
        return pct > 0 ? uint64_t(heal) * uint64_t(pct) : 0;
    }

    // The release: `pool` shared out over the injured allies in the order given (lowest health percent first): each takes
    // what it misses until the pool runs dry. `missing[i]` is ally i's missing health; `out[i]` gets its heal. Returns
    // what was handed out (the rest is lost: nobody needed it).
    inline uint64_t ReleaseSplit(uint64_t pool, uint32_t const* missing, uint32_t* out, std::size_t n)
    {
        uint64_t given = 0;
        for (std::size_t i = 0; i < n; ++i)
        {
            uint32_t const take = uint32_t(std::min<uint64_t>(pool - given, missing[i]));
            out[i] = take;
            given += take;
        }
        return given;
    }

    // The cooldown left after the release: `fullMs` (45 s) counted from the summon, `elapsedMs` ago (a release at 12 s
    // leaves 33 s; the automatic one at 20 s, 25 s). Never below 0.
    constexpr int32_t CooldownFromStart(int32_t fullMs, int64_t elapsedMs)
    {
        int64_t const left = int64_t(std::max<int32_t>(0, fullMs)) - std::max<int64_t>(0, elapsedMs);
        return int32_t(std::max<int64_t>(0, left));
    }
}
