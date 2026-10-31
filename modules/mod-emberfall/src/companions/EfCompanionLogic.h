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

    // ---- boss mechanics (companions.md §3 "Mechanics", dungeon-barrows.md §5a) -------------------------------------
    // Plain 2D geometry: the AI turns a hostile's telegraphed cast (a cone, a circle round the caster, a circle at a
    // point) into a danger area and walks out of it before the cast ends; a run-out mark sends its bearer away from the
    // group and the others away from the bearer.
    struct Vec2
    {
        float x = 0, y = 0;
    };

    inline float Dist(Vec2 a, Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

    // The smallest absolute difference between two angles (radians), 0..pi.
    inline float AngleDiff(float a, float b)
    {
        float d = std::fmod(std::fabs(a - b), 6.2831853f);
        return d > 3.14159265f ? 6.2831853f - d : d;
    }

    enum class DangerShape : uint8_t { None, Cone, Circle };

    // A telegraphed area: a cone from `origin` facing `facing` (full `angle`, radians) or a circle round `origin`.
    struct Danger
    {
        DangerShape shape = DangerShape::None;
        Vec2 origin;
        float radius = 0;    // the effect radius plus the caster's and our own size (the core's test, mob-abilities.md §3)
        float facing = 0;    // cone only
        float angle = 0;     // cone only
    };

    // Inside, with `margin` yards to spare (a step short of the edge still counts as inside).
    inline bool InDanger(Danger const& d, Vec2 p, float margin)
    {
        float const dist = Dist(d.origin, p);
        if (d.shape == DangerShape::Circle)
            return dist <= d.radius + margin;
        if (d.shape != DangerShape::Cone || dist > d.radius + margin)
            return false;
        if (dist < 1.0f)
            return true;   // at the caster's feet: inside its boundary, which the core counts as in front
        float const toP = std::atan2(p.y - d.origin.y, p.x - d.origin.x);
        // The cone's edge widened by the margin at this distance.
        return AngleDiff(toP, d.facing) <= d.angle * 0.5f + std::atan2(margin, dist);
    }

    // Where to step to get out: for a circle straight out from its middle to `clear` yards past the edge (from the very
    // middle towards `prefer`); for a cone round the side nearer to us, past its edge by an angle worth `clear` yards,
    // at least 4 yd from the caster. The shortest walk that ends outside.
    inline Vec2 EscapePoint(Danger const& d, Vec2 p, float clear, Vec2 prefer)
    {
        float const dist = Dist(d.origin, p);
        if (d.shape == DangerShape::Circle)
        {
            float a = std::atan2(p.y - d.origin.y, p.x - d.origin.x);
            if (dist < 0.3f)
                a = std::atan2(prefer.y - d.origin.y, prefer.x - d.origin.x);
            float const r = d.radius + clear;
            return Vec2{ d.origin.x + std::cos(a) * r, d.origin.y + std::sin(a) * r };
        }
        float const r = std::max(dist, 4.0f);
        float const toP = dist < 0.3f ? std::atan2(prefer.y - d.origin.y, prefer.x - d.origin.x) : std::atan2(p.y - d.origin.y, p.x - d.origin.x);
        // Which side: the sign of the angle from the facing to us (wrapped to -pi..pi).
        float const rel = std::remainder(toP - d.facing, 6.2831853f);
        float const side = rel >= 0 ? 1.0f : -1.0f;
        float const out = d.angle * 0.5f + std::atan2(clear, r) + 0.15f;
        float const a = d.facing + side * out;
        return Vec2{ d.origin.x + std::cos(a) * r, d.origin.y + std::sin(a) * r };
    }

    // A run-out mark on `self`: a spot `walk` yards away that is `radius` + `clear` yards from every other member, trying
    // eight directions starting away from the others' middle; else the one farthest from them. `open(point)`: the spot
    // is walkable (line of sight, floor).
    template <typename Open>
    inline Vec2 RunOutPoint(Vec2 self, std::vector<Vec2> const& others, float radius, float clear, float walk, Open const& open)
    {
        Vec2 mid = self;
        if (!others.empty())
        {
            mid = Vec2{};
            for (Vec2 const& o : others)
            {
                mid.x += o.x;
                mid.y += o.y;
            }
            mid.x /= float(others.size());
            mid.y /= float(others.size());
        }
        float const away = Dist(mid, self) > 0.3f ? std::atan2(self.y - mid.y, self.x - mid.x) : 0.0f;
        Vec2 best = self;
        float bestGap = -1;
        for (int i = 0; i < 8; ++i)
        {
            // away, then +-45, +-90, +-135, back
            int const k = (i + 1) / 2 * (i % 2 ? 1 : -1);
            float const a = away + float(k) * 0.7853982f;
            Vec2 const c{ self.x + std::cos(a) * walk, self.y + std::sin(a) * walk };
            if (!open(c))
                continue;
            float gap = 1e9f;
            for (Vec2 const& o : others)
                gap = std::min(gap, Dist(o, c));
            if (gap >= radius + clear)
                return c;
            if (gap > bestGap)
            {
                bestGap = gap;
                best = c;
            }
        }
        return best;
    }

    // Is someone else's run-out mark a danger to us: within its radius plus a margin.
    inline bool NearRunOut(Vec2 bearer, Vec2 self, float radius, float margin) { return Dist(bearer, self) <= radius + margin; }

    // A soak (the boss toolkit, dungeon-barrows.md §5b): `needed` of the group in it when it goes off (0: a shelter,
    // everyone). `inside`: the group members already in it (us not counted); `rank`: our place among the companions
    // outside it that may soak (the tank does not, except in a shelter), nearest first. Whether we go in (or, inside
    // already, stay).
    inline bool ShouldSoak(uint32_t needed, uint32_t inside, uint32_t rank)
    {
        if (!needed)
            return true;
        return inside < needed && rank < needed - inside;
    }

    // When to walk into a soak: in its last `leadMs` (its circle stays clear of the group until then, the healer keeps
    // healing), or at once for a shelter, which is up for one cast only.
    inline bool SoakTime(uint32_t needed, uint32_t msLeft, uint32_t leadMs) { return !needed || msLeft <= leadMs; }

    // Back inside an arena that has shrunk: from outside `radius` - `margin`, the point `margin` + 2 yd inside its edge
    // on the line to its middle; `self` itself when it is inside already.
    inline Vec2 ArenaReturnPoint(Vec2 self, Vec2 centre, float radius, float margin)
    {
        float const d = Dist(self, centre);
        if (d <= radius - margin)
            return self;
        float const r = std::max(radius - margin - 2.0f, 0.0f);
        if (d < 0.01f)
            return centre;
        return Vec2{ centre.x + (self.x - centre.x) / d * r, centre.y + (self.y - centre.y) / d * r };
    }

    // Interrupts: whether to kick a cast now. A cast is worth a kick once it has run `reactMs` (a human's reaction) and
    // while it has more than `minLeftMs` to go (the kick must land before it ends). Bosses' casts before anyone else's:
    // while a boss is in the fight, a companion keeps its interrupt for the boss.
    struct KickRule
    {
        uint32_t reactMs = 500;
        uint32_t minLeftMs = 250;
        uint32_t minCastMs = 1000;    // shorter casts are not worth the cooldown
    };

    inline bool WorthKicking(KickRule const& r, uint32_t castMs, uint32_t leftMs, bool casterIsBoss, bool bossInFight)
    {
        if (castMs < r.minCastMs || leftMs <= r.minLeftMs || castMs - std::min(castMs, leftMs) < r.reactMs)
            return false;
        return casterIsBoss || !bossInFight;
    }
}
