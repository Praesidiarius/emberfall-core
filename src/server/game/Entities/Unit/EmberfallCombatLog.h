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

#ifndef EMBERFALL_COMBAT_LOG_H
#define EMBERFALL_COMBAT_LOG_H

// EMBERFALL: the combat log tap (PATCHES.md #12; Emberfall docs/design/damage-meter.md): the combat log at its source. Every place that
// sends a combat log packet (SMSG_SPELLNONMELEEDAMAGELOG, SMSG_PERIODICAURALOG, SMSG_ATTACKERSTATEUPDATE,
// SMSG_SPELLLOGMISS, SMSG_SPELLHEALLOG, SMSG_SPELLDAMAGESHIELD, SMSG_ENVIRONMENTALDAMAGELOG, the interrupt and dispel
// logs) also hands the event to one sink, set by a module (mod-emberfall's damage meter), before range limits: the
// server sees every event, whoever stands near. Shield absorbs, which no packet names the caster of, come from
// Unit::CalcAbsorbResist. Without a sink (the default) each site costs one relaxed atomic load.
//
// The sink runs on the map thread of the event's units; the Unit pointers are valid for the call only.

#include "Define.h"
#include "ObjectGuid.h"

#include <atomic>

class Unit;

namespace Emberfall::CombatLog
{
    enum class Kind : uint8
    {
        SpellDamage,     // a spell's direct damage (amount, overkill, absorbed, resisted, blocked, crit)
        PeriodicDamage,  // a damage over time tick
        Melee,           // a melee swing (spellId 0; avoided: miss, dodge, parry, evade, immune, full block)
        DamageShield,    // thorns-like damage back at the attacker
        SpellMiss,       // a spell that missed, was resisted, dodged, reflected ... (missInfo)
        Heal,            // a direct heal (amount with overheal, overheal, absorbed by a heal absorb, crit)
        PeriodicHeal,    // a heal over time tick
        Absorb,          // a shield on target absorbed amount of source's damage; caster: the shield's caster
        Interrupt,       // source interrupted target's extraSpellId with spellId
        Dispel,          // source dispelled or stole amount auras from target with spellId
        Environmental,   // target took amount of environmental damage (envType: EnviromentalDamage)
    };

    struct Event
    {
        Kind kind = Kind::SpellDamage;
        Unit* source = nullptr;      // the attacker, healer, interrupter (may be null: a periodic aura's caster left)
        Unit* target = nullptr;      // the victim, the healed (never null)
        Unit* caster = nullptr;      // Absorb: the shield's caster (may be null)
        uint32 spellId = 0;          // the spell (0: melee); Absorb: the shield's spell
        uint32 extraSpellId = 0;     // Interrupt: the interrupted spell; Absorb: the attacking spell
        uint32 amount = 0;           // damage to health (overkill included), heal (overheal included), absorbed, count
        uint32 overkill = 0;         // overkill; heals: overheal
        uint32 absorbed = 0;
        uint32 resisted = 0;
        uint32 blocked = 0;
        uint32 targetHealthBefore = 0; // the target's health when the event was logged (before the damage; heals: after)
        uint8 missInfo = 0;          // SpellMiss: SpellMissInfo; Melee: 1 when the swing did nothing (avoided)
        uint8 envType = 0;
        bool crit = false;
        bool healthAfter = false;    // targetHealthBefore is the health after the event (heals)
    };

    using Sink = void (*)(Event const&);

    inline std::atomic<Sink>& SinkSlot()
    {
        static std::atomic<Sink> slot{ nullptr };
        return slot;
    }

    inline void SetSink(Sink sink) { SinkSlot().store(sink, std::memory_order_release); }
    inline bool Active() { return SinkSlot().load(std::memory_order_relaxed) != nullptr; }

    inline void Emit(Event const& e)
    {
        if (Sink sink = SinkSlot().load(std::memory_order_acquire))
            sink(e);
    }
}

#endif
