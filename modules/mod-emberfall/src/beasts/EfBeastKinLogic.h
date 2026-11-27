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

#ifndef EMBERFALL_BEAST_KIN_LOGIC_H
#define EMBERFALL_BEAST_KIN_LOGIC_H

// The Hunter's beasts, part 2 (Emberfall docs/design/hunter-beasts.md §4-§5), pure rules, no core types
// (tests/beasts_tests.cpp): the archetypes' passives on the hunter and the beast families' specials. The numbers are
// the data's (the passive's effect values, spells/pet_archetypes.yaml; the family's rule, ef_pet_family_spell); these
// functions only say what they do with them.
//
//   EFB  s -> c  KIN <family>   the beast family of the companion out (0: none): its tooltips' archetype and special

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace Emberfall::Beasts
{
    // The archetypes' rules (pets.yaml archetypes `id`; pet_kin.py ARCHETYPES).
    enum class Kind : std::uint8_t { None = 0, Packrunner = 1, Prowler = 2, Heavyhide = 3, Skywing = 4, Venomkin = 5, Shellback = 6 };

    // A family special's rule (taming/families.yaml special_rule; pet_kin.py RULES).
    enum class Rule : std::uint8_t { None = 0, Busy = 1, Slowed = 2, Water = 3, OnCast = 4, NoBoss = 5, Pierce = 6 };

    constexpr Kind KindOf(std::uint32_t id) { return id >= 1 && id <= 6 ? Kind(id) : Kind::None; }
    constexpr Rule RuleOf(std::uint32_t id) { return id <= 6 ? Rule(id) : Rule::None; }

    struct FamilyKin
    {
        Kind archetype = Kind::None;
        std::uint32_t special = 0;
        Rule rule = Rule::None;
        std::uint32_t ruleValue = 0;
    };

    struct ArchetypeSpells
    {
        std::uint32_t passive = 0;   // the hunter's aura
        std::uint32_t helper1 = 0;   // Packrunner: the pace; Heavyhide: the companion's hold; Skywing: Marked;
        std::uint32_t helper2 = 0;   // Venomkin: the venom; Shellback: the ward. Skywing: the shots' reach
    };

    // ---- the specials ----------------------------------------------------------------------------------------------

    // Whether the priority may try the special now (a row the rule holds back is skipped like an unknown one).
    constexpr bool SpecialAllowed(Rule rule, bool victimCasting, bool victimBoss)
    {
        if (rule == Rule::OnCast)
            return victimCasting;
        if (rule == Rule::NoBoss)
            return !victimBoss;
        return true;
    }

    // The damage factor of a special's rule at the hit (1: none). Pierce: see PierceFactor.
    constexpr float SpecialBonus(Rule rule, std::uint32_t value, bool victimBusy, bool victimSlowed, bool inWater)
    {
        bool const holds = (rule == Rule::Busy && victimBusy) || (rule == Rule::Slowed && victimSlowed) || (rule == Rule::Water && inWater);
        return holds ? 1.0f + float(value) / 100.0f : 1.0f;
    }

    // 3.3.5's armour reduction of a blow by an attacker of `level` (the core's CalcArmorReducedDamage, capped at 75 %).
    inline float ArmourReduction(float armour, std::uint32_t level)
    {
        if (armour <= 0.0f)
            return 0.0f;
        float const l = float(level);
        float const k = l < 60.0f ? 400.0f + 85.0f * l : 400.0f + 85.0f * l + 4.5f * 85.0f * (l - 59.0f);
        return std::min(0.75f, armour / (armour + k));
    }

    // The factor that makes a blow the core will still reduce by the whole armour land as if `pct` % of it were gone
    // (Stooping Strike: through half the armour), applied before the reduction.
    inline float PierceFactor(float armour, std::uint32_t level, std::uint32_t pct)
    {
        float const full = ArmourReduction(armour, level);
        if (full <= 0.0f || full >= 1.0f)
            return 1.0f;
        float const part = ArmourReduction(armour * (1.0f - std::min(100u, pct) / 100.0f), level);
        return (1.0f - part) / (1.0f - full);
    }

    // The other families' specials the companion still knows (to be forgotten): every special of `all` it knows but its
    // own.
    template <class Knows>
    std::vector<std::uint32_t> SpecialsToForget(std::vector<std::uint32_t> const& all, std::uint32_t own, Knows&& knows)
    {
        std::vector<std::uint32_t> out;
        for (std::uint32_t s : all)
            if (s != own && knows(s) && std::find(out.begin(), out.end(), s) == out.end())
                out.push_back(s);
        return out;
    }

    // ---- the passives ----------------------------------------------------------------------------------------------

    // What the hunter's own blow meets: the passive's numbers (its effect values) and the fight around it.
    struct HunterHit
    {
        Kind kind = Kind::None;
        std::int32_t v1 = 0, v2 = 0, v3 = 0;  // the passive's effect values
        bool onCompanionTarget = false;       // Packrunner: the companion fights this enemy
        bool victimUnhurt = false;            // Prowler: the enemy is at full health
        bool direct = true;                   // a blow, not a tick of damage over time
        std::uint32_t venomStacks = 0;        // Venomkin: the companion's venom on the enemy
    };

    // The hunter's damage factor from the archetype (1: none).
    constexpr float HunterDamageFactor(HunterHit const& h)
    {
        switch (h.kind)
        {
            case Kind::Packrunner: return h.onCompanionTarget ? 1.0f + float(h.v1) / 100.0f : 1.0f;
            case Kind::Prowler: return h.direct && h.victimUnhurt ? 1.0f + float(h.v1) / 100.0f : 1.0f;
            case Kind::Venomkin: return 1.0f + float(h.v1) * float(h.venomStacks) / 100.0f;
            default: return 1.0f;
        }
    }

    // The damage factor on a blow the hunter takes (Heavyhide: v1 % less within v2 yards of the companion).
    constexpr float HunterTakenFactor(Kind kind, std::int32_t v1, std::int32_t v2, float companionYards)
    {
        if (kind != Kind::Heavyhide || companionYards < 0.0f || companionYards > float(v2))
            return 1.0f;
        return std::max(0.0f, 1.0f - float(v1) / 100.0f);
    }

    // The crit chance (hundredths of a percent, as the core's melee roll) a companion's swing gains (Prowler: v2 % on
    // an enemy above v3 % health).
    constexpr std::int32_t CompanionCritBonus(Kind kind, std::int32_t v2, std::int32_t v3, float victimHealthPct)
    {
        return kind == Kind::Prowler && victimHealthPct > float(v3) ? v2 * 100 : 0;
    }

    // Packrunner's pace: on within v2 yards of the companion.
    constexpr bool PaceHolds(Kind kind, std::int32_t v2, float companionYards)
    {
        return kind == Kind::Packrunner && companionYards >= 0.0f && companionYards <= float(v2);
    }

    // Shellback's ward: the hunter falls under v1 % health (from above it) and the last ward is a cooldown ago.
    constexpr bool WardFires(float healthPctBefore, float healthPctAfter, std::int32_t v1, std::uint64_t nowMs,
                             std::uint64_t lastMs, std::uint64_t cooldownMs)
    {
        return healthPctAfter < float(v1) && healthPctBefore >= float(v1) && (lastMs == 0 || nowMs >= lastMs + cooldownMs);
    }

    // The ward's absorb: v2 % of the hunter's health.
    constexpr std::int32_t WardAbsorb(std::uint32_t maxHealth, std::int32_t v2)
    {
        return std::int32_t(std::uint64_t(maxHealth) * std::uint64_t(std::max(0, v2)) / 100u);
    }

    // Shellback's ward cooldown (a minute, the passive's text).
    constexpr std::uint64_t kWardCooldownMs = 60000;

    inline std::string Kin(std::uint32_t family) { return "KIN " + std::to_string(family); }
}

#endif // EMBERFALL_BEAST_KIN_LOGIC_H
