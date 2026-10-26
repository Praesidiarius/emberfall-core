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

// Standalone tests of the periodic crit rule (the core's src/server/game/Spells/Auras/EmberfallPeriodicCrit.h,
// PATCHES.md #23; tests/run_tests.sh builds and runs them): which chance a damage, leech or heal tick rolls by the
// spell's damage class, the ways out (damage class none, CANT_CRIT), creatures' own spells, the other aura types; and the
// class kits' periodic spells as built (Emberfall content/data/classes, 2026-10-26).

#include "EmberfallPeriodicCrit.h"

#include <cstdint>
#include <iostream>
#include <string>

using namespace Emberfall::PeriodicCrit;

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

    // other AuraType values the rule must leave alone
    constexpr uint32_t AURA_DUMMY = 4;
    constexpr uint32_t AURA_PERIODIC_TRIGGER_SPELL = 23;
    constexpr uint32_t AURA_PERIODIC_ENERGIZE = 24;
    constexpr uint32_t AURA_PERIODIC_DUMMY = 226;
    constexpr uint32_t AURA_PERIODIC_DAMAGE_PERCENT = 89;
    constexpr uint32_t AURA_OBS_MOD_HEALTH = 20;

    void testDamageClasses()
    {
        for (uint32_t aura : {AURA_PERIODIC_DAMAGE, AURA_PERIODIC_LEECH, AURA_PERIODIC_HEAL})
        {
            std::string const a = std::to_string(aura);
            check(Rule(aura, DMG_CLASS_MELEE, false, true) == Source::Melee, "aura " + a + ": melee class rolls the melee crit");
            check(Rule(aura, DMG_CLASS_RANGED, false, true) == Source::Ranged, "aura " + a + ": ranged class rolls the ranged crit");
            check(Rule(aura, DMG_CLASS_MAGIC, false, true) == Source::Spell, "aura " + a + ": magic class rolls the spell crit");
            check(Rule(aura, DMG_CLASS_NONE, false, true) == Source::None, "aura " + a + ": class none never crits");
            check(Rule(aura, 7, false, true) == Source::None, "aura " + a + ": an unknown class never crits");
        }
    }

    void testWaysOut()
    {
        for (uint32_t dc : {DMG_CLASS_NONE, DMG_CLASS_MAGIC, DMG_CLASS_MELEE, DMG_CLASS_RANGED})
        {
            check(Rule(AURA_PERIODIC_DAMAGE, dc, true, true) == Source::None, "CANT_CRIT damage never crits");
            check(Rule(AURA_PERIODIC_HEAL, dc, true, true) == Source::None, "CANT_CRIT heal never crits");
            // a creature's own spell keeps upstream's rule (mobs never crit with spells)
            check(Rule(AURA_PERIODIC_DAMAGE, dc, false, false) == Source::None, "a creature's DoT never crits");
            check(Rule(AURA_PERIODIC_HEAL, dc, false, false) == Source::None, "a creature's HoT never crits");
        }
    }

    void testOtherAuras()
    {
        for (uint32_t aura : {AURA_DUMMY, AURA_PERIODIC_TRIGGER_SPELL, AURA_PERIODIC_ENERGIZE, AURA_PERIODIC_DUMMY,
                              AURA_PERIODIC_DAMAGE_PERCENT, AURA_OBS_MOD_HEALTH})
            for (uint32_t dc : {DMG_CLASS_MAGIC, DMG_CLASS_MELEE, DMG_CLASS_RANGED})
                check(Rule(aura, dc, false, true) == Source::None,
                      "aura " + std::to_string(aura) + " is no damage/leech/heal tick: no crit chance of its own");
    }

    // The class kits' periodic spells (content/data/classes, generated Spell rows: aura, DefenseType, CANT_CRIT).
    struct KitSpell
    {
        uint32_t id;
        char const* name;
        uint32_t aura;
        uint32_t dmgClass;
        bool cantCrit;
        Source expected;
    };

    void testKits()
    {
        KitSpell const kits[] = {
            {96210, "Seed (HoT)", AURA_PERIODIC_HEAL, DMG_CLASS_MAGIC, false, Source::Spell},
            {96211, "Sprout (HoT)", AURA_PERIODIC_HEAL, DMG_CLASS_MAGIC, false, Source::Spell},
            {96205, "Thornbloom", AURA_PERIODIC_DAMAGE, DMG_CLASS_MAGIC, false, Source::Spell},
            {96226, "Venomous Barrage", AURA_PERIODIC_DAMAGE, DMG_CLASS_MAGIC, false, Source::Spell},
            {93139, "Broadhead (bleed, ranged)", AURA_PERIODIC_DAMAGE, DMG_CLASS_RANGED, false, Source::Ranged},
            {94207, "Sunlance", AURA_PERIODIC_DAMAGE, DMG_CLASS_MAGIC, false, Source::Spell},
            {94210, "Burning Sigil", AURA_PERIODIC_DAMAGE, DMG_CLASS_MAGIC, false, Source::Spell},
            {92100, "Hallowed Ground", AURA_PERIODIC_DAMAGE, DMG_CLASS_MAGIC, false, Source::Spell},
            {92322, "Lingering Radiance (a share of a heal)", AURA_PERIODIC_HEAL, DMG_CLASS_NONE, false, Source::None},
            {95071, "Thirst", AURA_PERIODIC_DAMAGE, DMG_CLASS_MAGIC, false, Source::Spell},
            {95201, "Crimson Brine", AURA_PERIODIC_DAMAGE, DMG_CLASS_MAGIC, false, Source::Spell},
            {91032, "Deep Wounds (bleed)", AURA_PERIODIC_DAMAGE, DMG_CLASS_MELEE, false, Source::Melee},
            {91217, "Saltwound (doubled by the strike's crit)", AURA_PERIODIC_DAMAGE, DMG_CLASS_MELEE, true, Source::None},
            {91223, "Razorstorm", AURA_PERIODIC_DAMAGE, DMG_CLASS_MELEE, false, Source::Melee},
            {99470, "First Aid (bandage)", AURA_PERIODIC_HEAL, DMG_CLASS_NONE, false, Source::None},
        };
        for (KitSpell const& k : kits)
            check(Rule(k.aura, k.dmgClass, k.cantCrit, true) == k.expected, std::to_string(k.id) + " " + k.name);
    }
}

int main()
{
    testDamageClasses();
    testWaysOut();
    testOtherAuras();
    testKits();
    std::cout << "periodic crit tests: " << (g_checks - g_failed) << "/" << g_checks << " passed\n";
    return g_failed == 0 ? 0 : 1;
}
