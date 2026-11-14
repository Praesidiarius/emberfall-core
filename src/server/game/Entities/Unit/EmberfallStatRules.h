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

#ifndef EMBERFALL_STAT_RULES_H
#define EMBERFALL_STAT_RULES_H

// EMBERFALL: the stat rules (PATCHES.md #40; Emberfall docs/design/classes.md §4.10, Nathanael 2026-11-15). For players only,
// while a module has switched them on (mod-emberfall's emberfall_stat_rules, Emberfall.OwnData and Emberfall.Stats.Rules):
//   - Intellect gives spell power one for one, damage and healing, every school (Unit::SpellBaseDamageBonusDone,
//     SpellBaseHealingBonusDone; the character sheet's fields follow through Player::UpdateSpellDamageAndHealingBonus);
//   - Spirit is no stat: mana comes back at a flat rate of the level's pool a second (the module's table), in and out of
//     a fight, the same while casting (no five-second rule: Player::UpdateManaRegen writes one number into both of the
//     core's regen fields, so Player::Regenerate's "recent cast" choice picks the same value); the mana regen percent auras
//     scale that rate, the flat mana-a-five-seconds auras and items add to it as upstream;
//   - health out of combat comes from Vigour (the core's Stamina) where upstream reads Spirit (Player::OCTRegenHPPerSpirit,
//     the same gt tables: Emberfall's gtOCTRegenHP / gtRegenHPPerSpt hold 0.1, so Vigour / 10 a second).
// What a script hook reaches stays in the module: the mana pool (OnPlayerAfterUpdateMaxPower) and attack power from
// Strength (OnPlayerAfterUpdateAttackPowerAndDamage). No core header here: modules/mod-emberfall/tests/statrules_tests.cpp
// tests this file on its own.

#include <cstdint>

class Player;

namespace Emberfall::StatRules
{
    // The switch and the one number the core cannot reach by itself. A module sets it at config load; the default is
    // upstream's behaviour.
    struct Settings
    {
        bool on = false;
        // the mana a second of a player at its class and level (the module's ef_player_mana table); nullptr or a value
        // below 0: no flat rate known for that player, upstream's regen
        float (*manaRegenPerSecond)(Player const* player) = nullptr;
    };

    inline Settings& Current()
    {
        static Settings settings;
        return settings;
    }

    inline bool On()
    {
        return Current().on;
    }

    // Spell power from Intellect: one for one, never below 0.
    constexpr int32_t SpellPowerFromIntellect(float intellect)
    {
        return intellect > 0.0f ? int32_t(intellect) : 0;
    }

    // The mana a second written into both regen fields: the flat rate times the regen percent auras (a talent's +20 %:
    // 1.2), plus the flat auras' mana a five seconds (upstream's power_regen_mp5, already divided by five). Never below 0.
    constexpr float ManaRegenPerSecond(float flatPerSecond, float pctAuraMultiplier, float mp5PerSecond)
    {
        float const flat = flatPerSecond > 0.0f && pctAuraMultiplier > 0.0f ? flatPerSecond * pctAuraMultiplier : 0.0f;
        float const total = flat + mp5PerSecond;
        return total > 0.0f ? total : 0.0f;
    }
}

#endif
