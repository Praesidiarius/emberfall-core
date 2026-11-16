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

// Standalone tests of a new companion's default name (tests/run_tests.sh builds and runs them):
// src/petname/EfPetNameLogic.h. Made-up names: the rule, not the game's creatures.

#include "EfPetNameLogic.h"

#include <iostream>
#include <string>

using namespace Emberfall::PetName;

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
}

int main()
{
    check(DefaultName("fell goat") == "Fell Goat", "each word capitalised");
    check(DefaultName("Silvermantle") == "Silvermantle", "a rare's own name as it is");
    check(DefaultName("the Old Pot") == "The Old Pot", "a leading article too");
    check(DefaultName("scald-newt basker") == "Scald-newt Basker", "a hyphen keeps its word whole");
    check(DefaultName("scald-newt pool-keeper") == "Pool-keeper", "too long: the last words that fit");
    check(DefaultName("aaaaaaaaaaaaaaaaaaaaaaaaaa") == "Aaaaaaaaaaaaaaaaaaaaa", "one word too long: cut to the limit");
    check(Chars(DefaultName("éééééééééééééééééééééééééé")) == kMaxChars, "the limit counts characters, not bytes");
    check(DefaultName("Gämse vom Grat") == "Gämse Vom Grat", "UTF-8 words stay whole");
    check(DefaultName("  odd   spacing ") == "Odd Spacing", "spaces collapse");
    check(DefaultName("").empty(), "no name, none");
    std::cout << "petname: " << g_checks - g_failed << "/" << g_checks << " checks passed\n";
    return g_failed ? 1 : 0;
}
