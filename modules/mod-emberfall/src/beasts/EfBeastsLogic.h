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

#ifndef EMBERFALL_BEASTS_LOGIC_H
#define EMBERFALL_BEASTS_LOGIC_H

// The Hunter's beasts, part 1 (Emberfall docs/design/hunter-beasts.md §1-§3), pure rules, no core types
// (tests/beasts_tests.cpp):
//   - the stable: every hunter's stalls open (no buying), never fewer than a row already uses;
//   - the tame choice ("When tamed": with me / to the stable) and what happens at a tame;
//   - the companion's spells by priority (the world's ef_pet_priority rows): which spells the AI tries, in which order;
//   - which pet bar packets are a hand cast the server refuses;
//   - the EFB protocol (addon whispers, prefix "EFB\t"):
//       c -> s  HELLO                 after world entry: the server answers PREF
//       c -> s  TAMETO W|S            the taming frame's choice (W: take it with me, S: send it to the stable); PREF back
//       s -> c  PREF W|S              the remembered choice
//       c -> s  LIST                  the field list: the server sends the stock stable list (MSG_LIST_STABLED_PETS)
//                                     with the hunter's own guid as the keeper (the window's field mode)
//       c -> s  CALL <pet number>     the beast Call from the Stable brings out: the server casts it with that pick;
//                                     CALLED <pet number> back (0: refused)
//       s -> c  TAUNTS ...            after HELLO (Taunts below)

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Emberfall::Beasts
{
    // ---- the stable -------------------------------------------------------------------------------------------------

    // The stalls a hunter has after login: all of them (the core's MAX_PET_STABLES), never fewer than he had.
    constexpr std::uint32_t OpenStalls(std::uint32_t had, std::uint32_t max) { return std::max(had, max); }

    // The first free stall (an index into PetStable::StabledPets) of `open` stalls; -1: none.
    template <class Used>
    int FirstFree(Used const& used, std::uint32_t open)
    {
        for (std::uint32_t i = 0; i < open && i < used.size(); ++i)
            if (!used[i])
                return int(i);
        return -1;
    }

    // ---- the tame choice --------------------------------------------------------------------------------------------

    enum class TameTo : std::uint8_t { WithMe = 0, Stable = 1 };

    // What the module does around the tame (the core's TAME_CREATURE always brings the new beast out).
    struct TamePlan
    {
        bool putAwayFirst = false;  // the companion out (or waiting) into a free stall before the tame
        bool swapBack = false;      // after: the new beast into that stall, the old one out again
        bool stableNew = false;     // after: the new beast into a free stall (nothing was out)
    };

    constexpr TamePlan PlanTame(TameTo to, bool companionKept)
    {
        TamePlan p;
        p.putAwayFirst = companionKept;
        p.swapBack = to == TameTo::Stable && companionKept;
        p.stableNew = to == TameTo::Stable && !companionKept;
        return p;
    }

    constexpr char TameToChar(TameTo t) { return t == TameTo::Stable ? 'S' : 'W'; }

    // ---- the companion's priority -----------------------------------------------------------------------------------

    struct PriorityRow
    {
        std::uint32_t spell = 0;
        std::uint32_t reserve = 0;  // power that must stay after the cast (0: none)
        bool taunt = false;         // cast only while the player keeps it toggled on
    };

    // What the pet knows of a row's spell right now.
    struct SpellState
    {
        bool known = false;         // in its spell book (not removed)
        bool toggledOn = false;     // its autocast toggle
        std::uint32_t cost = 0;     // its power cost
    };

    // The spells the AI tries this tick, best first: the rows in order, the known ones; a taunt only while toggled on; a
    // row with a reserve only when the pet's power covers its cost and the reserve. Everything else (cooldowns, range,
    // a target, the power for the cost itself) is the core's CanAutoCast.
    template <class StateOf>
    void Order(std::vector<PriorityRow> const& rows, std::uint32_t power, StateOf&& stateOf, std::vector<std::uint32_t>& out)
    {
        out.clear();
        for (PriorityRow const& r : rows)
        {
            SpellState const s = stateOf(r.spell);
            if (!s.known || (r.taunt && !s.toggledOn))
                continue;
            if (r.reserve && power < s.cost + r.reserve)
                continue;
            out.push_back(r.spell);
        }
    }

    // ---- the pet bar packets ----------------------------------------------------------------------------------------

    // The action state of a pet action's packed data (CMSG_PET_ACTION, the high byte: ActiveStates).
    constexpr std::uint8_t ActState(std::uint32_t data) { return std::uint8_t(data >> 24); }
    constexpr std::uint32_t ActSpell(std::uint32_t data) { return data & 0x00FFFFFFu; }

    // A spell button (ACT_PASSIVE 0x01, ACT_DISABLED 0x81, ACT_ENABLED 0xC1): the click casts it.
    constexpr bool IsSpellAct(std::uint8_t state) { return state == 0x01 || state == 0x81 || state == 0xC1; }

    // Whether the server refuses a hand cast of a companion's spell: on own data, at a hunter's companion. Commands
    // (attack, follow, stay) and reactions stay the player's.
    constexpr bool RefuseHandCast(bool ownData, bool hunterPet, bool isSpell) { return ownData && hunterPet && isSpell; }

    // ---- EFB ----------------------------------------------------------------------------------------------------------

    constexpr std::string_view kPrefix = "EFB\t";

    struct Request
    {
        enum class Kind { Hello, TameTo, List, Call } kind = Kind::Hello;
        TameTo to = TameTo::WithMe;
        std::uint32_t pet = 0;
    };

    inline std::optional<Request> Parse(std::string_view body)
    {
        // words split by single spaces, nothing around them
        if (body.empty() || body.size() > 64 || body.front() == ' ' || body.back() == ' ' || body.find("  ") != std::string_view::npos)
            return std::nullopt;
        auto word = [&]() {
            std::size_t const sp = body.find(' ');
            std::string_view w = body.substr(0, sp);
            body = sp == std::string_view::npos ? std::string_view{} : body.substr(sp + 1);
            return w;
        };
        std::string_view const verb = word();
        Request r;
        if (verb == "HELLO")
            r.kind = Request::Kind::Hello;
        else if (verb == "LIST")
            r.kind = Request::Kind::List;
        else if (verb == "TAMETO")
        {
            std::string_view const a = word();
            if (a != "W" && a != "S")
                return std::nullopt;
            r.kind = Request::Kind::TameTo;
            r.to = a == "S" ? TameTo::Stable : TameTo::WithMe;
        }
        else if (verb == "CALL")
        {
            std::string_view const a = word();
            if (a.empty() || a.size() > 10)
                return std::nullopt;
            std::uint64_t n = 0;
            for (char c : a)
            {
                if (c < '0' || c > '9')
                    return std::nullopt;
                n = n * 10 + std::uint64_t(c - '0');
            }
            if (!n || n > 0xFFFFFFFFull)
                return std::nullopt;
            r.kind = Request::Kind::Call;
            r.pet = std::uint32_t(n);
        }
        else
            return std::nullopt;
        if (!body.empty())
            return std::nullopt;
        return r;
    }

    inline std::string Pref(TameTo to) { return std::string("PREF ") + TameToChar(to); }
    inline std::string Called(std::uint32_t pet) { return "CALLED " + std::to_string(pet); }

    // s -> c  TAUNTS <spell>,<spell>,...   after HELLO: the companion spells whose toggle stays (the taunt on or off); the
    //                                      client's pet bar shows every other companion spell as automatic. "TAUNTS"
    //                                      alone: the server does not run the companions by priority (the stock bar)
    inline std::string Taunts(std::vector<std::uint32_t> const& spells, bool ordered)
    {
        if (!ordered)
            return "TAUNTS";
        std::string s = "TAUNTS ";
        for (std::size_t i = 0; i < spells.size(); ++i)
            s += (i ? "," : "") + std::to_string(spells[i]);
        if (spells.empty())
            s += "0";
        return s;
    }
}

#endif // EMBERFALL_BEASTS_LOGIC_H
