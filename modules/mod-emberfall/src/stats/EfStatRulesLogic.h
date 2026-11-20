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

// Emberfall: the stat rules (Emberfall docs/design/classes.md §4.10, Nathanael 2026-11-15; the core's PATCHES.md #40 does
// what no hook reaches). Pure: the attack power of a player's stats, the mana pool of its class and level in place of
// upstream's Intellect bonus, the table rows. No core types, so tests/statrules_tests.cpp runs it without a server.
//
// Upstream (3.3.5a, StatSystem.cpp Player::UpdateAttackPowerAndDamage), the attack power's base by class:
//   warrior, paladin       3 x level + 2 x Strength - 20         ranged (warrior): level + Agility - 10
//   hunter                 2 x level + Strength + Agility - 20   ranged: 2 x level + Agility - 10
//   druid (no form)        2 x Strength - 20                     ranged: Agility - 10
//   mage, warlock          Strength - 10                         ranged: Agility - 10
// Here: two attack power a point of Strength for every class, melee and ranged alike (Nathanael 2026-11-20, Emberfall
// feedback #679: "1 strength = 2 attack power"; one for one from 2026-11-15 until then; Emberfall.Stats.StrengthAttackPower
// sets it, default STRENGTH_ATTACK_POWER); a hunter (whose items carry Agility, the class's main stat) adds its Agility one
// for one [proposal]. No level term: from level 1 the rule gives a warrior 48 against upstream's 31 and a mage 36 against
// upstream's 8 (the swing is the auto attack's level rule anyway).
// Talents and auras (the TOTAL modifiers: the Earthwarden's attack power from Agility in its form) stay on top.
// Upstream's mana pool: create mana (player_class_stats BaseMana, which the costs are percents of) plus Intellect's bonus
// (20 + 15 a point above 20). Here: the class and level's pool (ef_player_mana) in place of both; the max-mana auras
// (talents) stay on top.

#ifndef EF_STAT_RULES_LOGIC_H
#define EF_STAT_RULES_LOGIC_H

#include <cstdint>

namespace Emberfall::StatRules
{
    constexpr uint8_t CLASS_HUNTER_ID = 3;   // SharedDefines.h CLASS_HUNTER

    // Attack power a point of Strength (Emberfall's formulas.yaml player_stats.attack_power_per_strength; 2 since
    // 2026-11-20, feedback #679). The config's Emberfall.Stats.StrengthAttackPower overrides it (1: the rule of 2026-11-15).
    constexpr float STRENGTH_ATTACK_POWER = 2.0f;

    // The base attack power (upstream's val2, before the BASE_PCT modifiers) of a player's Strength and Agility: Strength
    // times `perStrength`, a hunter's Agility one for one; the same for melee and ranged. Never below 0.
    constexpr float AttackPowerBase(uint8_t classId, float strength, float agility, float perStrength = STRENGTH_ATTACK_POWER)
    {
        float ap = strength > 0.0f && perStrength > 0.0f ? strength * perStrength : 0.0f;
        if (classId == CLASS_HUNTER_ID && agility > 0.0f)
            ap += agility;
        return ap;
    }

    // The base attack power Player::UpdateAttackPowerAndDamage goes on with: with the rules on, the rule's base times
    // upstream's BASE_PCT modifier; with them off, upstream's own base (its class formula), untouched.
    constexpr float AttackPowerUnderRules(bool rulesOn, uint8_t classId, float strength, float agility, float perStrength,
                                          float basePct, float upstreamBase)
    {
        return rulesOn ? AttackPowerBase(classId, strength, agility, perStrength) * basePct : upstreamBase;
    }

    // The max mana upstream computed, `value` = ((flat base + create) x basePct + flat total + intellectBonus) x totalPct,
    // with the class and level's pool in place of create mana and no Intellect bonus:
    // ((flat base + pool) x basePct + flat total) x totalPct. Never below 0.
    constexpr float PoolInPlaceOfIntellect(float value, float createMana, float pool, float intellectBonus,
                                           float basePct, float totalPct)
    {
        float const out = value + ((pool - createMana) * basePct - intellectBonus) * totalPct;
        return out > 0.0f ? out : 0.0f;
    }

    // One ef_player_mana row: a class at a level, its mana pool and its mana a second.
    struct ManaRow
    {
        uint8_t classId = 0;
        uint8_t level = 0;
        float pool = 0.0f;
        float regen = 0.0f;
    };

    // Whether a row can be used: a known class and level, a pool above 0 and a regen not below 0.
    constexpr bool ValidRow(ManaRow const& row)
    {
        return row.classId > 0 && row.level > 0 && row.pool > 0.0f && row.regen >= 0.0f;
    }

    // The key of a class and level in the module's map.
    constexpr uint16_t RowKey(uint8_t classId, uint8_t level)
    {
        return uint16_t(uint16_t(classId) << 8 | level);
    }

    // The item stat types (ItemTemplate.h ItemModType) no item gives any more: Spirit (6). Rule 5: the module drops it
    // from any item, so old data shows nothing and gives nothing.
    constexpr bool DroppedItemStat(uint32_t statType)
    {
        return statType == 6;   // ITEM_MOD_SPIRIT
    }
}

#endif
