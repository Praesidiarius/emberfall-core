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

// The Class Journeys' rules that need no core (Emberfall docs/design/class-journeys/README.md §2.2-§2.5, hunter.md §3):
// which steps a character is owed (a bot: every step of its level; a character from before the journey: the steps'
// `existing` rules, once), what a beast's rank needs (the Hunter's gates), which credits a tame gives, whether a cast
// counts for a "use it there" objective, and the cycle of the Hunter's stable. emberfall_journeys.cpp and
// emberfall_journeys_hunter.cpp drive them; tests/journeys_tests.cpp checks them.

#ifndef EMBERFALL_JOURNEY_LOGIC_H
#define EMBERFALL_JOURNEY_LOGIC_H

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace Emberfall::Journeys
{
    // ef_journey_step.existing
    enum Existing : std::uint8_t { kExistingNone = 0, kExistingLevel = 1, kExistingTamed = 2 };
    // ef_journey_step.gate: what the Hunter's taming opens with the step's mark
    enum Gate : std::uint8_t { kGateNone = 0, kGateElite = 1, kGateRare = 2, kGateRareElite = 3 };
    // ef_journey_tame_credit.ranks bits
    enum RankBit : std::uint8_t { kRankNormal = 1, kRankElite = 2, kRankRare = 4, kRankRareElite = 8 };

    // SMSG_PET_TAME_FAILURE's reason for "not ready yet": this + the step's level (the client's text names the level).
    constexpr std::uint8_t kTameFailureJourney = 100;

    struct Step
    {
        std::uint8_t cls = 0;
        std::uint8_t step = 0;
        std::uint8_t level = 0;
        std::uint32_t spell = 0;      // the mark
        std::uint32_t spell2 = 0;
        std::uint8_t gate = kGateNone;
        std::uint8_t existing = kExistingNone;
    };

    // What decides the steps a character is owed without its quests.
    struct Who
    {
        std::uint8_t cls = 0;
        std::uint32_t level = 0;
        bool bot = false;          // a playerbot: every step at or below its level (README §2.5)
        bool migrating = false;    // the first login since the journey came (ef_character_migration)
        bool tamed = false;        // a hunter with Beastcraft or a companion (hunter.md §3)
    };

    inline void AddSpells(std::vector<std::uint32_t>& out, Step const& s)
    {
        for (std::uint32_t spell : {s.spell, s.spell2})
            if (spell && std::find(out.begin(), out.end(), spell) == out.end())
                out.push_back(spell);
    }

    // A step is owed when the character may have it without the quests: a bot at its level, or the once-only rule of
    // the characters from before the journey.
    inline bool Owed(Step const& s, Who const& w)
    {
        if (s.cls != w.cls)
            return false;
        if (w.bot)
            return w.level >= s.level;
        if (!w.migrating)
            return false;
        switch (s.existing)
        {
            case kExistingLevel: return w.level >= s.level;
            case kExistingTamed: return w.tamed;
            default: return false;
        }
    }

    // The spells to teach (marks and second spells), in the steps' order.
    inline std::vector<std::uint32_t> SpellsOwed(std::vector<Step> const& steps, Who const& w)
    {
        std::vector<std::uint32_t> out;
        for (Step const& s : steps)
            if (Owed(s, w))
                AddSpells(out, s);
        return out;
    }

    // creature_template.rank (0 normal, 1 elite, 2 rare elite, 3 boss, 4 rare) -> the gate it needs.
    inline std::uint8_t GateOfRank(std::uint32_t rank)
    {
        switch (rank)
        {
            case 1: case 3: return kGateElite;
            case 2: return kGateRareElite;
            case 4: return kGateRare;
            default: return kGateNone;
        }
    }

    inline std::uint8_t RankBitOf(std::uint32_t rank)
    {
        switch (rank)
        {
            case 1: case 3: return kRankElite;
            case 2: return kRankRareElite;
            case 4: return kRankRare;
            default: return kRankNormal;
        }
    }

    // The step whose mark opens `gate` for the class (nullptr: no step gates it, so it is open).
    inline Step const* GateStep(std::vector<Step> const& steps, std::uint8_t cls, std::uint8_t gate)
    {
        if (gate == kGateNone)
            return nullptr;
        for (Step const& s : steps)
            if (s.cls == cls && s.gate == gate)
                return &s;
        return nullptr;
    }

    struct TameCredit
    {
        std::uint32_t creature = 0;
        std::uint8_t ranks = 0;
        std::uint32_t minLevel = 0;
    };

    // The credits a tame of a beast of this rank and level gives, each once.
    inline std::vector<std::uint32_t> TameCredits(std::vector<TameCredit> const& credits, std::uint32_t rank, std::uint32_t level)
    {
        std::vector<std::uint32_t> out;
        std::uint8_t const bit = RankBitOf(rank);
        for (TameCredit const& c : credits)
            if ((c.ranks & bit) && level >= c.minLevel && std::find(out.begin(), out.end(), c.creature) == out.end())
                out.push_back(c.creature);
        return out;
    }

    struct Cast
    {
        std::uint32_t quest = 0;
        std::uint32_t spell = 0;
        std::uint32_t area = 0;    // AreaTable: the player's zone or area
        std::uint32_t credit = 0;
    };

    inline bool CastCounts(Cast const& c, std::uint32_t spell, std::uint32_t zone, std::uint32_t area)
    {
        return c.spell == spell && (c.area == zone || c.area == area);
    }

    // The Hunter's stable (hunter.md §3): the stalls in use, in order; Call from the Stable and Beast at Heel take the
    // next one after the last they took, round the stable. -1: the stable is empty.
    inline int NextStall(std::vector<bool> const& used, int last)
    {
        int const n = int(used.size());
        for (int k = 1; k <= n; ++k)
        {
            int const i = ((last < 0 ? -1 : last) + k) % n;
            if (used[i])
                return i;
        }
        return -1;
    }
}

#endif
