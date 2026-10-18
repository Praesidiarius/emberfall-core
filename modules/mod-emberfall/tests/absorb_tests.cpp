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

// Standalone tests of the absorb report logic (src/absorbs/EfAbsorbLogic.h; tests/run_tests.sh builds and runs them):
// the message format, the 100 ms throttle, only changes sent, the highest amount seen as the cap (reset by a new
// aura), a fixed cap (the shell), the `0;0` for an aura gone, several auras of one spell.

#include "EfAbsorbLogic.h"

#include <iostream>
#include <string>

using namespace Emberfall::Absorbs;

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

    // Emberfall's mage (content/data/classes/mage.yaml): Barrier 94044, Frost Shell 94091.
    constexpr std::uint32_t kBarrier = 94044, kFrostShell = 94091;

    Snapshot Snap(std::uint32_t spell, std::int64_t left, std::int64_t applyTime = 100, std::uint64_t instance = 1,
                  std::int64_t fixedCap = -1)
    {
        Snapshot s;
        s.spellId = spell;
        s.left = left;
        s.fixedCap = fixedCap;
        s.applyTime = applyTime;
        s.instance = instance;
        return s;
    }

    bool Is(std::vector<Line> const& lines, std::size_t i, std::uint32_t spell, std::int64_t left, std::int64_t cap)
    {
        return i < lines.size() && lines[i].spellId == spell && lines[i].left == left && lines[i].cap == cap;
    }

    void TestFormat()
    {
        check(Body({ kBarrier, 812, 1000 }) == "1;0;ABS;94044;812;1000", "the body");
        check(Text({ kBarrier, 0, 0 }) == "EFA\t1;0;ABS;94044;0;0", "the text: prefix EFA and a tab");
    }

    void TestThrottle()
    {
        State st;
        check(!st.Tick(40) && !st.Tick(40), "80 ms: not yet");
        check(st.Tick(40), "120 ms: a batch");
        check(!st.Tick(99), "99 ms after: not yet");
        check(st.Tick(1), "100 ms after: a batch");
        check(st.Tick(500), "a long frame: one batch");
        check(!st.Tick(10), "and the clock starts again");
    }

    void TestHighest()
    {
        State st;
        auto l = Diff(st, { Snap(kBarrier, 1000) });
        check(l.size() == 1 && Is(l, 0, kBarrier, 1000, 1000), "Barrier up: 1000 of 1000");
        l = Diff(st, { Snap(kBarrier, 1000) });
        check(l.empty(), "nothing changed: nothing sent");
        l = Diff(st, { Snap(kBarrier, 640) });
        check(l.size() == 1 && Is(l, 0, kBarrier, 640, 1000), "a hit: 640 of 1000 (the highest seen)");
        l = Diff(st, { Snap(kBarrier, 1000) });
        check(l.size() == 1 && Is(l, 0, kBarrier, 1000, 1000), "refreshed in place: 1000 of 1000");
        l = Diff(st, { Snap(kBarrier, 300) });
        check(Is(l, 0, kBarrier, 300, 1000), "300 of 1000");
        l = Diff(st, { Snap(kBarrier, 800, 105, 1) });
        check(l.size() == 1 && Is(l, 0, kBarrier, 800, 800), "applied again (new apply time): the cap starts again");
        l = Diff(st, { Snap(kBarrier, 500, 105, 2) });
        check(l.size() == 1 && Is(l, 0, kBarrier, 500, 500), "a new aura object: the cap starts again");
        l = Diff(st, {});
        check(l.size() == 1 && Is(l, 0, kBarrier, 0, 0), "gone: 0;0");
        l = Diff(st, {});
        check(l.empty() && st.spells.empty(), "gone and told: nothing more");
    }

    void TestShell()
    {
        State st;
        auto l = Diff(st, { Snap(kFrostShell, 120, 100, 1, 3500) });
        check(l.size() == 1 && Is(l, 0, kFrostShell, 120, 3500), "Frost Shell: 120 of its cap 3500 (35 % of 10000)");
        l = Diff(st, { Snap(kFrostShell, 400, 100, 1, 3500) });
        check(Is(l, 0, kFrostShell, 400, 3500), "added to: 400 of 3500");
        l = Diff(st, { Snap(kFrostShell, 400, 100, 1, 4000) });
        check(l.size() == 1 && Is(l, 0, kFrostShell, 400, 4000), "the cap moved (Winter's Embrace, more health): sent");
        l = Diff(st, { Snap(kFrostShell, 400, 100, 1, 300) });
        check(Is(l, 0, kFrostShell, 400, 400), "the cap never below what is left");
    }

    void TestSeveral()
    {
        State st;
        auto l = Diff(st, Merge({ Snap(kFrostShell, 200, 100, 7, 3500), Snap(kBarrier, 1000, 100, 9) }));
        check(l.size() == 2 && Is(l, 0, kBarrier, 1000, 1000) && Is(l, 1, kFrostShell, 200, 3500), "two shields: two lines");
        l = Diff(st, Merge({ Snap(kBarrier, 900, 100, 9), Snap(kFrostShell, 200, 100, 7, 3500) }));
        check(l.size() == 1 && Is(l, 0, kBarrier, 900, 1000), "only the one that changed");
        l = Diff(st, Merge({ Snap(kFrostShell, 0, 100, 7, 3500) }));
        check(l.size() == 2 && Is(l, 0, kFrostShell, 0, 3500) && Is(l, 1, kBarrier, 0, 0),
              "the shell at 0 but still up: 0 of 3500; Barrier gone: 0;0");
        auto m = Merge({ Snap(500, 100, 50, 4), Snap(500, 250, 60, 3) });
        check(m.size() == 1 && m[0].left == 350 && m[0].applyTime == 60 && m[0].instance == 3 && m[0].fixedCap == -1,
              "two auras of one spell: their amounts add up");
        auto m2 = Merge({ Snap(500, 250, 60, 3), Snap(500, 100, 50, 4) });
        check(m2.size() == 1 && m2[0].instance == m[0].instance, "in either order");
    }
}

int main()
{
    TestFormat();
    TestThrottle();
    TestHighest();
    TestShell();
    TestSeveral();
    std::cout << "absorb tests: " << (g_checks - g_failed) << "/" << g_checks << " passed\n";
    return g_failed ? 1 : 0;
}
