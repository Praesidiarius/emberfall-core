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

// Standalone tests of the leash rule (src/leash/EfLeashLogic.h; tests/run_tests.sh builds and runs them): which landed
// hit of a creature renews its own leash timer, which DoT tick renews its victim's (#185), and the timer per level.

#include "EfLeashLogic.h"

#include <iostream>
#include <string>

using namespace Emberfall::Leash;

namespace
{
    int g_failed = 0, g_checks = 0;

    void check(bool ok, std::string const& what)
    {
        ++g_checks;
        if (!ok)
        {
            ++g_failed;
            std::cerr << "FAIL: " << what << "\n";
        }
    }

    Hit MobHit(HitKind kind)
    {
        Hit h;
        h.attackerIsCreature = true;
        h.attackerEngaged = true;
        h.kind = kind;
        return h;
    }
}

int main()
{
    check(HitRenewsAttackerLeash(MobHit(HitKind::Melee)), "an engaged mob's melee hit renews");
    check(HitRenewsAttackerLeash(MobHit(HitKind::SpellDirect)), "an engaged mob's bolt renews");
    check(!HitRenewsAttackerLeash(MobHit(HitKind::OverTime)), "its DoT ticking on a runner does not");
    check(!HitRenewsAttackerLeash(MobHit(HitKind::Other)), "self damage, no-damage effects do not");

    Hit pet = MobHit(HitKind::Melee);
    pet.attackerPlayerControlled = true;
    check(!HitRenewsAttackerLeash(pet), "a player's pet or companion has no leash to renew");

    Hit idle = MobHit(HitKind::Melee);
    idle.attackerEngaged = false;
    check(!HitRenewsAttackerLeash(idle), "a creature not engaged (sparring, scripted) does not");

    Hit home = MobHit(HitKind::Melee);
    home.attackerEvading = true;
    check(!HitRenewsAttackerLeash(home), "an evading creature stays on its way home");

    Hit self = MobHit(HitKind::SpellDirect);
    self.selfHit = true;
    check(!HitRenewsAttackerLeash(self), "a creature hitting itself does not");

    Hit player = MobHit(HitKind::Melee);
    player.attackerIsCreature = false;
    check(!HitRenewsAttackerLeash(player), "a player's hit renews nothing here (the core renews the victim's)");

    // feedback #185: a player's DoT tick on an engaged mob renews the mob's leash, as a direct hit it takes does
    Tick tick;
    tick.victimIsCreature = true;
    tick.victimEngaged = true;
    tick.attackerPlayerControlled = true;
    tick.kind = HitKind::OverTime;
    tick.damage = 42;
    check(TickRenewsVictimLeash(tick), "a player's DoT tick on an engaged mob renews its leash");
    Tick t = tick;
    t.kind = HitKind::Melee;
    check(!TickRenewsVictimLeash(t), "direct hits stay the core's (it renews the victim itself)");
    t = tick;
    t.damage = 0;
    check(!TickRenewsVictimLeash(t), "a fully absorbed tick does not");
    t = tick;
    t.victimEvading = true;
    check(!TickRenewsVictimLeash(t), "a tick does not call an evading mob back");
    t = tick;
    t.victimEngaged = false;
    check(!TickRenewsVictimLeash(t), "a mob not engaged has no leash to renew");
    t = tick;
    t.victimPlayerOwned = true;
    check(!TickRenewsVictimLeash(t), "a player's pet taking a tick does not");
    t = tick;
    t.attackerPlayerControlled = false;
    check(!TickRenewsVictimLeash(t), "a mob's DoT on another mob does not");
    t = tick;
    t.selfHit = true;
    check(!TickRenewsVictimLeash(t), "self damage does not");
    t = tick;
    t.victimIsCreature = false;
    check(!TickRenewsVictimLeash(t), "a player taking a tick has no leash");

    // upstream's Creature::GetLeashTimer: 11 s to level 29, then a second per ten levels; Classic: 11/12/13/14/15
    check(LeashSeconds(1) == 11, "level 1: 11 s");
    check(LeashSeconds(19) == 11, "level 19: 11 s");
    check(LeashSeconds(29) == 11, "level 29: 11 s");
    check(LeashSeconds(30) == 12, "level 30: 12 s");
    check(LeashSeconds(45) == 13, "level 45: 13 s");
    check(LeashSeconds(60) == 15, "level 60: 15 s");

    std::cout << "leash tests: " << (g_checks - g_failed) << "/" << g_checks << " passed\n";
    return g_failed ? 1 : 0;
}
