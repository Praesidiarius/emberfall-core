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

// Emberfall companions (Emberfall docs/design/companions.md): the parts of the companion system that need no core,
// so tests/companion_tests.cpp checks them standalone: roles and the `.efparty` arguments, the formation, the level
// scaling numbers, rank choice, the healer's triage, the DPS start and threat gates, the EFC frame message.
// Everything here is plain C++20 over plain values.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Emberfall::Companions
{
    // ---- roles ---------------------------------------------------------------------------------------------------
    enum class Role : uint8_t { None = 0, Tank = 1, Healer = 2, Melee = 3, Ranged = 4 };

    // What `.efparty add` takes: a concrete role, or "dps" (melee or ranged, whichever archetype comes next).
    enum class RoleRequest : uint8_t { None, Tank, Healer, Dps, Melee, Ranged };

    inline std::string Lower(std::string_view s)
    {
        std::string out(s);
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return out;
    }

    inline RoleRequest ParseRole(std::string_view word)
    {
        std::string const w = Lower(word);
        if (w == "tank" || w == "t")
            return RoleRequest::Tank;
        if (w == "healer" || w == "heal" || w == "h")
            return RoleRequest::Healer;
        if (w == "dps" || w == "damage" || w == "d")
            return RoleRequest::Dps;
        if (w == "melee" || w == "m")
            return RoleRequest::Melee;
        if (w == "ranged" || w == "range" || w == "r")
            return RoleRequest::Ranged;
        return RoleRequest::None;
    }

    inline bool Fits(RoleRequest want, Role have)
    {
        switch (want)
        {
            case RoleRequest::Tank: return have == Role::Tank;
            case RoleRequest::Healer: return have == Role::Healer;
            case RoleRequest::Dps: return have == Role::Melee || have == Role::Ranged;
            case RoleRequest::Melee: return have == Role::Melee;
            case RoleRequest::Ranged: return have == Role::Ranged;
            case RoleRequest::None: return false;
        }
        return false;
    }

    inline char RoleLetter(Role r)
    {
        switch (r)
        {
            case Role::Tank: return 'T';
            case Role::Healer: return 'H';
            case Role::Melee: return 'M';
            case Role::Ranged: return 'R';
            default: return '?';
        }
    }

    inline char const* RoleName(Role r)
    {
        switch (r)
        {
            case Role::Tank: return "tank";
            case Role::Healer: return "healer";
            case Role::Melee: return "melee";
            case Role::Ranged: return "ranged";
            default: return "?";
        }
    }

    // ---- `.efparty add <role> [archetype] [count]` ---------------------------------------------------------------
    struct AddArgs
    {
        RoleRequest role = RoleRequest::None;
        std::string archetype; // empty: the role's archetypes in turn
        uint32_t count = 1;
        std::string error;     // non-empty: the arguments are wrong
    };

    inline std::vector<std::string> Words(std::string_view s)
    {
        std::vector<std::string> out;
        std::string cur;
        for (char c : s)
        {
            if (c == ' ' || c == '\t')
            {
                if (!cur.empty())
                    out.push_back(cur), cur.clear();
            }
            else
                cur += c;
        }
        if (!cur.empty())
            out.push_back(cur);
        return out;
    }

    inline std::optional<uint32_t> ParseCount(std::string const& w)
    {
        if (w.empty() || w.size() > 9 || !std::all_of(w.begin(), w.end(), [](unsigned char c) { return std::isdigit(c); }))
            return std::nullopt;
        return uint32_t(std::stoul(w));
    }

    // "<role> [archetype] [count]" and "<role> [count] [archetype]"; the count is 1-<maxCount>.
    inline AddArgs ParseAdd(std::string_view args, uint32_t maxCount)
    {
        AddArgs a;
        std::vector<std::string> const w = Words(args);
        if (w.empty())
        {
            a.error = "a role is missing (tank, healer, dps, melee or ranged)";
            return a;
        }
        a.role = ParseRole(w[0]);
        if (a.role == RoleRequest::None)
        {
            a.error = "unknown role '" + w[0] + "' (tank, healer, dps, melee or ranged)";
            return a;
        }
        bool haveCount = false;
        for (size_t i = 1; i < w.size(); ++i)
        {
            bool const digits = std::all_of(w[i].begin(), w[i].end(), [](unsigned char c) { return std::isdigit(c); });
            if (std::optional<uint32_t> n = ParseCount(w[i]); n && !haveCount)
            {
                a.count = *n;
                haveCount = true;
            }
            else if (a.archetype.empty() && !digits)
                a.archetype = Lower(w[i]);
            else
            {
                a.error = "too many arguments ('" + w[i] + "')";
                return a;
            }
        }
        if (a.count < 1 || a.count > maxCount)
            a.error = "the count must be 1 to " + std::to_string(maxCount);
        return a;
    }

    // ---- formation ------------------------------------------------------------------------------------------------
    // Where companion `index` (0-based, in summon order) follows its owner: distance and angle relative to the owner's
    // facing (0 = ahead, pi = behind). A loose fan behind the owner, the first ring at 2.5-3 yd, the next at 4.5 yd.
    struct Slot
    {
        float dist;
        float angle;
    };

    inline Slot FormationSlot(uint32_t index)
    {
        static constexpr float kPi = 3.14159265f;
        static constexpr Slot kSlots[] = {
            { 2.5f, kPi - 0.65f }, { 2.5f, kPi + 0.65f }, { 3.0f, kPi - 1.45f }, { 3.0f, kPi + 1.45f },
            { 4.5f, kPi },         { 4.5f, kPi - 0.4f },  { 4.5f, kPi + 0.4f },  { 5.5f, kPi - 0.9f },
        };
        constexpr uint32_t n = sizeof(kSlots) / sizeof(kSlots[0]);
        Slot s = kSlots[index % n];
        s.dist += 2.0f * float(index / n); // more than eight: further rings
        return s;
    }

    // ---- scaling --------------------------------------------------------------------------------------------------
    // The stock creature_classlevelstats row of the companion's level and class, as plain numbers.
    struct BaseStats
    {
        uint32_t health = 0;   // basehp of the expansion that applies
        uint32_t mana = 0;     // basemana (class 2/8 rows)
        float armor = 0;       // basearmor
        float damage = 0;      // damage_base of the expansion (weapon damage per hit of a normal creature)
        uint32_t attackPower = 0;
    };

    // An archetype's multipliers (ef_companion_archetype).
    struct ArchetypeMods
    {
        float health = 1, mana = 1, armor = 1, damage = 1, rangedDamage = 0;
        uint32_t attackMs = 2000;
    };

    struct Scaled
    {
        uint32_t health = 0, mana = 0, createMana = 0, armor = 0;
        float minDamage = 0, maxDamage = 0;            // main hand, per hit
        float minRanged = 0, maxRanged = 0;            // ranged weapon, per hit (0: none)
        float referenceDps = 0;                        // a normal creature of the level: its white damage per second
    };

    // A normal (non-elite) creature of this level and class swings for damage + AP/14 * 2 s per 2 s (Creature::SelectLevel,
    // BASE_ATTACK_TIME): that is the reference DPS. A companion's white damage per second is `mods.damage` times it, spread
    // over its own attack time; hits vary +-20 %. Health, armor and the mana pool are the base row times the multipliers.
    // Spell costs are percentages of the create mana, which is the class's base mana (a player-like budget), so the pool
    // (`mods.mana` x base mana) decides how many casts a companion has.
    inline Scaled Scale(BaseStats const& b, ArchetypeMods const& mods, uint32_t createManaBase)
    {
        Scaled s;
        s.health = std::max<uint32_t>(1, uint32_t(std::lround(float(b.health) * mods.health)));
        s.createMana = createManaBase;
        s.mana = uint32_t(std::lround(float(createManaBase) * mods.mana));
        s.armor = uint32_t(std::lround(b.armor * mods.armor));
        float const perHitRef = b.damage * 1.25f + float(b.attackPower) / 14.0f * 2.0f; // mean of 1.0-1.5 x damage + AP part
        s.referenceDps = perHitRef / 2.0f;
        float const attack = float(std::max<uint32_t>(mods.attackMs, 500)) / 1000.0f;
        float const perHit = s.referenceDps * mods.damage * attack;
        s.minDamage = perHit * 0.8f;
        s.maxDamage = perHit * 1.2f;
        if (mods.rangedDamage > 0)
        {
            // Ranged shots come every 2.5 s (the archetype's shoot spell), at rangedDamage x the reference DPS.
            float const perShot = s.referenceDps * mods.rangedDamage * 2.5f;
            s.minRanged = perShot * 0.8f;
            s.maxRanged = perShot * 1.2f;
        }
        return s;
    }

    // ---- ranks ----------------------------------------------------------------------------------------------------
    // A rank chain as (spell, required level) pairs in rank order; the highest rank the level reaches, or 0.
    inline uint32_t RankForLevel(std::vector<std::pair<uint32_t, uint32_t>> const& chain, uint32_t level)
    {
        uint32_t best = 0;
        for (auto const& [spell, need] : chain)
            if (need <= level)
                best = spell;
        return best;
    }

    // ---- healer triage --------------------------------------------------------------------------------------------
    enum class HealKind : uint8_t { None, Fast, Big, Small, Hot, Shield };

    struct HealTarget
    {
        uint32_t id = 0;        // the caller's handle
        float hpPct = 100;      // 0-100
        bool tank = false;
        bool alive = true;
        bool hasHot = false;    // our heal-over-time is on it
        bool hasShield = false; // our shield (or its lockout) is on it
        bool inRange = true;    // within heal range and in sight
    };

    struct HealConfig
    {
        float fastBelow = 35;    // a fast heal below this
        float bigBelow = 60;     // a big (slow, efficient) heal below this (the tank: tankBigBelow)
        float tankBigBelow = 75;
        float smallBelow = 80;   // a small heal below this (the tank: tankBigBelow covers it first)
        float hotBelow = 92;     // a heal-over-time below this, when none is on
        float shieldTankBelow = 101; // the tank gets the shield whenever it is off (in combat)
        float tankWeight = 1.35f; // the tank's deficit counts this much more
        float lowManaPct = 25;   // below this mana: only fast/big heals under bigBelow, no HoT, no shield
    };

    struct HealChoice
    {
        HealKind kind = HealKind::None;
        uint32_t id = 0;
    };

    // The member with the largest weighted deficit, and what it gets. `inCombat`: shields only in combat.
    inline HealChoice ChooseHeal(std::vector<HealTarget> const& members, float manaPct, bool inCombat, HealConfig const& c = {})
    {
        HealTarget const* worst = nullptr;
        float worstScore = 0;
        for (HealTarget const& m : members)
        {
            if (!m.alive || !m.inRange)
                continue;
            float const deficit = 100.0f - m.hpPct;
            float const score = deficit * (m.tank ? c.tankWeight : 1.0f);
            if (deficit > 0.5f && (!worst || score > worstScore))
            {
                worst = &m;
                worstScore = score;
            }
        }
        bool const lowMana = manaPct < c.lowManaPct;
        if (worst)
        {
            float const hp = worst->hpPct;
            if (hp < c.fastBelow)
                return { HealKind::Fast, worst->id };
            if (hp < (worst->tank ? c.tankBigBelow : c.bigBelow))
                return { HealKind::Big, worst->id };
            if (!lowMana && hp < c.smallBelow)
                return { HealKind::Small, worst->id };
        }
        if (lowMana)
            return {};
        // Nobody needs a direct heal: a shield on the tank in combat, then a HoT on whoever is below hotBelow (tank first).
        if (inCombat)
            for (HealTarget const& m : members)
                if (m.alive && m.inRange && m.tank && !m.hasShield && m.hpPct < c.shieldTankBelow)
                    return { HealKind::Shield, m.id };
        HealTarget const* hot = nullptr;
        for (HealTarget const& m : members)
            if (m.alive && m.inRange && !m.hasHot && m.hpPct < c.hotBelow)
                if (!hot || (m.tank && !hot->tank) || (m.tank == hot->tank && m.hpPct < hot->hpPct))
                    hot = &m;
        if (hot)
            return { HealKind::Hot, hot->id };
        return {};
    }

    // ---- DPS gates ------------------------------------------------------------------------------------------------
    struct DpsGate
    {
        uint32_t waitMs = 2000;   // the tank must have held the target this long ...
        float waitThreat = 250;   // ... or have this much threat on it
        float threatCap = 0.80f;  // a DPS holds off while its threat is at least this share of the tank's
    };

    // May a DPS start on a target the tank has held for `tankHeldMs` (0: the tank does not have it) with `tankThreat`?
    // A target nobody tanks (no tank at all, or the tank is dead) is fair game at once.
    inline bool MayStart(DpsGate const& g, bool haveTank, uint32_t tankHeldMs, float tankThreat)
    {
        if (!haveTank)
            return true;
        return tankHeldMs >= g.waitMs || (tankHeldMs > 0 && tankThreat >= g.waitThreat);
    }

    // Must a DPS pause its damage on the target? Only while the tank holds it (a loose mob is the tank's to pick up; the
    // DPS keeps hitting what attacks it) and only when the DPS's threat is close to the tank's: at `threatCap` of it, or
    // when its next hit (`nextHit`, its recent biggest) would take it past 105 % (the core takes a mob away at 110 % in
    // melee range, 130 % outside).
    inline bool MustHold(DpsGate const& g, bool tankHolds, float myThreat, float tankThreat, float nextHit = 0.0f)
    {
        if (!tankHolds)
            return false;
        float const tank = std::max(tankThreat, 1.0f);
        return myThreat >= g.threatCap * tank || myThreat + nextHit >= 1.05f * tank;
    }

    // ---- regen between pulls --------------------------------------------------------------------------------------
    // Out of combat a companion eats/drinks below these and stops once both are above `untilPct`.
    struct RegenRule
    {
        float eatBelow = 80, drinkBelow = 70, untilPct = 98, pctPerSecond = 6;
    };

    inline bool WantsRegen(RegenRule const& r, float hpPct, float manaPct, bool usesMana)
    {
        return hpPct < r.eatBelow || (usesMana && manaPct < r.drinkBelow);
    }

    inline bool RegenDone(RegenRule const& r, float hpPct, float manaPct, bool usesMana)
    {
        return hpPct >= r.untilPct && (!usesMana || manaPct >= r.untilPct);
    }

    // ---- the EFC frame message ------------------------------------------------------------------------------------
    // Server -> client, a whisper to oneself in LANG_ADDON with prefix "EFC" (Emberfall's client hands it to the UI as
    // CHAT_MSG_ADDON; companions.md §5): "EFC\t1;<count>;<entry>;<entry>..." with one entry per companion in summon
    // order: "<guid hex>,<role letter>,<archetype>,<hp %>,<mana % or ->,<state>" where state is one of
    // F follow, S stay, C combat, E eat/drink, D dead. At most 251 bytes after the prefix: entries that do not fit are left
    // out (the count still says how many there are).
    struct FrameEntry
    {
        uint64_t guid = 0;
        Role role = Role::None;
        std::string archetype;
        uint32_t hpPct = 0;
        int32_t manaPct = -1; // -1: no mana
        char state = 'F';
    };

    inline std::string Hex(uint64_t v)
    {
        static constexpr char kDigits[] = "0123456789ABCDEF";
        std::string s(16, '0');
        for (int i = 15; i >= 0; --i, v >>= 4)
            s[size_t(i)] = kDigits[v & 0xF];
        return s;
    }

    inline std::string FrameBody(std::vector<FrameEntry> const& entries, size_t maxBytes = 251)
    {
        std::string body = "1;" + std::to_string(entries.size());
        for (FrameEntry const& e : entries)
        {
            std::string arch = e.archetype.substr(0, 12);
            std::replace(arch.begin(), arch.end(), ';', '_');
            std::replace(arch.begin(), arch.end(), ',', '_');
            std::string const item = ";" + Hex(e.guid) + "," + RoleLetter(e.role) + "," + arch + "," +
                                     std::to_string(std::min<uint32_t>(e.hpPct, 100)) + "," +
                                     (e.manaPct < 0 ? std::string("-") : std::to_string(std::min<int32_t>(e.manaPct, 100))) + "," +
                                     e.state;
            if (body.size() + item.size() > maxBytes)
                break;
            body += item;
        }
        return body;
    }
}
