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

// Standalone tests of the Infusionist's arithmetic (src/classkit/EfInfuseLogic.h; tests/run_tests.sh builds and runs them):
// the prepared potions (spend, the queue, recharges at once, the rate change, restores by priority, Perfect Formula), the
// EFP body, the independent applications and the chain's percents. The numbers are examples fed to the functions, not the
// kit's values (those: the game repo's content/data/classes/mage.yaml).

#include "EfInfuseLogic.h"

#include <iostream>
#include <string>

using namespace Emberfall::ClassKit::Infuse;

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

    bool Sane(Potions const& p)
    {
        return p.ready + p.recharging.size() + p.queued == p.vials;
    }
}

int main()
{
    // spending: three recharge at once, the rest wait
    {
        Potions p = Full(10);
        check(Spend(p, 2, 0, 3, 8000) == 2 && p.ready == 8 && p.recharging.size() == 2 && p.queued == 0 && Sane(p), "2 spent, 2 recharging");
        Spend(p, 2, 0, 3, 8000);
        check(p.ready == 6 && p.recharging.size() == 3 && p.queued == 1 && Sane(p), "a fourth waits in the queue");
        check(!CanPay(Full(1), 2, 0) && CanPay(Full(2), 2, 0), "a 2-potion cast needs 2");
    }
    // recharging: 8 s each, the queued one starts in the freed slot with the time left over
    {
        Potions p = Full(10);
        Spend(p, 4, 0, 3, 8000);
        check(Advance(p, 7999, 3, 8000) == 0 && p.ready == 6, "nothing back before 8 s");
        check(Advance(p, 1, 3, 8000) == 3 && p.ready == 9 && p.recharging.size() == 1 && p.queued == 0 && Sane(p),
              "three back at 8 s, the fourth starts");
        check(p.recharging[0].leftMs == 8000, "the fourth runs a full 8 s");
        Advance(p, 8500, 3, 8000);
        check(p.ready == 10 && p.recharging.empty() && Sane(p), "all ready after 16 s");
        Potions q = Full(10);
        Spend(q, 4, 0, 3, 8000);
        Advance(q, 9000, 3, 8000);
        check(q.ready == 9 && q.recharging.size() == 1 && q.recharging[0].leftMs == 7000, "a long tick carries over");
    }
    // the rate: a running recharge keeps its share
    {
        Potions p = Full(10);
        Spend(p, 1, 0, 3, 8000);
        Advance(p, 4000, 3, 8000);
        Rescale(p, 3000);
        check(p.recharging[0].leftMs == 1500 && p.recharging[0].totalMs == 3000, "half done at 8 s: 1.5 of 3 s left");
        Rescale(p, 8000);
        check(p.recharging[0].leftMs == 4000 && p.recharging[0].totalMs == 8000, "and back");
        // more at once: the queued start; fewer: the running ones run on
        Potions q = Full(10);
        Spend(q, 6, 0, 3, 8000);
        StartQueued(q, 5, 3000);
        check(q.recharging.size() == 5 && q.queued == 1 && Sane(q), "Expanded Production: 5 at once");
        Advance(q, 1000, 3, 8000);
        check(q.recharging.size() == 5 && Sane(q), "back to 3 at once: the 5 running run on");
    }
    // restores: the queue first, then the one with the most time left; all ready: nothing
    {
        Potions p = Full(10);
        Spend(p, 4, 0, 3, 8000);
        check(Restore(p, 1, 3, 8000) == 1 && p.queued == 0 && p.recharging.size() == 3 && p.ready == 7 && Sane(p), "the queued one first");
        Advance(p, 2000, 3, 8000);
        Spend(p, 1, 0, 3, 8000);
        check(p.recharging.size() == 3 && p.queued == 1, "full slots again");
        Restore(p, 2, 3, 8000);
        check(p.queued == 0 && p.recharging.size() == 2 && Sane(p), "then the longest recharge");
        check(p.recharging[0].leftMs == 6000 && p.recharging[1].leftMs == 6000, "the newest one went");
        Potions full = Full(10);
        check(Restore(full, 2, 3, 8000) == 0 && full.ready == 10, "all ready: nothing more");
    }
    // Perfect Formula: every fifth potion free
    {
        Potions p = Full(10);
        for (int i = 0; i < 4; ++i)
            Spend(p, 1, 5, 3, 8000);
        check(p.ready == 6 && p.pf == 4 && NextFree(p, 5), "four spent: the next is free");
        check(Spend(p, 1, 5, 3, 8000) == 0 && p.ready == 6 && p.pf == 0, "the fifth takes no vial");
        check(Paid(2, 4, 5) == 1 && Paid(2, 3, 5) == 1 && Paid(2, 2, 5) == 2, "a 2-potion cast: one free when either is");
        Potions empty = Full(10);
        empty.ready = 0;
        empty.queued = 10;
        empty.pf = 4;
        check(CanPay(empty, 1, 5) && !CanPay(empty, 2, 5) && !Depleted(empty, 5), "at 0 ready a free 1-potion cast goes");
        empty.pf = 3;
        check(Depleted(empty, 5) && Depleted(empty, 0), "no free potion: Arcane Restoration");
        check(AdvanceFormula(1, 2, 5) == 3 && AdvanceFormula(3, 2, 5) == 4 && AdvanceFormula(4, 2, 5) == 4,
              "restores advance the count, never past the free one");
        check(AdvanceFormula(2, 2, 0) == 0, "no Perfect Formula: no count");
    }
    // the message
    {
        Potions p = Full(10);
        Spend(p, 4, 5, 3, 8000);
        Advance(p, 500, 3, 8000);
        check(Message(p, 5) == "1;6;10;1;4;5;7500/8000,7500/8000,7500/8000", "EFP body: " + Message(p, 5));
        check(Message(Full(10), 0) == "1;10;10;0;0;0;", "a full store, no Perfect Formula");
        check(OffMessage() == "1;0;0;0;0;0;", "off");
        check(Shape(p, 5) != Shape(Full(10), 5), "a different state, a different shape");
        Potions later = p;
        Advance(later, 100, 3, 8000);
        check(Shape(later, 5) == Shape(p, 5), "time passing alone does not change the shape");
    }
    // Volatile Flask's applications
    {
        std::vector<std::uint64_t> ends = { 1000, 5000, 9000 };
        check(Prune(ends, 1000) == 2 && LongestLeft(ends, 1000) == 8000, "the oldest ran out");
        check(Prune(ends, 9000) == 0 && LongestLeft(ends, 9000) == 0, "all ran out");
    }
    // Flowing Remedy
    check(JumpPct(0, 10) == 100 && JumpPct(2, 10) == 120 && JumpPct(4, 10) == 140 && JumpPct(3, 0) == 100, "the jumps' percents");

    std::cout << "classkit infuse tests: " << (g_checks - g_failed) << "/" << g_checks << " passed\n";
    return g_failed ? 1 : 0;
}
