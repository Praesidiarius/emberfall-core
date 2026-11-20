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

// Standalone tests of the Corsair rework's rules (ankarna's "Gameplay & Survivability Rework", 2026-11-19; Emberfall
// docs/design/classes/warrior-corsair.md §21; src/classkit/EfFlowLogic.h; tests/run_tests.sh builds and runs them):
// No Quarter's end keeps a prepared finisher, its start resets an incomplete Flow and keeps a prepared one; the parry
// window counts the engine's base in; a linked aura holds with its carrier and its rule. The finisher tables are the
// hand-off's own example (its builder letters), fed to the functions: the game's data lives in the game repo.

#include "EfFlowLogic.h"

#include <iostream>
#include <string>

using namespace Emberfall::ClassKit::Flow;

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

    // builders: 0 Quickcut (A), 1 Crosscurrent (B), 2 Piercing Strike (C); finishers: 1 Keelbreaker, 2 Razorwake, 3 Stormwake
    enum : int { QC = 0, CC = 1, PS = 2 };
    enum : uint8_t { NONE = 0, KEEL = 1, RAZOR = 2, STORM = 3 };

    FlowTable HandOff()
    {
        FlowTable t;
        t.pair[QC][CC] = t.pair[CC][QC] = KEEL;
        t.pair[QC][PS] = t.pair[PS][QC] = RAZOR;
        t.pair[CC][PS] = t.pair[PS][CC] = STORM;
        t.own[QC] = RAZOR;     // No Quarter: Quickcut -> Razorwake
        t.own[CC] = STORM;     //             Crosscurrent -> Stormwake
        t.own[PS] = KEEL;      //             Piercing Strike -> Keelbreaker
        return t;
    }
}

int main()
{
    FlowTable const t = HandOff();

    // the pairs, order irrelevant, and a repeat builds nothing
    {
        FlowState s;
        s = AfterBuilder(s, t, QC);
        check(s.marks == 1 && s.ready == NONE, "a builder alone lays its mark");
        s = AfterBuilder(s, t, QC);
        check(s.marks == 1 && s.ready == NONE, "the same builder twice completes nothing");
        s = AfterBuilder(s, t, CC);
        check(s.marks == 0 && s.ready == KEEL, "Quickcut + Crosscurrent: Keelbreaker (the new start)");
        s = AfterBuilder(s, t, PS);
        check(s.ready == KEEL && s.marks == 0, "a prepared finisher: builders change nothing");
        s = AtFinisher(s);
        check(s.ready == NONE, "the finisher spends it");
        s = AfterBuilder(AfterBuilder(FlowState{}, t, PS), t, QC);
        check(s.ready == RAZOR, "Piercing Strike + Quickcut: Razorwake");
        s = AfterBuilder(AfterBuilder(FlowState{}, t, PS), t, CC);
        check(s.ready == STORM, "Piercing Strike + Crosscurrent: Stormwake");
    }

    // the hand-off's example: No Quarter has 0.5 s left, Quickcut prepares Razorwake, No Quarter ends, Razorwake stays and
    // is used (20 Energy: the Flow Finisher's own cost, not the state's), then normal two-builder Flow
    {
        FlowState s = AtNoQuarterCast(FlowState{});
        s = AfterBuilder(s, t, QC);
        check(s.ready == RAZOR && s.marks == 0, "No Quarter: Quickcut prepares Razorwake at once, no mark");
        s = AtNoQuarterEnd(s);
        check(s.ready == RAZOR && !s.noQuarter, "No Quarter ends: Razorwake stays prepared");
        s = AfterBuilder(s, t, CC);
        check(s.ready == RAZOR && s.marks == 0, "after the end, with Razorwake prepared a builder changes nothing");
        s = AtFinisher(s);
        check(s.ready == NONE, "Razorwake is used and spent");
        s = AfterBuilder(s, t, QC);
        check(s.marks == 1 && s.ready == NONE, "then normal Flow: one builder is a mark only");
        s = AfterBuilder(s, t, PS);
        check(s.ready == RAZOR, "and two prepare the pair's finisher");
    }

    // the three mappings, each kept past the end
    for (int b : { QC, CC, PS })
    {
        FlowState s = AtNoQuarterEnd(AfterBuilder(AtNoQuarterCast(FlowState{}), t, b));
        check(s.ready == t.own[b], "No Quarter's mapping of builder " + std::to_string(b) + " survives its end");
    }

    // activation: an incomplete Flow is reset, a prepared finisher stays
    {
        FlowState s = AfterBuilder(FlowState{}, t, CC);
        s = AtNoQuarterCast(s);
        check(s.marks == 0 && s.noQuarter, "No Quarter's cast resets an incomplete Flow");
        FlowState r = AfterBuilder(AfterBuilder(FlowState{}, t, QC), t, CC);
        r = AtNoQuarterCast(r);
        check(r.ready == KEEL, "No Quarter's cast keeps a prepared finisher");
        r = AfterBuilder(r, t, PS);
        check(r.ready == KEEL, "under No Quarter a prepared finisher still holds the builders");
    }

    // the parry window (Defensive Momentum): 20 in all for a character who could not parry, 20 more for one who could
    {
        check(kEngineBaseParryPct == 5, "the engine's base parry is 5 %");
        check(ParryWindowAmount(20, false) == 15, "no parry before: the aura gives 15, the engine's base 5: 20 in all");
        check(ParryWindowAmount(20, true) == 20, "a character who parries already gets the whole 20 on top");
        check(ParryWindowAmount(3, false) == 0 && ParryWindowAmount(0, true) == 0, "never below 0");
    }

    // the linked aura: on with Momentum and the talent; off when Momentum drops, or refreshed without the talent
    {
        check(LinkedAuraOn(true, true), "Momentum up with Defensive Momentum: the parry is on");
        check(!LinkedAuraOn(false, true), "Momentum drops: the parry goes");
        check(!LinkedAuraOn(true, false), "Momentum refreshed without the talent: no parry");
    }

    // a channel's hold reach: its range and 10 %, at most 3 yd more (Cutlass Barrage's 5 yd: 5.5)
    {
        check(ChannelHoldRange(5.0f) > 5.49f && ChannelHoldRange(5.0f) < 5.51f, "a 5 yd channel holds to 5.5 yd");
        check(ChannelHoldRange(40.0f) > 42.99f && ChannelHoldRange(40.0f) < 43.01f, "at most 3 yd more");
        check(ChannelHoldRange(0.0f) == 0.0f, "no range, no check");
    }

    std::cout << "classkit flow: " << (g_checks - g_failed) << "/" << g_checks << " passed\n";
    return g_failed == 0 ? 0 : 1;
}
