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

// Standalone tests of the stat rules (src/stats/EfStatRulesLogic.h and the core's EmberfallStatRules.h, PATCHES.md #40;
// tests/run_tests.sh builds and runs them): spell power and attack power one for one, the mana pool in place of create
// mana and Intellect's bonus, the flat regen with its auras, the table rows, Spirit dropped from items.

#include "EfStatRulesLogic.h"
#include "EmberfallStatRules.h"

#include <cmath>
#include <iostream>
#include <string>

using namespace Emberfall::StatRules;

namespace
{
    int g_failed = 0, g_checks = 0;

    void Check(bool ok, std::string const& what)
    {
        ++g_checks;
        if (!ok)
        {
            ++g_failed;
            std::cerr << "FAIL: " << what << '\n';
        }
    }

    bool Near(float a, float b, float tol = 0.001f) { return std::fabs(a - b) <= tol; }

    // upstream's max mana (StatSystem.cpp Player::UpdateMaxPower) with Intellect's bonus (GetManaBonusFromIntellect)
    float Upstream(float flatBase, float create, float basePct, float flatTotal, float intellect, float totalPct)
    {
        float const baseInt = intellect < 20.0f ? intellect : 20.0f;
        float const bonus = baseInt + (intellect - baseInt) * 15.0f;
        return ((flatBase + create) * basePct + flatTotal + bonus) * totalPct;
    }

    float IntBonus(float intellect)
    {
        float const baseInt = intellect < 20.0f ? intellect : 20.0f;
        return baseInt + (intellect - baseInt) * 15.0f;
    }
}

int main()
{
    // ---- rule 1: spell power from Intellect
    Check(SpellPowerFromIntellect(0.0f) == 0, "no Intellect, no spell power");
    Check(SpellPowerFromIntellect(-5.0f) == 0, "negative Intellect gives none");
    Check(SpellPowerFromIntellect(34.8f) == 34, "one for one, whole points");
    Check(SpellPowerFromIntellect(275.0f) == 275, "one for one at 60");

    // ---- rule 2: attack power from Strength (a hunter's Agility too)
    for (uint8_t cls : {1, 2, 8, 9, 11})
    {
        Check(Near(AttackPowerBase(cls, 95.0f, 67.0f), 95.0f), "Strength one for one, Agility nothing, class " + std::to_string(cls));
        Check(Near(AttackPowerBase(cls, 0.0f, 50.0f), 0.0f), "no Strength, no attack power, class " + std::to_string(cls));
    }
    Check(Near(AttackPowerBase(3, 81.0f, 67.0f), 148.0f), "a hunter: Strength and Agility one for one");
    Check(Near(AttackPowerBase(3, -4.0f, 10.0f), 10.0f), "a debuffed Strength below 0 counts as 0");
    // at level 1 the rule gives a warrior 24 (upstream 3 + 48 - 20 = 31) and a mage 18 (upstream 8): above 0 for all
    Check(AttackPowerBase(1, 24.0f, 20.0f) > 0.0f && AttackPowerBase(8, 18.0f, 20.0f) > 0.0f, "level 1 attack power above 0");

    // ---- rule 3: the pool in place of create mana and Intellect's bonus
    {
        // a mage at 10: create 195, Intellect 45, no auras: upstream 195 + 20 + 25 x 15 = 590; the pool 975
        float const up = Upstream(0, 195, 1.0f, 0, 45, 1.0f);
        Check(Near(up, 590.0f), "upstream's mage pool at 10");
        Check(Near(PoolInPlaceOfIntellect(up, 195, 975, IntBonus(45), 1.0f, 1.0f), 975.0f), "the pool alone, no auras");
        // Intellect no longer moves it
        for (float intellect : {10.0f, 45.0f, 300.0f})
        {
            float const v = Upstream(0, 195, 1.0f, 0, intellect, 1.0f);
            Check(Near(PoolInPlaceOfIntellect(v, 195, 975, IntBonus(intellect), 1.0f, 1.0f), 975.0f),
                  "Intellect " + std::to_string(int(intellect)) + " leaves the pool");
        }
        // a talent's +10 % max mana (TOTAL_PCT 1.1) and +50 flat stay on top of the pool
        float const withAuras = Upstream(20, 195, 1.0f, 50, 45, 1.1f);
        Check(Near(PoolInPlaceOfIntellect(withAuras, 195, 975, IntBonus(45), 1.0f, 1.1f), ((20 + 975) + 50) * 1.1f, 0.01f),
              "auras on top of the pool");
        // a BASE_PCT aura scales the pool, as it scaled create mana
        float const basePct = Upstream(0, 195, 1.2f, 0, 45, 1.0f);
        Check(Near(PoolInPlaceOfIntellect(basePct, 195, 975, IntBonus(45), 1.2f, 1.0f), 975 * 1.2f, 0.01f), "BASE_PCT on the pool");
        // the class kits' hook adds its own delta on the same value: the order does not matter
        float const kitDelta = 150.0f;
        Check(Near(PoolInPlaceOfIntellect(up + kitDelta, 195, 975, IntBonus(45), 1.0f, 1.0f),
                   PoolInPlaceOfIntellect(up, 195, 975, IntBonus(45), 1.0f, 1.0f) + kitDelta), "additive with another hook");
        Check(PoolInPlaceOfIntellect(10.0f, 500.0f, 0.0f, 300.0f, 1.0f, 1.0f) == 0.0f, "never below 0");
    }

    // ---- rule 6: the flat regen
    Check(Near(ManaRegenPerSecond(9.75f, 1.0f, 0.0f), 9.75f), "the row's rate");
    Check(Near(ManaRegenPerSecond(10.0f, 1.2f, 0.0f), 12.0f), "a +20 % regen talent scales the rate");
    Check(Near(ManaRegenPerSecond(10.0f, 1.0f, 2.0f), 12.0f), "mana a five seconds adds");
    Check(Near(ManaRegenPerSecond(10.0f, 0.0f, 2.0f), 2.0f), "a -100 % aura stops the rate, not the flat auras");
    Check(Near(ManaRegenPerSecond(0.0f, 1.0f, -5.0f), 0.0f), "never below 0");
    Check(!On(), "off until a module switches it on");

    // ---- the table rows
    {
        ManaRow ok{8, 10, 975.0f, 9.75f};
        Check(ValidRow(ok), "a full row");
        Check(!ValidRow(ManaRow{0, 10, 975.0f, 1.0f}), "no class");
        Check(!ValidRow(ManaRow{8, 0, 975.0f, 1.0f}), "no level");
        Check(!ValidRow(ManaRow{8, 10, 0.0f, 1.0f}), "no pool");
        Check(!ValidRow(ManaRow{8, 10, 975.0f, -1.0f}), "a negative regen");
        Check(ValidRow(ManaRow{8, 10, 975.0f, 0.0f}), "a regen of 0 is allowed");
        Check(RowKey(8, 10) != RowKey(10, 8), "keys tell class and level apart");
        Check(RowKey(11, 60) == RowKey(11, 60), "keys are stable");
    }

    // ---- rule 5: Spirit dropped from items
    Check(DroppedItemStat(6), "Spirit (6) is dropped");
    for (uint32_t stat : {3u, 4u, 5u, 7u, 45u})
        Check(!DroppedItemStat(stat), "stat " + std::to_string(stat) + " stays");

    std::cout << "statrules_tests: " << (g_checks - g_failed) << "/" << g_checks << " checks passed\n";
    return g_failed ? 1 : 0;
}
