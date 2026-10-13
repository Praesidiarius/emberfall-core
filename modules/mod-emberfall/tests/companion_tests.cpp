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

// Standalone tests of the companion logic that needs no core (src/companions/EfCompanionLogic.h; tests/run_tests.sh
// builds and runs them): the `.efparty add` arguments, the formation, the level scaling numbers (with the stock
// creature_classlevelstats rows of levels 20 and 60), rank choice, the healer's triage, the DPS gates, regen between
// pulls and the EFC frame message.

#include "EfCompanionLogic.h"

#include <cstdio>
#include <iostream>
#include <set>

using namespace Emberfall::Companions;

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

    bool near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }

    void TestRoles()
    {
        check(ParseRole("tank") == RoleRequest::Tank && ParseRole("HEALER") == RoleRequest::Healer && ParseRole("dps") == RoleRequest::Dps &&
                  ParseRole("ranged") == RoleRequest::Ranged && ParseRole("melee") == RoleRequest::Melee && ParseRole("heal") == RoleRequest::Healer,
              "role words");
        check(ParseRole("wizard") == RoleRequest::None && ParseRole("") == RoleRequest::None, "unknown roles");
        check(Fits(RoleRequest::Dps, Role::Melee) && Fits(RoleRequest::Dps, Role::Ranged) && !Fits(RoleRequest::Dps, Role::Healer), "dps fits melee and ranged");
        check(!Fits(RoleRequest::Melee, Role::Ranged) && Fits(RoleRequest::Tank, Role::Tank) && !Fits(RoleRequest::None, Role::Tank), "fits");
    }

    void TestAddArgs()
    {
        AddArgs a = ParseAdd("dps 3", 8);
        check(a.error.empty() && a.role == RoleRequest::Dps && a.count == 3 && a.archetype.empty(), "add dps 3");
        a = ParseAdd("  healer   priest ", 8);
        check(a.error.empty() && a.role == RoleRequest::Healer && a.count == 1 && a.archetype == "priest", "add healer priest");
        a = ParseAdd("ranged Mage 2", 8);
        check(a.error.empty() && a.role == RoleRequest::Ranged && a.count == 2 && a.archetype == "mage", "add ranged mage 2");
        a = ParseAdd("ranged 2 mage", 8);
        check(a.error.empty() && a.count == 2 && a.archetype == "mage", "add ranged 2 mage (count first)");
        check(!ParseAdd("", 8).error.empty(), "add without a role");
        check(!ParseAdd("bard", 8).error.empty(), "add with an unknown role");
        check(!ParseAdd("dps 9", 8).error.empty(), "add more than the maximum");
        check(!ParseAdd("dps 0", 8).error.empty(), "add zero");
        check(!ParseAdd("dps mage rogue", 8).error.empty(), "add two archetypes");
        check(!ParseAdd("dps 2 3", 8).error.empty(), "add two counts");
        check(!ParseAdd("dps 1234", 8).error.empty(), "add a silly count");
    }

    void TestFormation()
    {
        std::set<std::pair<int, int>> spots;
        for (uint32_t i = 0; i < 8; ++i)
        {
            Slot const s = FormationSlot(i);
            check(s.dist >= 2.0f && s.dist <= 6.0f, "slot distance " + std::to_string(i));
            // Behind the owner: the angle is within 90 degrees of pi.
            check(std::fabs(s.angle - 3.14159265f) <= 1.5f, "slot behind " + std::to_string(i));
            spots.insert({ int(std::lround(s.dist * std::cos(s.angle) * 10)), int(std::lround(s.dist * std::sin(s.angle) * 10)) });
        }
        check(spots.size() == 8, "eight different spots");
        check(FormationSlot(8).dist > FormationSlot(0).dist, "a ninth companion goes to an outer ring");
    }

    void TestScaling()
    {
        // creature_classlevelstats, stock: level 20 class 1 (warrior) and class 2 (paladin) rows.
        BaseStats w20{ 484, 0, 836, 9.6823f, 70 };
        uint32_t const palMana20 = 490;
        ArchetypeMods tank{ 2.6f, 0, 2.0f, 1.5f, 0, 2000 };
        Scaled s = Scale(w20, tank, 0);
        check(s.health == 1258, "tank health at 20 (2.6 x 484): " + std::to_string(s.health));
        check(s.armor == 1672, "tank armor at 20: " + std::to_string(s.armor));
        check(s.mana == 0 && s.createMana == 0, "a tank has no mana");
        // Reference: (9.6823 x 1.25 + 70 / 14 x 2) / 2 = 11.05 DPS for a normal level-20 creature.
        check(near(s.referenceDps, 11.05f, 0.02f), "reference DPS at 20: " + std::to_string(s.referenceDps));
        float const avgHit = (s.minDamage + s.maxDamage) / 2.0f;
        check(near(avgHit / 2.0f, 1.5f * s.referenceDps, 0.05f), "tank white DPS = 1.5 x reference");
        check(near(s.minDamage / s.maxDamage, 0.8f / 1.2f), "hits vary +-20 %");

        ArchetypeMods rogue{ 1.7f, 0, 1.0f, 2.6f, 0, 1700 };
        s = Scale(w20, rogue, 0);
        check(near((s.minDamage + s.maxDamage) / 2.0f / 1.7f, 2.6f * s.referenceDps, 0.05f), "rogue white DPS = 2.6 x reference at 1.7 s swings");

        BaseStats p20{ 433, 490, 836, 9.0674f, 64 };
        ArchetypeMods priest{ 1.6f, 4.0f, 0.8f, 0.5f, 0, 2000 };
        s = Scale(p20, priest, palMana20);
        check(s.health == 693 && s.mana == 1960 && s.createMana == 490, "priest at 20: 693 health, 1960 mana, costs from 490");

        ArchetypeMods hunter{ 1.8f, 2.0f, 1.0f, 0.6f, 2.2f, 2000 };
        s = Scale(p20, hunter, palMana20);
        check(s.minRanged > 0 && near((s.minRanged + s.maxRanged) / 2.0f / 2.5f, 2.2f * s.referenceDps, 0.05f), "hunter ranged DPS = 2.2 x reference");

        // Level 60, class 1: damage_base 33.9625, AP 252 -> (42.45 + 36) / 2 = 39.2.
        BaseStats w60{ 3052, 0, 3750, 33.9625f, 252 };
        s = Scale(w60, tank, 0);
        check(near(s.referenceDps, 39.23f, 0.05f) && s.health == 7935, "level 60 tank: " + std::to_string(s.health));
        check(Scale(BaseStats{}, tank, 0).health == 1, "never zero health");
    }

    void TestRanks()
    {
        // Heal: 2054 (16), 2055 (22), 6063 (28), 6064 (34).
        std::vector<std::pair<uint32_t, uint32_t>> heal{ { 2054, 16 }, { 2055, 22 }, { 6063, 28 }, { 6064, 34 } };
        check(RankForLevel(heal, 15) == 0, "no Heal before 16");
        check(RankForLevel(heal, 16) == 2054 && RankForLevel(heal, 21) == 2054, "Heal 1 at 16-21");
        check(RankForLevel(heal, 22) == 2055 && RankForLevel(heal, 60) == 6064, "Heal 2 at 22, 4 at 60");
        check(RankForLevel({}, 60) == 0, "an empty chain");
    }

    void TestHeal()
    {
        HealConfig const c;
        // Nobody hurt, out of combat: nothing; the tank without its shield in combat: the shield.
        std::vector<HealTarget> all{ { 0, 100, false }, { 1, 100, true }, { 2, 100, false } };
        check(ChooseHeal(all, 100, false).kind == HealKind::None, "nothing to heal");
        HealChoice h = ChooseHeal(all, 100, true);
        check(h.kind == HealKind::Shield && h.id == 1, "the tank's shield in combat");
        all[1].hasShield = true;
        check(ChooseHeal(all, 100, true).kind == HealKind::None, "shielded, topped: nothing");

        // Triage: the tank at 60 % beats a DPS at 55 % (weight 1.35), and gets a big heal (tank threshold 75).
        std::vector<HealTarget> g{ { 0, 55, false }, { 1, 60, true }, { 2, 90, false } };
        h = ChooseHeal(g, 80, true);
        check(h.id == 1 && h.kind == HealKind::Big, "tank first, big heal: id " + std::to_string(h.id));
        // But a DPS in real danger (30 %) beats the tank at 60 %: fast heal.
        g[0].hpPct = 30;
        h = ChooseHeal(g, 80, true);
        check(h.id == 0 && h.kind == HealKind::Fast, "a DPS at 30 %: fast heal");
        // Small heal band, HoT band.
        std::vector<HealTarget> s{ { 0, 78, false }, { 1, 100, true, true, false, true } };
        check(ChooseHeal(s, 80, true).kind == HealKind::Small, "a DPS at 78 %: small heal");
        s[0].hpPct = 88;
        h = ChooseHeal(s, 80, true);
        check(h.kind == HealKind::Hot && h.id == 0, "a DPS at 88 %: HoT");
        s[0].hasHot = true;
        check(ChooseHeal(s, 80, true).kind == HealKind::None, "HoT already on: nothing");
        // Low mana: no small heals, no HoTs, no shield; still big and fast ones.
        std::vector<HealTarget> lm{ { 0, 70, false }, { 1, 100, true } };
        check(ChooseHeal(lm, 20, true).kind == HealKind::None, "low mana: a DPS at 70 % waits");
        lm[0].hpPct = 50;
        check(ChooseHeal(lm, 20, true).kind == HealKind::Big, "low mana: a DPS at 50 % gets a big heal");
        // Dead and out-of-range members are skipped.
        std::vector<HealTarget> dr{ { 0, 0, false, false }, { 1, 20, false, true, false, false, false } };
        check(ChooseHeal(dr, 100, true).kind == HealKind::None, "dead and out of range: nothing");
        // The HoT goes to the tank first.
        std::vector<HealTarget> ht{ { 0, 85, false }, { 1, 90, true, true, false, true } };
        h = ChooseHeal(ht, 100, true);
        check(h.kind == HealKind::Hot && h.id == 1, "the HoT to the tank first");
    }

    void TestGates()
    {
        DpsGate const g;
        check(MayStart(g, false, 0, 0), "no tank: start at once");
        check(!MayStart(g, true, 0, 1000), "the tank does not have it: wait");
        check(!MayStart(g, true, 1500, 100), "1.5 s, little threat: wait");
        check(MayStart(g, true, 2000, 0), "2 s: go");
        check(MayStart(g, true, 400, 300), "enough threat early: go");
        check(!MustHold(g, false, 5000, 100), "a loose mob: never hold");
        check(!MustHold(g, true, 700, 1000), "70 % of the tank's threat: keep going");
        check(MustHold(g, true, 800, 1000), "80 %: hold");
        check(MustHold(g, true, 1, 0), "the tank has no threat yet: hold");
        check(!MustHold(g, true, 500, 1000, 200) && MustHold(g, true, 500, 1000, 560), "the next hit would pass 105 %: hold");
        DpsGate loose{ 0, 0, 1.10f };
        check(!MustHold(loose, true, 1000, 1000) && MustHold(loose, true, 1060, 1000), "a looser cap still stops at 105 %");
    }

    void TestRegen()
    {
        RegenRule const r;
        check(WantsRegen(r, 70, 100, true) && WantsRegen(r, 100, 60, true) && !WantsRegen(r, 100, 60, false), "eat or drink below the marks");
        check(!WantsRegen(r, 85, 75, true), "above the marks: no");
        check(!RegenDone(r, 97, 100, true) && RegenDone(r, 98, 99, true) && RegenDone(r, 99, 0, false), "done at 98 %");
    }

    void TestFrame()
    {
        check(Hex(0xF130000E1A000123ULL) == "F130000E1A000123" && Hex(0) == "0000000000000000", "hex guids");
        std::vector<FrameEntry> e{
            { 0xF130000E1A000001ULL, Role::Tank, "warrior", 100, -1, 'F' },
            { 0xF130000E1A000002ULL, Role::Healer, "priest", 55, 73, 'C' },
            { 0xF130000E1A000003ULL, Role::Ranged, "ma;ge,x", 0, 0, 'D' },
        };
        std::string const body = FrameBody(e);
        check(body == "1;3;F130000E1A000001,T,warrior,100,-,F;F130000E1A000002,H,priest,55,73,C;F130000E1A000003,R,ma_ge_x,0,0,D",
              "frame body: " + body);
        // Sixteen companions do not fit: the body stays within 251 bytes, the count says 16.
        std::vector<FrameEntry> many(16, FrameEntry{ 0xF130000E1A000001ULL, Role::Melee, "rogue", 100, -1, 'F' });
        std::string const b2 = FrameBody(many);
        check(b2.size() <= 251 && b2.rfind("1;16;", 0) == 0, "a long roster is cut at 251 bytes");
        FrameEntry big{ 1, Role::Melee, "rogue", 250, 300, 'C' };
        check(FrameBody({ big }) == "1;1;0000000000000001,M,rogue,100,100,C", "percentages are capped at 100");
    }
}

int main()
{
    TestRoles();
    TestAddArgs();
    TestFormation();
    TestScaling();
    TestRanks();
    TestHeal();
    TestGates();
    TestRegen();
    TestFrame();
    std::printf("companion tests: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
