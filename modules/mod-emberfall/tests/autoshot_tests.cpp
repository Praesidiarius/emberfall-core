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

// Standalone tests of the core's auto shot rule (src/server/game/Spells/EmberfallAutoShot.h, PATCHES.md #42; Emberfall
// alpha1 feedback #650; tests/run_tests.sh builds and runs them): on own data the Hunter's auto shot is not refused to a
// moving player, at its start or at any shot (Unit::_UpdateAutoRepeatSpell asks the same check); every other auto-repeat
// spell and every spell a seat interrupts are refused as upstream; stock data is upstream's. A planted cast (a cast time
// that movement interrupts) is not this rule's: Spell::prepare and Spell::update stop it unless #25's cast-while-moving
// check lets it, and without a check (or with one that names other spells) nothing lets it.

#include "EmberfallAutoShot.h"
#include "EmberfallCastWhileMoving.h"

#include <cstdint>
#include <iostream>
#include <string>

using namespace Emberfall;

namespace
{
    int g_failed = 0, g_checks = 0;

    void check(bool ok, std::string const& what)
    {
        ++g_checks;
        if (!ok)
        {
            ++g_failed;
            std::cerr << "FAILED: " << what << "\n";
        }
    }

    constexpr uint32_t kWandShoot = 5019; // stock's wand Shoot: another auto-repeat spell
    constexpr uint32_t kSitSpell = 433;   // a spell a seat interrupts (stock's Food)

    void testMoving()
    {
        check(!AutoShot::MovingRefuses(true, AutoShot::kSpellId, true, false),
            "own data: Loose shoots while the hunter walks, runs, strafes or jumps (#650)");
        check(AutoShot::MovingRefuses(true, kWandShoot, true, false), "own data: another auto-repeat spell (a wand) still waits");
        check(AutoShot::MovingRefuses(true, kSitSpell, false, true), "own data: a spell a seat interrupts is still refused");
        check(AutoShot::MovingRefuses(true, AutoShot::kSpellId, true, true), "an auto shot a seat would interrupt: refused");
        check(!AutoShot::MovingRefuses(true, 99999, false, false), "an instant shot is not refused by this check");
        // stock data: upstream
        check(AutoShot::MovingRefuses(false, AutoShot::kSpellId, true, false), "stock data: upstream's auto shot waits while moving");
        check(AutoShot::MovingRefuses(false, kWandShoot, true, false), "stock data: a wand waits while moving");
        check(!AutoShot::MovingRefuses(false, 99999, false, false), "stock data: an instant spell is not refused here");
    }

    bool OnlyPatientShot(Unit const*, SpellInfo const* spell) { return spell == reinterpret_cast<SpellInfo const*>(0x1); }

    void testPlanted()
    {
        SpellInfo const* fullDraw = reinterpret_cast<SpellInfo const*>(0x2);
        // Without a check (stock, or no class kit loaded) nothing with a cast time is cast on the move: it plants.
        CastWhileMoving::SetCheck(nullptr);
        check(!CastWhileMoving::Allowed(nullptr, fullDraw), "no check: Full Draw plants (stopped by moving)");
        // A check that lets other spells move: Full Draw still plants; the auto shot rule lifts nothing of it.
        CastWhileMoving::SetCheck(&OnlyPatientShot);
        check(!CastWhileMoving::Allowed(nullptr, fullDraw), "a check for other spells: Full Draw still plants");
        check(CastWhileMoving::Allowed(nullptr, reinterpret_cast<SpellInfo const*>(0x1)), "the check's own spell moves");
        CastWhileMoving::SetCheck(nullptr);
    }
}

int main()
{
    testMoving();
    testPlanted();
    std::cout << "auto shot tests: " << (g_checks - g_failed) << "/" << g_checks << " passed\n";
    return g_failed == 0 ? 0 : 1;
}
