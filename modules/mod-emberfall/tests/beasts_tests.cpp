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

// The Hunter's beasts, part 1 (src/beasts/EfBeastsLogic.h and the core's src/server/game/AI/CoreAI/EmberfallPetPriority.h,
// PATCHES.md #46; Emberfall docs/design/hunter-beasts.md §1-§3): the stalls, the tame choice, the priority, the refused
// hand casts, EFB. Rules only: no kind, spell or number of the content is pinned.

#include "EfBeastKinLogic.h"
#include "EfBeastsLogic.h"
#include "EmberfallPetPriority.h"

#include <array>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace
{
    int g_checks = 0;
    int g_failed = 0;

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
    namespace B = Emberfall::Beasts;

    // ---- the stalls
    check(B::OpenStalls(2, 80) == 80, "a hunter who bought two stalls has them all");
    check(B::OpenStalls(0, 80) == 80, "a new hunter has them all");
    check(B::OpenStalls(90, 80) == 90, "never fewer than he had");
    {
        std::vector<bool> used = {true, true, false, true};
        check(B::FirstFree(used, 4) == 2, "the first free stall");
        check(B::FirstFree(used, 2) == -1, "only the open stalls count");
        std::vector<bool> full(80, true);
        check(B::FirstFree(full, 80) == -1, "a full stable");
        full[79] = false;
        check(B::FirstFree(full, 80) == 79, "the last stall");
    }

    // ---- the tame choice
    {
        auto p = B::PlanTame(B::TameTo::WithMe, true);
        check(p.putAwayFirst && !p.swapBack && !p.stableNew, "with me: the old one goes to the stable, the new one stays out");
        p = B::PlanTame(B::TameTo::WithMe, false);
        check(!p.putAwayFirst && !p.swapBack && !p.stableNew, "with me, nothing out: the new one comes out");
        p = B::PlanTame(B::TameTo::Stable, true);
        check(p.putAwayFirst && p.swapBack && !p.stableNew, "to the stable: the new one takes the old one's stall, the old one is back");
        p = B::PlanTame(B::TameTo::Stable, false);
        check(!p.swapBack && p.stableNew, "to the stable, nothing out: the new one goes into a free stall");
    }

    // ---- the priority
    {
        std::vector<B::PriorityRow> rows = {{10, 0, true}, {20, 0, false}, {30, 15, false}, {40, 0, false}};
        std::map<uint32_t, B::SpellState> pet = {
            {10, {true, true, 0}}, {20, {true, false, 0}}, {30, {true, false, 25}}, {40, {true, false, 25}}};
        auto stateOf = [&pet](uint32_t s) { auto it = pet.find(s); return it == pet.end() ? B::SpellState{} : it->second; };
        std::vector<uint32_t> out;
        B::Order(rows, 100, stateOf, out);
        check((out == std::vector<uint32_t>{10, 20, 30, 40}), "the rows in their order");
        check(!pet[20].toggledOn && out[1] == 20, "a spell toggled off is still cast: toggles do not change the fight");
        pet[10].toggledOn = false;
        B::Order(rows, 100, stateOf, out);
        check((out == std::vector<uint32_t>{20, 30, 40}), "the taunt waits while the player has it off");
        B::Order(rows, 30, stateOf, out);
        check((out == std::vector<uint32_t>{20, 40}), "a row with a reserve waits until the power covers it");
        pet.erase(20);
        B::Order(rows, 100, stateOf, out);
        check((out == std::vector<uint32_t>{30, 40}), "a spell the pet does not know is skipped");
        pet[30].heldBack = true;
        B::Order(rows, 100, stateOf, out);
        check((out == std::vector<uint32_t>{40}), "a special its rule holds back is skipped (the beasts' part 2)");
        pet[30].heldBack = false;
        check(Emberfall::PetPriority::Pick(true, 3) == 0, "an ordered companion casts the first that can go");
        check(Emberfall::PetPriority::Pick(false, 3) == 3, "anything else: upstream's random pick");
        std::vector<uint32_t> none;
        check(!Emberfall::PetPriority::List(nullptr, none) && none.empty(), "no list function: upstream's");
    }

    // ---- the refused hand casts
    {
        check(B::ActState(0xC1000000u | 93252u) == 0xC1 && B::ActSpell(0xC1000000u | 93252u) == 93252u, "the packed pet action");
        check(B::IsSpellAct(0xC1) && B::IsSpellAct(0x81) && B::IsSpellAct(0x01), "spell buttons");
        check(!B::IsSpellAct(0x07) && !B::IsSpellAct(0x06) && !B::IsSpellAct(0x00), "commands and reactions are no spells");
        check(B::RefuseHandCast(true, true, true), "a hand cast of a companion's spell is refused");
        check(!B::RefuseHandCast(true, true, false), "attack, follow, stay and the reactions stay the player's");
        check(!B::RefuseHandCast(true, false, true), "other pets as upstream");
        check(!B::RefuseHandCast(false, true, true), "stock data as upstream");
    }

    // ---- EFB
    {
        auto r = B::Parse("TAMETO S");
        check(r && r->kind == B::Request::Kind::TameTo && r->to == B::TameTo::Stable, "TAMETO S");
        r = B::Parse("TAMETO W");
        check(r && r->to == B::TameTo::WithMe, "TAMETO W");
        check(!B::Parse("TAMETO X") && !B::Parse("TAMETO") && !B::Parse("TAMETO S S"), "a bad TAMETO");
        r = B::Parse("CALL 1234");
        check(r && r->kind == B::Request::Kind::Call && r->pet == 1234, "CALL");
        check(!B::Parse("CALL 0") && !B::Parse("CALL -1") && !B::Parse("CALL 99999999999") && !B::Parse("CALL 12a"), "a bad CALL");
        check(B::Parse("LIST") && B::Parse("HELLO") && !B::Parse("") && !B::Parse("NOPE"), "the other verbs");
        check(B::Pref(B::TameTo::Stable) == "PREF S" && B::Called(7) == "CALLED 7", "the answers");
        check(B::Taunts({5, 9}, true) == "TAUNTS 5,9" && B::Taunts({}, true) == "TAUNTS 0" && B::Taunts({5}, false) == "TAUNTS",
              "TAUNTS: the toggles that stay, or none when the server does not order the companions");
        for (std::string const& junk : std::vector<std::string>{"\t", "CALL ", " LIST", "LIST ", "TAMETO  S", std::string(300, 'x')})
            check(!B::Parse(junk), "junk is refused (and never read past its end: ASan)");
        check(B::Kin(7) == "KIN 7" && B::Kin(0) == "KIN 0", "KIN: the companion's family, 0 when none");
    }

    // ---- the beasts' part 2: the specials (src/beasts/EfBeastKinLogic.h; hunter-beasts.md §5)
    {
        check(B::KindOf(1) == B::Kind::Packrunner && B::KindOf(6) == B::Kind::Shellback && B::KindOf(0) == B::Kind::None
                  && B::KindOf(7) == B::Kind::None, "the archetype ids");
        check(B::RuleOf(4) == B::Rule::OnCast && B::RuleOf(99) == B::Rule::None, "the rule ids");
        check(B::SpecialAllowed(B::Rule::None, false, true), "a special with no rule goes at anyone");
        check(!B::SpecialAllowed(B::Rule::OnCast, false, false) && B::SpecialAllowed(B::Rule::OnCast, true, false),
              "an interrupt waits for a cast");
        check(!B::SpecialAllowed(B::Rule::NoBoss, false, true) && B::SpecialAllowed(B::Rule::NoBoss, false, false),
              "a hold never at a boss");
        check(B::SpecialAllowed(B::Rule::Busy, false, true), "a bonus rule never holds a special back");
        check(B::SpecialBonus(B::Rule::Busy, 50, true, false, false) > 1.49f && B::SpecialBonus(B::Rule::Busy, 50, false, true, true) == 1.0f,
              "busy: more only while the enemy fights someone else");
        check(B::SpecialBonus(B::Rule::Slowed, 40, false, true, false) > 1.39f && B::SpecialBonus(B::Rule::Slowed, 40, true, false, true) == 1.0f,
              "slowed: more only on a slowed enemy");
        check(B::SpecialBonus(B::Rule::Water, 67, false, false, true) > 1.6f && B::SpecialBonus(B::Rule::Water, 67, true, true, false) == 1.0f,
              "water: more only in water");
        check(B::SpecialBonus(B::Rule::OnCast, 50, true, true, true) == 1.0f, "a holding rule adds nothing");
        check(B::ArmourReduction(0, 60) == 0.0f && B::ArmourReduction(1e9f, 60) == 0.75f, "the armour's reduction, capped");
        float const half = B::PierceFactor(3000, 60, 50);
        check(half > 1.0f && half < 1.0f / (1.0f - B::ArmourReduction(3000, 60)), "through half the armour: more, less than none");
        check(B::PierceFactor(3000, 60, 100) * (1.0f - B::ArmourReduction(3000, 60)) > 0.999f, "through all of it: as if none");
        check(B::PierceFactor(0, 60, 50) == 1.0f, "no armour, nothing to go through");
        check(B::PierceFactor(4000, 20, 50) > B::PierceFactor(1000, 20, 50), "more armour, more to go through");
        std::vector<uint32_t> const all = {10, 20, 30, 20};
        auto knows = [](uint32_t s) { return s == 10 || s == 20; };
        check((B::SpecialsToForget(all, 20, knows) == std::vector<uint32_t>{10}), "another family's special goes, its own stays");
        check(B::SpecialsToForget(all, 10, [](uint32_t) { return false; }).empty(), "nothing known, nothing to forget");
    }

    // ---- the beasts' part 2: the passives (hunter-beasts.md §4)
    {
        B::HunterHit h;
        check(B::HunterDamageFactor(h) == 1.0f, "no archetype, no change");
        h.kind = B::Kind::Packrunner;
        h.v1 = 3;
        check(B::HunterDamageFactor(h) == 1.0f, "Packrunner: nothing off the companion's target");
        h.onCompanionTarget = true;
        check(B::HunterDamageFactor(h) > 1.0f, "Packrunner: more on the companion's target");
        h = {};
        h.kind = B::Kind::Prowler;
        h.v1 = 30;
        h.victimUnhurt = true;
        check(B::HunterDamageFactor(h) > 1.29f, "Prowler: the first blow on an unhurt enemy");
        h.direct = false;
        check(B::HunterDamageFactor(h) == 1.0f, "Prowler: a tick is no first blow");
        h.direct = true;
        h.victimUnhurt = false;
        check(B::HunterDamageFactor(h) == 1.0f, "Prowler: a hurt enemy, nothing");
        h = {};
        h.kind = B::Kind::Venomkin;
        h.v1 = 1;
        float const one = (h.venomStacks = 1, B::HunterDamageFactor(h));
        float const three = (h.venomStacks = 3, B::HunterDamageFactor(h));
        check(three > one && one > 1.0f, "Venomkin: each stack of venom more");
        h.venomStacks = 0;
        check(B::HunterDamageFactor(h) == 1.0f, "Venomkin: no venom, nothing");
        check(B::HunterTakenFactor(B::Kind::Heavyhide, 6, 10, 5.0f) < 1.0f, "Heavyhide: less taken beside the companion");
        check(B::HunterTakenFactor(B::Kind::Heavyhide, 6, 10, 11.0f) == 1.0f, "Heavyhide: nothing out of its range");
        check(B::HunterTakenFactor(B::Kind::Packrunner, 6, 10, 5.0f) == 1.0f, "only Heavyhide guards");
        check(B::HunterTakenFactor(B::Kind::Heavyhide, 200, 10, 1.0f) == 0.0f, "never below nothing");
        check(B::CompanionCritBonus(B::Kind::Prowler, 10, 80, 90.0f) > 0 && B::CompanionCritBonus(B::Kind::Prowler, 10, 80, 50.0f) == 0,
              "Prowler: the companion's crits early in a fight only");
        check(B::CompanionCritBonus(B::Kind::Skywing, 10, 80, 90.0f) == 0, "only the Prowler's");
        check(B::PaceHolds(B::Kind::Packrunner, 20, 19.0f) && !B::PaceHolds(B::Kind::Packrunner, 20, 21.0f)
                  && !B::PaceHolds(B::Kind::Prowler, 20, 1.0f), "Packrunner: the pace within range");
        check(B::WardFires(40, 30, 35, 100000, 0, B::kWardCooldownMs), "Shellback: the ward when the hunter falls under");
        check(!B::WardFires(30, 20, 35, 100000, 0, B::kWardCooldownMs), "not again while he stays under");
        check(!B::WardFires(40, 36, 35, 100000, 0, B::kWardCooldownMs), "not above the line");
        check(!B::WardFires(40, 30, 35, 100000, 100000 - 1000, B::kWardCooldownMs) &&
                  B::WardFires(40, 30, 35, 100000 + B::kWardCooldownMs, 100000, B::kWardCooldownMs), "once a cooldown");
        check(B::WardAbsorb(1000, 12) == 120 && B::WardAbsorb(1000, -5) == 0, "the ward's share of the health");
    }

    std::cout << "beasts_tests: " << g_checks << " checks, " << g_failed << " failed\n";
    return g_failed ? 1 : 0;
}
