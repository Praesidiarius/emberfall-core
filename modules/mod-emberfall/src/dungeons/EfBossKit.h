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

// The boss mechanics toolkit, core side (Emberfall docs/design/dungeon-barrows.md §5b): what a BossAI script calls for an
// intermission (the immune shroud), an energy bar on the target frame, a soak circle (a marker creature that resolves
// itself and tells its boss), an arena that has shrunk (its edge pulses, the companions keep inside). The state and the
// rules are EfBossToolkit.h (no core; tests/dungeon_tests.cpp). Every soak and hazard is registered for the companions
// here too (EfCompanions.h RegisterSoak, RegisterHazard), so a new dungeon's mechanic is played by them from its first day.

#ifndef EMBERFALL_BOSS_KIT_H
#define EMBERFALL_BOSS_KIT_H

#include "Define.h"
#include "EfBossToolkit.h"

#include <vector>

class Creature;
struct Position;
class TempSummon;
class Unit;
class WorldObject;

namespace Emberfall::Bosses
{
    // The group's side (players and their companions) alive within `range` of `src`.
    std::vector<Unit*> GroupSideNear(WorldObject* src, float range);
    // ... and of those, the ones within `radius` (2D, their own size counted) of (x, y).
    std::vector<Unit*> GroupSideIn(WorldObject* src, float x, float y, float radius);

    // An intermission: the boss takes no damage (`shroud`: an aura of damage taken -100 % with its look), stops
    // fighting, walks to `to` if given and stands there. EndImmune gives it back its fight.
    void BeginImmune(Creature* boss, uint32 shroud, Position const* to);
    void EndImmune(Creature* boss, uint32 shroud);

    // The energy bar on the boss's target frame (0-100). The core does not refill a creature's energy (no
    // UNIT_FLAG2_REGENERATE_POWER), so the script's EnergyBar is the one truth and this only shows it.
    void ShowEnergy(Creature* boss, uint32 energy);

    // An arena that has shrunk: `spell` (triggered, from the boss) on every one of the group outside the ring; the
    // companions are told the ring (SetArena) so they keep inside it. Returns how many were outside.
    uint32 PulseOutsideArena(Creature* boss, Point2 centre, float radius, uint32 spell);
    void SetArena(Creature* boss, Point2 centre, float radius);
    void ClearArena(Creature* boss);

    // A soak circle: a marker creature (`entry`, script npc_ef_soak_marker) carrying `aura` (its look) for `fuseMs`; when
    // it goes off, `total` damage of `hitSpell` (a custom base point 0) is shared among the group's side within `radius`
    // (ResolveSoak); with fewer than `needed` there it casts `failSpell` instead and tells its boss (SetData).
    struct SoakDef
    {
        uint32 entry = 0;
        uint32 aura = 0;
        float radius = 0;
        uint32 needed = 1;
        uint32 fuseMs = 0;
        uint32 hitSpell = 0;
        uint32 total = 0;
        uint32 floor = 0;
        uint32 failSpell = 0;   // 0: none (the boss's SetData does the failure)
    };
    void RegisterSoakDef(SoakDef const& def);
    SoakDef const* FindSoakDef(uint32 entry);
    TempSummon* SummonSoak(Creature* boss, uint32 entry, float x, float y, float z);

    // What a soak marker tells its summoner (CreatureAI::SetData): the id, the marker's entry or the soakers.
    constexpr uint32 kSoakFailedData = 9001;   // value: the marker's entry
    constexpr uint32 kSoakDoneData = 9002;     // value: how many shared it

    // A plain marker (script npc_ef_boss_marker): a shelter, a ring's middle. It only carries the aura it is given.
    TempSummon* SummonMarker(Creature* boss, uint32 entry, float x, float y, float z, uint32 aura, uint32 lifeMs);
    // A ground caster (a marker too): casts `spell` (a telegraphed ground circle) at its own spot, so a boss can have
    // several telegraphs on the floor at once (a creature casts one spell at a time). Register its entry once
    // (RegisterGroundCaster) and the companions read its casts like an enemy's.
    TempSummon* SummonGroundCaster(Creature* boss, uint32 entry, float x, float y, float z, uint32 spell, uint32 lifeMs);
    void RegisterGroundCaster(uint32 entry);
    // A moving hazard: the companions keep `radius` from any hostile carrying `aura`.
    void RegisterHazardAura(uint32 aura, float radius);
}

void AddEmberfallBossKitScripts();

#endif
