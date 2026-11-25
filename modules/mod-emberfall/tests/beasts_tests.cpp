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
    }

    std::cout << "beasts_tests: " << g_checks << " checks, " << g_failed << " failed\n";
    return g_failed ? 1 : 0;
}
