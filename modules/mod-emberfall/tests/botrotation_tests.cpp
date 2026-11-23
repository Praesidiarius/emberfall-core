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

// Standalone tests of the bots' routine conditions (mod-playerbots' src/Emberfall/EmberfallRotationLogic.h;
// tests/run_tests.sh builds and runs them): the encoding the game repo's botpilot writes, each condition and its
// negation, the time to live.

#include "EmberfallRotationLogic.h"

#include <iostream>
#include <map>
#include <string>

using namespace EmberfallRotation::Logic;

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

    struct Fake : Facts
    {
        float power = 0.0f;
        std::map<uint32_t, float> self, target, cooldown;   // seconds left
        std::map<uint32_t, uint32_t> selfStacks, targetStacks;
        uint32_t enemies = 1;
        float ttl = -1.0f;
        bool casting = false;
        int enemyAsks = 0;

        float Power() override { return power; }
        float SelfAuraLeft(uint32_t s) override { auto it = self.find(s); return it == self.end() ? -1.0f : it->second; }
        float TargetAuraLeft(uint32_t s) override { auto it = target.find(s); return it == target.end() ? -1.0f : it->second; }
        uint32_t SelfStacks(uint32_t s) override { auto it = selfStacks.find(s); return it == selfStacks.end() ? 0 : it->second; }
        uint32_t TargetStacks(uint32_t s) override { auto it = targetStacks.find(s); return it == targetStacks.end() ? 0 : it->second; }
        float CooldownLeft(uint32_t s) override { auto it = cooldown.find(s); return it == cooldown.end() ? -1.0f : it->second; }
        uint32_t Enemies() override { ++enemyAsks; return enemies; }
        float TimeToLive() override { return ttl; }
        bool TargetCastsInterruptible() override { return casting; }
        bool queued = false;
        float swing = 100.0f;
        bool SwingQueued() override { return queued; }
        float SwingProgress() override { return swing; }
    };

    bool Run(std::string const& text, Fake& f) { return AllHold(Parse(text), f); }
}

int main()
{
    check(Parse("P:0:100;!S:94208:0;T:96142:3").size() == 3, "three items parse");
    check(Parse("X:1:2;P:0:5;bad;P:1").size() == 1, "unknown ops and malformed items are left out");
    Fake f;
    check(Run("", f), "no condition: the step goes");

    f.power = 99.0f;
    check(!Run("P:0:100", f), "power >= 100 waits at 99 (Supernova at 100 Heat)");
    f.power = 100.0f;
    check(Run("P:0:100", f) && !Run("!P:0:100", f), "power >= 100 at 100; power < 100 does not");

    check(!Run("S:94208:0", f) && Run("!S:94208:0", f), "an aura the bot lacks: `self` fails, `no self` holds");
    f.self[94208] = 6.0f;
    check(Run("S:94208:0", f) && Run("S:94208:5", f) && !Run("S:94208:7", f), "self KEY >= S counts the seconds left");
    check(Run("!S:94208:7", f) && !Run("!S:94208:5", f), "self KEY < S: missing or under S left (the refresh window)");

    check(Run("!T:96142:3", f), "a damage over time missing on the target is due");
    f.target[96142] = 2.0f;
    check(Run("!T:96142:3", f) && !Run("T:96142:3", f), "... and one with 2 s left is due at < 3");
    f.target[96142] = 9.0f;
    check(!Run("!T:96142:3", f), "... one with 9 s left is not");

    f.selfStacks[93420] = 4;
    check(!Run("K:93420:5", f) && Run("!K:93420:5", f), "4 Combo Points: the finisher at 5 waits, the builder goes");
    f.selfStacks[93420] = 5;
    check(Run("K:93420:5", f), "5 Combo Points: the finisher goes");
    f.targetStacks[1] = 3;
    check(Run("U:1:3", f) && !Run("U:1:4", f), "target stacks");

    check(!Run("R:94215:3", f) && Run("!R:94215:3", f), "an unknown big button never holds the spender back");
    f.cooldown[94215] = 2.0f;
    check(Run("R:94215:3", f) && !Run("!R:94215:3", f), "a cooldown 2 s from ready: `not ready < 3` holds the spender back");
    f.cooldown[94215] = 0.0f;
    check(Run("R:94215:0", f), "a ready cooldown is ready within 0 s");

    f.enemies = 2;
    check(!Run("E:0:3", f) && Run("!E:0:3", f), "two enemies: no area step");
    f.enemies = 3;
    check(Run("E:0:3", f), "three enemies: the area step");

    f.ttl = -1.0f;
    check(Run("L:0:8", f) && !Run("!L:0:8", f), "an unknown time to live: a long damage over time goes, `ttl < 8` does not");
    f.ttl = 5.0f;
    check(!Run("L:0:8", f) && Run("!L:0:8", f), "a target 5 s from death: no damage over time of 8 s");

    check(!Run("C:0:0", f), "no interrupt without a cast");
    f.casting = true;
    check(Run("C:0:0", f), "the interrupt on an interruptible cast");

    // the Executioner v4.0's queued swings: the GCD buttons wait while one is queued, the queue waits for the sweet spot
    check(!Run("N:0:0", f) && Run("!N:0:0", f), "nothing queued: `not queued` holds");
    f.queued = true;
    check(Run("N:0:0", f) && !Run("!N:0:0", f), "a swing queued: Deathblow waits (`not queued` fails)");
    f.queued = false;
    std::string const sweet = "!N:0:0;W:0:60;!W:0:80";
    f.swing = 59.0f;
    check(!Run(sweet, f), "59 % of the swing: Final Verdict waits for the sweet spot");
    f.swing = 70.0f;
    check(Run(sweet, f), "70 %: queued in the sweet spot");
    f.swing = 80.0f;
    check(!Run(sweet, f), "80 %: `swing < 80` no longer holds");
    f.swing = 100.0f;
    check(!Run("!W:0:80", f), "no running swing (100 %): never the sweet spot");

    Fake lazy;
    lazy.power = 0.0f;
    check(!Run("P:0:50;E:0:3", lazy) && lazy.enemyAsks == 0, "the first failing condition ends the check (enemies not counted)");

    check(TimeToLive(1000, 1000, 10.0f) < 0.0f, "no damage taken: unknown");
    check(TimeToLive(1000, 800, 2.0f) < 0.0f, "the first seconds: unknown");
    float const t = TimeToLive(1000, 600, 4.0f);
    check(t > 5.9f && t < 6.1f, "400 taken in 4 s, 600 left: 6 s to live");

    // the kiting step (ef_bot_spec.kite, the Marksman's notes of 2026-11-19)
    KiteFacts k;
    k.kite = k.ranged = true;
    k.foeDistance = 3.0f;
    k.nowMs = 100000;
    check(KiteStep(k), "kite: a foe in reach of a ranged kiting bot: a step away");
    KiteFacts no = k;
    no.kite = false;
    check(!KiteStep(no), "kite: not a spec that kites");
    no = k;
    no.ranged = false;
    check(!KiteStep(no), "kite: not while it fights in melee");
    no = k;
    no.foeDistance = 12.0f;
    check(!KiteStep(no), "kite: the foe out of reach: it shoots");
    no.foeDistance = -1.0f;
    check(!KiteStep(no), "kite: no foe on it");
    no = k;
    no.rooted = true;
    check(!KiteStep(no), "kite: rooted, no step");
    no = k;
    no.grouped = true;
    check(!KiteStep(no), "kite: in a group the tank holds it");
    no = k;
    no.castingPlanted = true;
    check(!KiteStep(no), "kite: a planted cast is not broken");
    no = k;
    no.lastStepMs = k.nowMs - 2000;
    check(!KiteStep(no), "kite: one step every 6 s, shooting between");
    no.lastStepMs = k.nowMs - 6000;
    check(KiteStep(no), "kite: the next step after 6 s");

    std::cout << "botrotation: " << (g_checks - g_failed) << "/" << g_checks << " checks passed\n";
    return g_failed ? 1 : 0;
}
