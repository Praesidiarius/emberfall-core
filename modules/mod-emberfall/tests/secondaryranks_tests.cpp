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

// Standalone tests of the secondary professions' move onto the primaries' steps (src/professions/EfSecondaryRanks.h;
// tests/run_tests.sh builds and runs them): the rule (Emberfall's secondary_ranks.remap, the same examples as its
// test_secondary_ranks.py), the login migration on a table of characters, the talent budget never below the old one
// (src/profspecs/EfProfModel.h Budget, the module's own), the login line in three languages.

#include "EfProfModel.h"
#include "EfSecondaryRanks.h"

#include <iostream>
#include <string>

using namespace Emberfall::Professions;
namespace SR = Emberfall::Professions::SecondaryRanks;

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

    bool Is(std::optional<SR::Moved> const& m, uint32_t value, uint32_t cap, uint32_t step)
    {
        return m && m->value == value && m->cap == cap && m->step == step;
    }
}

int main()
{
    // the rule: the ends and the rank caps onto each other, Apprentice unchanged, strictly rising, the bands kept
    {
        check(SR::Remap(0) == 0, "0 stays");
        for (size_t i = 0; i < SR::kOldCaps.size(); ++i)
            check(SR::Remap(SR::kOldCaps[i]) == SR::kNewCaps[i], "cap " + std::to_string(SR::kOldCaps[i]) + " lands on its new cap");
        bool same = true, rising = true, bands = true;
        for (uint32_t v = 0; v <= 75; ++v)
            same = same && SR::Remap(v) == v;
        for (uint32_t v = 1; v <= 450; ++v)
            rising = rising && SR::Remap(v) > SR::Remap(v - 1);
        for (uint32_t v = 1; v <= 300; ++v)
        {
            size_t o = 0, n = 0;
            while (v > SR::kOldCaps[o])
                ++o;
            while (SR::Remap(v) > SR::kNewCaps[n])
                ++n;
            bands = bands && o == n;
        }
        check(same, "0-75 unchanged");
        check(rising, "strictly rising");
        check(bands, "every value stays in its rank's band");
        check(SR::Remap(141) == 163 && SR::Remap(151) == 177 && SR::Remap(121) == 136, "the examples (as test_secondary_ranks.py)");
        check(SR::Remap(330) == 570 && SR::Remap(410) == 756, "past the old cap: the last band's slope");
    }
    // the login migration: the save's value and cap, and the cap the load gave
    {
        check(!SR::Migrate(0, 0), "not learned: nothing");
        check(!SR::Migrate(74, 75), "74/75: nothing (Apprentice)");
        check(!SR::Migrate(75, 75, 75), "75/75: nothing");
        check(Is(SR::Migrate(141, 150, 175), 163, 175, 2), "141/150 -> 163/175 (the load gave 175 already)");
        check(Is(SR::Migrate(141, 150), 163, 175, 2), "141/150 -> 163/175 (a save without the rank spell)");
        check(Is(SR::Migrate(150, 150, 175), 175, 175, 2), "150/150 -> 175/175");
        check(Is(SR::Migrate(60, 150, 175), 60, 175, 2), "60/150 -> 60/175");
        check(Is(SR::Migrate(151, 225, 325), 177, 325, 3), "151/225 -> 177/325");
        check(Is(SR::Migrate(300, 300, 500), 500, 500, 4), "300/300 -> 500/500");
        check(Is(SR::Migrate(141, 150, 325), 163, 325, 3), "a higher rank's spell known: that rank's cap, never lower");
        check(!SR::Migrate(163, 175, 175), "migrated already (or learned on the new steps): never twice");
        check(!SR::Migrate(500, 500, 500), "500/500: nothing");
        check(!SR::Migrate(290, 325, 325), "on the new steps: nothing");
        bool fits = true;
        for (uint32_t cap : SR::kOldCaps)
            for (uint32_t v = 1; v <= cap; ++v)
                if (auto const m = SR::Migrate(v, cap))
                    fits = fits && m->value <= m->cap && m->value >= v && SR::RankOfCap(SR::kNewCaps, m->cap) == SR::RankOfCap(SR::kOldCaps, cap);
        check(fits, "never above the cap, never lower, never another rank");
    }
    // the talents: the primaries' budget (from 175, one per 10) at the moved skill is never below the old one (from 150,
    // one per 5), so a spend from before still fits; 32 points at 500
    {
        ef::prof::Model now, before;
        now.pointsPerSkill = 10;
        now.pointsFrom[185] = 175;
        before.pointsFrom[185] = 150;
        before.stepOf[185] = 5;
        bool never = true;
        for (uint32_t v = 0; v <= 300; ++v)
            never = never && now.Budget(185, SR::Remap(v)) >= before.Budget(185, v);
        check(never, "the budget never shrinks");
        check(now.Budget(185, 500) == 32 && before.Budget(185, 300) == 30, "32 at 500 (was 30 at 300)");
        check(now.Budget(185, SR::Remap(200)) == before.Budget(185, 200), "Craftsman's band: the same points");
    }
    // the login line
    {
        check(SR::LoginLine(Lang::En, SR::SkillName(185, Lang::En), 141, 150, 163, 175) ==
              "Cooking now goes up to 500, like the main professions. Your skill 141/150 is now 163/175; your rank and recipes stay.",
              "enUS line");
        check(SR::LoginLine(Lang::De, SR::SkillName(129, Lang::De), 300, 300, 500, 500).find("Erste Hilfe steigt jetzt") == 0, "deDE line");
        check(SR::LoginLine(Lang::Fr, SR::SkillName(900, Lang::Fr), 151, 225, 177, 325).find("177/325") != std::string::npos, "frFR line");
    }

    std::cout << "secondary ranks tests: " << (g_checks - g_failed) << "/" << g_checks << " passed\n";
    return g_failed ? 1 : 0;
}
