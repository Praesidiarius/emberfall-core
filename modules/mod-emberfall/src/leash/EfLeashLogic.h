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

// Emberfall: the open world's leash (Emberfall docs/design/leash-and-social-aggro.md). Pure: which landed hit keeps a
// creature on its victim. No core types, so tests/leash_tests.cpp runs it without a server.
//
// The core's leash (Creature::CanCreatureAttack): an engaged creature away from its home keeps its victim while its
// leash timer runs (11 s at level 1-29 up to 15 s at 50-60, Creature::GetLeashTimer); once it ran out, only a victim
// within CreatureLeashRadius of its home (2D) holds it, otherwise its threat goes offline and it evades home. Upstream
// renews the timer when the creature is hit (Unit::DealDamage, not by damage over time or a damage shield), attacked
// or engaged. Classic renews it too when the creature lands a melee hit on its victim (leash-and-social-aggro.md §2):
// a mob that keeps hitting you never leashes. The module adds that half here.

#ifndef EF_LEASH_LOGIC_H
#define EF_LEASH_LOGIC_H

namespace Emberfall::Leash
{
    // The core's DamageEffectType, by meaning (DIRECT_DAMAGE 0, SPELL_DIRECT_DAMAGE 1, DOT 2, HEAL 3, NODAMAGE 4,
    // SELF_DAMAGE 5).
    enum class HitKind { Melee, SpellDirect, OverTime, Other };

    struct Hit
    {
        bool attackerIsCreature = false;      // a Creature (not a player)
        bool attackerPlayerControlled = false; // a pet, guardian, charm or companion of a player: no leash of its own
        bool attackerEngaged = false;          // in combat with a threat list (an evading creature never extends)
        bool attackerEvading = false;
        bool selfHit = false;                  // attacker == victim (a creature's own self damage)
        HitKind kind = HitKind::Other;
    };

    // Does this landed hit renew the attacker's leash timer? Melee and direct spell hits (a caster mob's bolts count:
    // it is fighting you), not damage over time (a DoT ticking on a player who ran off must not hold the mob), and
    // never for a player's own creature or a creature already on its way home.
    inline bool HitRenewsAttackerLeash(Hit const& h)
    {
        if (!h.attackerIsCreature || h.attackerPlayerControlled || !h.attackerEngaged || h.attackerEvading || h.selfHit)
            return false;
        return h.kind == HitKind::Melee || h.kind == HitKind::SpellDirect;
    }

    // A creature taking a periodic tick (Emberfall feedback #185). Upstream's Unit::DealDamage renews the victim's leash
    // for a direct hit it takes but not for a DoT tick (nor a damage shield): a mob a player keeps dotting from range
    // evaded once its timer ran out. Here a tick from a player or a player's pet renews it like a direct hit would.
    struct Tick
    {
        bool victimIsCreature = false;
        bool victimPlayerOwned = false;        // a player's pet, guardian or companion: upstream renews none of these
        bool victimEngaged = false;            // in combat with a threat list
        bool victimEvading = false;            // on its way home: a tick does not call it back
        bool attackerPlayerControlled = false; // a player, or a unit a player controls or owns
        bool selfHit = false;
        HitKind kind = HitKind::Other;
        unsigned damage = 0;                   // a fully absorbed tick renews nothing (as upstream's hits)
    };

    // Does this periodic tick renew the victim's leash timer?
    inline bool TickRenewsVictimLeash(Tick const& t)
    {
        if (t.kind != HitKind::OverTime || !t.victimIsCreature || t.victimPlayerOwned || !t.victimEngaged ||
            t.victimEvading || !t.attackerPlayerControlled || t.selfHit)
            return false;
        return t.damage > 0;
    }

    // The leash timer of a creature of minimum level `minLevel`, as upstream's Creature::GetLeashTimer (seconds); here
    // for the tests and the design doc's table.
    inline unsigned LeashSeconds(unsigned minLevel)
    {
        int const modifier = int(minLevel / 10) - 2;
        int const t = 11 + modifier;
        return unsigned(t < 11 ? 11 : t);
    }
}

#endif
