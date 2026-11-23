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

// Standalone tests of the Executioner v4.0's queued-swing rules (src/classkit/EfExecLogic.h; tests/run_tests.sh builds and
// runs them; Emberfall docs/design/classes/warrior-executioner.md §21): the swing's progress from the server's attack
// timer, the sweet spot's window, the second press, which casts cancel a queued swing. The window in the vectors is the
// hand-off's (60-80 %); the module reads it from the talents' own spells.

#include "EfExecLogic.h"

#include <cmath>
#include <iostream>
#include <string>

using namespace Emberfall::ClassKit::Exec;

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

    bool near(float a, float b) { return std::fabs(a - b) < 0.01f; }
}

int main()
{
    // the swing's progress: the timer counts down from the swing's time to 0
    check(near(SwingProgressPct(3300, 3300.0f), 0.0f), "a swing that just began is at 0 %");
    check(near(SwingProgressPct(1320, 3300.0f), 60.0f), "1.32 s of 3.3 s left: 60 %");
    check(near(SwingProgressPct(660, 3300.0f), 80.0f), "0.66 s of 3.3 s left: 80 %");
    check(near(SwingProgressPct(0, 3300.0f), 100.0f), "a swing that ran out (waits for the attack): 100 %");
    check(near(SwingProgressPct(-50, 3300.0f), 100.0f), "a timer below 0: 100 %");
    check(near(SwingProgressPct(4000, 3300.0f), 0.0f), "a timer above the swing's time (a haste change's rounding): 0 %");
    check(near(SwingProgressPct(500, 0.0f), 100.0f), "no swing time: 100 %");
    // haste: the same share of the swing at a faster speed (the core rescales the rest) is the same progress
    check(near(SwingProgressPct(1100, 2750.0f), 60.0f), "20 % haste: 1.1 s of 2.75 s left is still 60 %");

    // the sweet spot: 60-80 %, both ends in
    check(!SweetSpot(1400, 3300.0f, 60, 80), "57.6 %: before the window");
    check(SweetSpot(1320, 3300.0f, 60, 80), "60 %: the window's start is in");
    check(SweetSpot(990, 3300.0f, 60, 80), "70 %: in");
    check(SweetSpot(660, 3300.0f, 60, 80), "80 %: the window's end is in");
    check(!SweetSpot(600, 3300.0f, 60, 80), "81.8 %: after the window");
    check(!SweetSpot(0, 3300.0f, 60, 80), "no running swing (100 %): never");
    check(!SweetSpot(3300, 3300.0f, 60, 80), "a swing that just began: never");
    check(!InWindow(70.0f, 80, 60), "an empty window never holds");
    check(!InWindow(70.0f, 70, 70), "a window of no width never holds");
    // the same share of the swing at another speed: the window follows the weapon and haste
    check(SweetSpot(1050, 3500.0f, 60, 80) && SweetSpot(780, 2600.0f, 60, 80), "70 % of a 3.5 s and of a 2.6 s swing");

    // a second press of the queued swing does nothing; another swing replaces it; the swing's own cast passes
    uint32_t const brutalSwing = 91020, finalVerdict = 91021;
    check(RefusesPress(brutalSwing, brutalSwing, false), "Brutal Swing pressed again while queued: refused");
    check(!RefusesPress(brutalSwing, finalVerdict, false), "Final Verdict while Brutal Swing waits: replaces it");
    check(!RefusesPress(brutalSwing, brutalSwing, true), "the queued Brutal Swing's own cast at the swing: not refused");
    check(!RefusesPress(0, brutalSwing, false), "nothing queued: the press queues it");

    // what cancels a queued swing
    CastFacts other;
    other.queued = true;
    other.kitFamily = true;
    check(CancelsQueue(other), "another Warrior ability (Deathblow, Reaping Arc): cancels it");
    CastFacts keep = other;
    keep.exempt = true;
    check(!CancelsQueue(keep), "Killing Instinct (named by the rule): keeps it");
    CastFacts trig = other;
    trig.triggered = true;
    check(!CancelsQueue(trig), "a triggered spell (a proc, a script's helper, the bleed): keeps it");
    CastFacts swing = other;
    swing.nextSwing = true;
    check(!CancelsQueue(swing), "another queued swing (the engine replaces it) or the swing's own cast: no cancel here");
    CastFacts item = other;
    item.kitFamily = false;
    check(!CancelsQueue(item), "a spell of no kit (a potion, a mount, the hearthstone): keeps it");
    CastFacts passive = other;
    passive.passive = true;
    check(!CancelsQueue(passive), "a passive learned or applied: keeps it");
    CastFacts none = other;
    none.queued = false;
    check(!CancelsQueue(none), "nothing queued: nothing to cancel");

    std::cout << "classkit exec tests: " << (g_checks - g_failed) << "/" << g_checks << " passed\n";
    return g_failed ? 1 : 0;
}
