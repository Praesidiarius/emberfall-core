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

// Standalone tests of the personal quest objects' logic (src/questloot/EfQuestLootLogic.h; tests/run_tests.sh builds and
// runs them): the release-versus-despawn rule, the per-player lockouts, hiding after the loot window closes, the respawn.

#include "EfQuestLootLogic.h"

#include <iostream>
#include <set>
#include <string>

using namespace Emberfall::QuestLoot;

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

    constexpr uint64_t kA = 0x10, kB = 0x20, kC = 0x30;

    void TestKeep()
    {
        check(KeepOnStateChange(kGoJustDeactivated, true, 0), "a spawned object's emptied loot released: kept");
        check(!KeepOnStateChange(kGoJustDeactivated, true, 1700000000), "a despawn (respawn time set first): despawns");
        check(!KeepOnStateChange(kGoJustDeactivated, false, 0), "a summoned object: the stock path");
        check(!KeepOnStateChange(kGoReady, true, 0), "back to ready: nothing to do");
        check(!KeepOnStateChange(2, true, 0), "activated (opened): nothing to do");
    }

    void TestLockouts()
    {
        Lockouts l;
        check(l.Empty() && !l.Locked(kA, 100), "nobody is locked out of a fresh object");
        l.Looted(kA, 100, 60);
        check(l.Locked(kA, 100) && l.Locked(kA, 159), "A took the item: locked for the respawn time");
        check(!l.Locked(kA, 160), "and free at the respawn");
        check(!l.Locked(kB, 120), "B, in the same group, is not locked out");
        l.Looted(kB, 130, 60);
        check(l.Size() == 2, "two lockouts");
        check(l.TakeExpired(159).empty(), "none ended at 159");
        auto expired = l.TakeExpired(160);
        check(expired.size() == 1 && expired[0] == kA, "A's ended at 160");
        check(!l.Locked(kA, 161) && l.Locked(kB, 161), "A free, B still locked");
        expired = l.TakeExpired(500);
        check(expired.size() == 1 && expired[0] == kB && l.Size() == 0, "B's ended too");
    }

    void TestMinimum()
    {
        Lockouts l;
        l.Looted(kA, 100, 0);
        check(l.Locked(kA, 100 + kMinLockoutSeconds - 1), "spawntimesecs 0: still the minimum lockout");
        check(!l.Locked(kA, 100 + kMinLockoutSeconds), "ends after the minimum");
        Lockouts m;
        m.Looted(kA, 100, 3);
        check(m.Locked(kA, 105), "a few seconds' respawn: raised to the minimum");
    }

    void TestHide()
    {
        Lockouts l;
        std::set<uint64_t> looting{ kA };
        auto isLooting = [&looting](uint64_t p) { return looting.count(p) > 0; };
        l.Looted(kA, 100, 60);
        l.Looted(kA, 100, 60);   // two items from the same loot: one hide
        check(l.TakeHideReady(100, isLooting).empty(), "A's loot window is still open: not hidden yet");
        looting.clear();
        auto ready = l.TakeHideReady(101, isLooting);
        check(ready.size() == 1 && ready[0] == kA, "A closed the window: hide once");
        check(l.TakeHideReady(102, isLooting).empty(), "and only once");
        l.Looted(kB, 100, 60);
        looting.insert(kB);
        check(l.TakeHideReady(170, isLooting).empty(), "B's lockout ended before the window closed: nothing to hide");
        looting.clear();
        check(l.TakeHideReady(171, isLooting).empty(), "B was dropped from the waiting list");
        l.Looted(kC, 200, 60);
        check(!l.Empty(), "C waits");
        l.TakeHideReady(200, isLooting);
        l.TakeExpired(300);
        check(l.Empty(), "everything ended: empty (the AI's update is a no-op)");
    }

    void TestRelock()
    {
        Lockouts l;
        l.Looted(kA, 100, 60);
        l.TakeExpired(160);
        l.Looted(kA, 170, 60);
        check(l.Locked(kA, 200) && !l.Locked(kA, 230), "after the respawn A may take it again and is locked anew");
    }
}

int main()
{
    TestKeep();
    TestLockouts();
    TestMinimum();
    TestHide();
    TestRelock();
    std::cout << "questloot tests: " << (g_checks - g_failed) << "/" << g_checks << " passed\n";
    return g_failed ? 1 : 0;
}
