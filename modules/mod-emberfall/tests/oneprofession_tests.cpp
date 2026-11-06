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

// Standalone tests of the one-main-profession migration (src/professions/EfOneProfession.h; tests/run_tests.sh builds
// and runs them): which craft a character keeps, that nothing goes within the limit, and the mail in three languages.

#include "EfOneProfession.h"

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

    bool has(std::string const& s, std::string const& part) { return s.find(part) != std::string::npos; }
}

int main()
{
    // the higher skill stays, whatever the order
    {
        Cut c = CutTo({ { 164, 120, 175 }, { 171, 210, 325 } }, 1);
        check(c.keep.size() == 1 && c.keep[0].skill == 171, "higher skill kept");
        check(c.drop.size() == 1 && c.drop[0].skill == 164, "lower skill dropped");
        Cut d = CutTo({ { 171, 210, 325 }, { 164, 120, 175 } }, 1);
        check(d.keep[0].skill == 171 && d.drop[0].skill == 164, "order does not matter");
    }
    // ties: the higher rank, then the lower line ID
    {
        Cut c = CutTo({ { 165, 75, 75 }, { 197, 75, 175 } }, 1);
        check(c.keep[0].skill == 197, "tie on skill: higher rank kept");
        Cut d = CutTo({ { 333, 50, 75 }, { 164, 50, 75 } }, 1);
        check(d.keep[0].skill == 164 && d.drop[0].skill == 333, "full tie: lower line ID kept");
    }
    // within the limit nothing goes
    {
        check(CutTo({ { 171, 1, 75 } }, 1).drop.empty(), "one craft, limit 1: kept");
        check(CutTo({}, 1).drop.empty() && CutTo({}, 1).keep.empty(), "no craft: nothing");
        check(CutTo({ { 171, 1, 75 }, { 164, 5, 75 } }, 2).drop.empty(), "two crafts, limit 2: kept");
    }
    // more than two (a GM's character): one stays, the rest go best first
    {
        Cut c = CutTo({ { 171, 10, 75 }, { 164, 300, 325 }, { 165, 150, 175 } }, 1);
        check(c.keep.size() == 1 && c.keep[0].skill == 164, "three crafts: the best kept");
        check(c.drop.size() == 2 && c.drop[0].skill == 165 && c.drop[1].skill == 171, "three crafts: two dropped");
        check(CutTo({ { 171, 10, 75 } }, 0).drop.size() == 1, "limit 0: everything goes");
    }
    // the client's locale
    {
        check(LangOf(0) == Lang::En && LangOf(2) == Lang::Fr && LangOf(3) == Lang::De, "enUS, frFR, deDE");
        check(LangOf(1) == Lang::En && LangOf(8) == Lang::En, "other locales read English");
    }
    // names and the mail
    {
        check(CraftName(164, Lang::De) == "Schmiedekunst" && CraftName(164, Lang::Fr) == "Forge", "craft names");
        check(CraftName(186, Lang::En).empty(), "not a craft: no name");
        check(JoinNames({ "A" }, Lang::En) == "A", "join one");
        check(JoinNames({ "A", "B" }, Lang::De) == "A und B", "join two (de)");
        check(JoinNames({ "A", "B", "C" }, Lang::Fr) == "A, B et C", "join three (fr)");
        for (Lang l : { Lang::En, Lang::De, Lang::Fr })
        {
            Letter m = MailFor(l, CraftName(171, l), CraftName(164, l));
            check(!m.subject.empty() && has(m.body, CraftName(171, l)) && has(m.body, CraftName(164, l)),
                  "the mail names the kept and the lost craft");
        }
        check(has(MailFor(Lang::De, "X", "Y").body, "Hauptberuf"), "the German mail is German");
        check(has(MailFor(Lang::Fr, "X", "Y").body, "métier principal"), "the French mail is French");
    }

    std::cout << "oneprofession_tests: " << g_checks - g_failed << "/" << g_checks << " passed\n";
    return g_failed ? 1 : 0;
}
