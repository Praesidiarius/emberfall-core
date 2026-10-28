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

// Standalone tests of the profession XP arithmetic (src/professions/EfProfessionXp.h; tests/run_tests.sh builds and
// runs them): the colours at the 40-point bands' edges, the XP per colour and requirement.

#include "EfProfessionXp.h"

#include <iostream>
#include <string>

using namespace Emberfall::Professions;

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

    // a gathering node of requirement r under own data: yellow r+40, green r+80, grey r+120
    Colour Node(uint32_t skill, uint32_t r) { return ColourOf(skill, r, r + 40, r + 80, r + 120); }
}

int main()
{
    check(Node(104, 105) == Colour::Locked, "below the requirement: locked");
    check(Node(105, 105) == Colour::Orange, "at the requirement: orange");
    check(Node(144, 105) == Colour::Orange, "+39: orange");
    check(Node(145, 105) == Colour::Yellow, "+40: yellow");
    check(Node(184, 105) == Colour::Yellow, "+79: yellow");
    check(Node(185, 105) == Colour::Green, "+80: green");
    check(Node(224, 105) == Colour::Green, "+119: green");
    check(Node(225, 105) == Colour::Grey, "+120: grey");
    check(Node(500, 1) == Colour::Grey, "Master on copper: grey");
    check(ColourOf(50, 1, 0, 0, 0) == Colour::Grey, "no thresholds: grey");
    // a recipe of requirement 76, trivial low 116, high 196 (green from 156)
    check(ColourOf(120, 76, 116, 156, 196) == Colour::Yellow, "recipe +44: yellow");
    check(ColourOf(160, 76, 116, 156, 196) == Colour::Green, "recipe +84: green");

    // the formula itself at rate 1 (the numbers before 2026-10-28)
    XpRules r;
    r.rate = 1.0;
    check(XpFor(r, Colour::Orange, 1) == 70, "rate 1, Apprentice orange: 70");
    check(XpFor(r, Colour::Yellow, 105) == 76, "rate 1, iron yellow: 76");
    check(XpFor(r, Colour::Orange, 175) == 81, "rate 1, requirement 175: 81");
    check(XpFor(r, Colour::Orange, 325) == 90, "rate 1, requirement 325: 90");
    check(XpFor(r, Colour::Orange, 500) == 100, "rate 1, requirement 500: 100");
    check(XpFor(r, Colour::Green, 105) == 38, "rate 1, iron green: half of 76");
    check(XpFor(r, Colour::Grey, 105) == 0, "grey: nothing");
    check(XpFor(r, Colour::Locked, 105) == 0, "locked: nothing");

    // the default rate 0.1 (2026-10-28: profession XP is cosmetic): a tenth of the above
    XpRules d;
    check(d.rate == 0.1, "default rate 0.1");
    check(XpFor(d, Colour::Orange, 1) == 7, "default, Apprentice orange: 7");
    check(XpFor(d, Colour::Orange, 70) == 7, "default, tin orange: 7 (was 74)");
    check(XpFor(d, Colour::Yellow, 105) == 8, "default, iron yellow: 8");
    check(XpFor(d, Colour::Orange, 175) == 8, "default, requirement 175: 8");
    check(XpFor(d, Colour::Orange, 325) == 9, "default, requirement 325: 9");
    check(XpFor(d, Colour::Orange, 500) == 10, "default, requirement 500: 10");
    check(XpFor(d, Colour::Green, 1) == 4, "default, copper green: 4");
    check(XpFor(d, Colour::Green, 105) == 4, "default, iron green: 4");
    check(XpFor(d, Colour::Green, 500) == 5, "default, requirement 500 green: 5");
    check(XpFor(d, Colour::Grey, 500) == 0, "default, grey: nothing");
    XpRules zero;
    zero.rate = 0;
    check(XpFor(zero, Colour::Orange, 500) == 0, "rate 0: nothing");
    zero.rate = -1;
    check(XpFor(zero, Colour::Orange, 500) == 0, "negative rate: nothing");
    XpRules off;
    off.base = 0;
    off.perSkill = 0;
    check(XpFor(off, Colour::Orange, 300) == 0, "base 0: nothing");

    std::cout << "profession xp tests: " << (g_checks - g_failed) << "/" << g_checks << " passed\n";
    return g_failed ? 1 : 0;
}
