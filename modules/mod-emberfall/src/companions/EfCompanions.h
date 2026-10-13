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

// Emberfall companions (Emberfall docs/design/companions.md): AI party members for testing, summoned by a GM with
// `.efparty`. A companion is a Guardian (SummonProperties 61: ally, guardian, no slot) of its owner: it follows, takes the
// owner's faction, gives the owner kill credit, has no threat list of its own and is unsummoned with the owner. Its
// archetype (ef_companion_archetype) gives the creature, the role, the level scaling and the spells (ef_companion_spell,
// by kind and priority; rank-1 IDs, the module picks the highest rank the level reaches). Switch:
// Emberfall.Companions.Enable (default 1; GM-only command, so on everywhere the data is).

#include "CreatureAI.h"
#include "DataMap.h"
#include "EfCompanionLogic.h"
#include "ObjectGuid.h"
#include "Position.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

class Creature;
class Player;
class Unit;

namespace Emberfall::Companions
{
    // What a spell row is for (ef_companion_spell.kind). The AI of the role decides when a kind applies.
    enum class Kind : uint8
    {
        None,
        Taunt,      // tank: a mob not on the tank
        AoeThreat,  // tank: min_targets mobs within 8 yd
        Threat,     // tank: on its target
        Nuke,       // damage on the target (healer: only when idle with mana)
        Aoe,        // damage, min_targets mobs near the target
        Behind,     // melee: only from behind the target
        Shoot,      // ranged auto attack (its cooldown is the shot interval)
        Dot,        // damage over time, when not on the target
        HealFast,
        HealBig,
        HealSmall,
        Hot,
        Shield,     // with Weakened Soul (6788) as the lockout
        Dispel,     // dispels magic (or disease, by the spell's dispel type) from a group member
        Res,        // out of combat, a dead group member
    };

    Kind ParseKind(std::string const& s);
    char const* KindName(Kind k);

    struct SpellRow
    {
        uint32 spell = 0;      // the spell, or the first rank of a chain
        Kind kind = Kind::None;
        uint8 priority = 0;    // lower first within a kind
        uint8 minLevel = 1, maxLevel = 255;
        uint32 cooldownMs = 0; // on top of the spell's own (recovery and category) cooldowns
        uint8 minTargets = 0;  // Aoe, AoeThreat
    };

    struct Archetype
    {
        uint8 id = 0;
        std::string name;      // `.efparty add dps mage`
        Role role = Role::None;
        uint32 entry = 0;      // creature_template
        uint8 unitClass = 1;   // creature_classlevelstats class for health, armor, damage
        ArchetypeMods mods;
        float threatMod = 1;   // damage threat multiplier (the AI adds (threatMod - 1) x damage)
        bool freeCost = false; // casts without power cost (rage/energy kits)
        bool usesMana = false;
        float range = 0;       // 0 melee; else the distance it fights from
        std::vector<SpellRow> spells;
    };

    struct Config
    {
        bool enable = true;
        uint32 security = 2;          // minimum account security for `.efparty`
        uint32 maxPerPlayer = 8;
        DpsGate gate;
        float leash = 40.0f;          // in combat, farther than this from the owner: break off and come back
        float teleportBeyond = 80.0f; // farther than this (out of combat) or on another floor: teleport to the owner
        uint32 frameMs = 500;         // the EFC frame message, at most this often
        bool noProximityAggro = true; // out of combat companions are immune to NPCs (mobs never aggro on them first)
        float healRange = 40.0f;
    };

    // ---- data and config -------------------------------------------------------------------------------------------
    void ReadConfig(bool reload);
    Config const& Cfg();
    bool Load();                             // ef_companion_* -> archetypes; false (logged) when missing or broken
    bool Enabled();
    Archetype const* FindArchetype(uint8 id);
    Archetype const* FindArchetype(std::string const& name);
    std::vector<Archetype const*> Archetypes();
    uint32 RankFor(uint32 spell, uint8 level); // the highest rank of spell's chain the level reaches (or spell itself)

    // ---- the owner's roster (Player CustomData) -------------------------------------------------------------------
    struct MobTrack
    {
        uint32 tankHeldMs = 0;   // continuous time the tank has been the mob's victim
        uint32 firstSeenMs = 0;  // owner clock
        ObjectGuid lastVictim;
        std::string name;
    };

    struct FightStats
    {
        uint32 fights = 0, kills = 0, fightMs = 0, killMsSum = 0, aggroLosses = 0, companionDeaths = 0, ownerDeaths = 0;
        uint64 mobMs = 0, mobMsOnTank = 0;  // engaged-mob time, and the part with the tank as the mob's victim
        float minLead = 0;                  // lowest tank threat / next-highest threat on a mob the tank holds (0: none)
        double leadSum = 0;
        uint32 leadSamples = 0;
        float healerManaMin = 101;          // lowest healer mana % seen
        std::map<std::string, uint32> lossesTo; // aggro losses by the archetype (or "owner") that took the mob
    };

    struct Member
    {
        ObjectGuid guid;
        uint8 archetype = 0;
        bool dead = false;
        Position deathPos;
        uint64 damage = 0, healing = 0;
    };

    class Roster : public DataMap::Base
    {
    public:
        std::vector<Member> members;  // summon order: the formation slot is the index
        bool stay = false;
        bool assist = true;
        uint32 clockMs = 0;
        uint32 frameTimer = 0, frameHeartbeat = 0, trackTimer = 0;
        std::string lastFrame;
        bool inFight = false;
        bool resummonPending = false; // the owner died: the core unsummoned the guardians (Unit::RemoveAllControlled)
        uint32 fightStartMs = 0;
        std::map<ObjectGuid, MobTrack> mobs;
        FightStats stats;
    };

    Roster* GetRoster(Player* owner, bool create);

    // ---- the AI's view (EfCompanionAI.cpp implements it) ---------------------------------------------------------
    class CompanionAIBase : public CreatureAI
    {
    public:
        explicit CompanionAIBase(Creature* c) : CreatureAI(c) { }
        virtual void Configure(Archetype const* arch, uint32 index, ObjectGuid owner) = 0;
        virtual char FrameState() const = 0;   // F, S, C, E (D is the roster's)
        virtual uint8 ArchetypeId() const = 0;
        virtual std::string Debug() const = 0; // the last decision, for `.efparty list`
    };

    CompanionAIBase* AIOf(Creature* c);

    // ---- operations --------------------------------------------------------------------------------------------------
    // `.efparty add`: summons the companions; the lines say what happened.
    std::vector<std::string> Add(Player* owner, AddArgs const& args);
    std::vector<std::string> List(Player* owner);
    std::vector<std::string> Remove(Player* owner, std::string const& which); // "all" or a 1-based number
    void SetStay(Player* owner, bool stay);
    void SetAssist(Player* owner, bool assist);
    uint32 Resync(Player* owner);            // re-scales every living companion to the owner's level
    void Resummon(Player* owner);            // after a map change: summons the roster again next to the owner
    void DespawnAll(Player* owner, bool forget);
    // A dead companion back (the healer's resurrection): summoned where it died with hpPct of its health and mana.
    Creature* Revive(Player* owner, ObjectGuid guid, float hpPct);
    std::vector<std::pair<ObjectGuid, Position>> DeadMembers(Player* owner);
    std::vector<std::string> StatsLines(Player* owner);
    void ResetStats(Player* owner);
    std::vector<std::string> ThreatLines(Player* owner); // per engaged mob: each group member's threat

    // Where formation slot `index` is next to the owner now (at the owner's height).
    Position FormationPoint(Player* owner, uint32 index);

    // Applies the level scaling (EfCompanionLogic.h Scale) to a summoned companion.
    void ApplyScaling(Creature* c, Archetype const& arch, uint8 level);

    // Every 100 ms-ish from OnPlayerUpdate: the mob tracker (tank hold times, stats) and the EFC frame message.
    void OnOwnerUpdate(Player* owner, uint32 diff);
    void OnCompanionDied(Creature* c);
    void OnDamage(Creature* c, uint32 damage);
    void OnHeal(Creature* c, uint32 healing);
    void OnOwnerDied(Player* owner);

    // ---- the group, as the AIs see it ----------------------------------------------------------------------------
    Player* OwnerOf(Creature const* c);
    std::vector<Creature*> LiveCompanions(Player* owner);
    // The owner, its living companions and its party members on the map within `range` of the owner.
    std::vector<Unit*> GroupUnits(Player* owner, float range = 100.0f);
    bool InGroup(Player* owner, Unit const* u);
    // The group's tank: a living tank companion, else the owner (the owner tanks unless a tank companion is out).
    Unit* Tank(Player* owner);
    bool OwnerIsTank(Player* owner);
    // Living hostile creatures that have a group member on their threat list (or attack one), within 60 yd of the owner.
    std::vector<Unit*> EngagedEnemies(Player* owner);
    // How long (ms) the tank has continuously held this mob (0: not on the tank), from the tracker.
    uint32 TankHeldMs(Player* owner, Unit const* mob);
    // The mob is crowd-controlled in a way damage would break (polymorph, sap, stasis-like stuns, fear, sleep ...).
    bool IsCrowdControlled(Unit const* mob);
}
