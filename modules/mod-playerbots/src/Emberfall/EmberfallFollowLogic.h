/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version. Emberfall's part of the module (Copyright Emberfall / Tales of Orrin).
 */

/*
 * The bots follow a player (Emberfall docs/design/playerbots-pilot.md §14): the rule as pure logic, no core types, so
 * mod-emberfall's standalone tests (tests/botfollow_tests.cpp) run it without a server. EmberfallFollow.cpp feeds it
 * the online bots and carries out its plan in the world.
 *
 *   PickFollowed   who is followed: the online character of the first listed account that has one
 *   WindowFor      the level window round the followed character, clamped to 1..max
 *   MakePlan       which bots get a new level or only a new place, so that `share` percent of each side's bots end
 *                  within the window and in a zone of their side whose band holds their level; the rest stays spread
 *   PickZone       the fitting zone with the fewest bots (under the cap) for one bot
 */

#ifndef PLAYERBOTS_EMBERFALLFOLLOWLOGIC_H
#define PLAYERBOTS_EMBERFALLFOLLOWLOGIC_H

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace EmberfallFollow::Logic
{
    // a side as a bit set: the Accord 1, the Kindred 2, a shared zone both
    constexpr uint8_t SIDE_ACCORD = 1;
    constexpr uint8_t SIDE_KINDRED = 2;
    constexpr uint8_t SIDE_BOTH = SIDE_ACCORD | SIDE_KINDRED;

    // "accord", "kindred", "both" (case as written by the game repo's botpilot conf); anything else: both
    inline uint8_t ParseSide(std::string const& text)
    {
        if (text == "accord" || text == "1")
            return SIDE_ACCORD;
        if (text == "kindred" || text == "2")
            return SIDE_KINDRED;
        return SIDE_BOTH;
    }

    struct Zone
    {
        uint32_t id = 0;
        uint8_t lo = 0, hi = 0;   // the zone's band (Playerbots.ZoneBracket.<zone>), both ends included
        uint8_t sides = SIDE_BOTH;
    };

    inline bool ZoneFits(Zone const& z, uint8_t level, uint8_t side) { return level >= z.lo && level <= z.hi && (z.sides & side); }

    struct Window
    {
        uint8_t lo = 1, hi = 1;
        bool Contains(uint8_t level) const { return level >= lo && level <= hi; }
        // levels outside the window to its nearest end (0 inside)
        uint8_t Distance(uint8_t level) const { return level < lo ? lo - level : level > hi ? level - hi : 0; }
        uint32_t Size() const { return hi - lo + 1u; }
    };

    inline Window WindowFor(uint8_t level, uint8_t levels, uint8_t maxLevel)
    {
        if (maxLevel < 1)
            maxLevel = 1;
        level = std::clamp<uint8_t>(level, 1, maxLevel);
        Window w;
        w.lo = uint8_t(std::max(1, int(level) - int(levels)));
        w.hi = uint8_t(std::min(int(maxLevel), int(level) + int(levels)));
        return w;
    }

    // An online candidate for being followed: its account and character.
    struct Online
    {
        uint32_t account = 0;
        uint32_t guid = 0;
        uint8_t level = 0;
    };

    // The followed character: the first account of `accounts` (in the configured order) with a character online.
    // Returns its index in `online`, or -1 when none of them is online.
    inline int PickFollowed(std::vector<uint32_t> const& accounts, std::vector<Online> const& online)
    {
        for (uint32_t account : accounts)
        {
            if (!account)
                continue;
            for (size_t i = 0; i < online.size(); ++i)
                if (online[i].account == account)
                    return int(i);
        }
        return -1;
    }

    // What keeps a bot out of a pass (taken in a later one).
    struct Exemptions
    {
        bool grouped = false;   // in a group (bots never group with each other here: a player's group)
        bool combat = false;
        bool dead = false;
        bool seen = false;      // a real player within sight
        bool dungeon = false;   // an instance: dungeon, raid, battleground
        bool vehicle = false;
        bool travelling = false; // on a flight path or in a teleport
        bool Any() const { return grouped || combat || dead || seen || dungeon || vehicle || travelling; }
    };

    struct Bot
    {
        uint32_t guid = 0;
        uint8_t side = SIDE_ACCORD;  // SIDE_ACCORD or SIDE_KINDRED
        uint8_t level = 1;
        uint32_t zone = 0;
        bool exempt = false;
    };

    enum class Move : uint8_t
    {
        Place = 0,   // in the window, in a zone that does not fit: a new place only
        Relevel = 1  // outside the window: a new level in the window, then a place in a zone of that band
    };

    struct Order
    {
        uint32_t guid = 0;
        Move kind = Move::Place;
        uint8_t level = 0;      // the level the bot has after the move
        uint8_t side = SIDE_ACCORD;
        uint8_t distance = 0;   // its levels from the window before the move (re-levels: furthest first)
    };

    struct SideCount
    {
        uint32_t bots = 0;      // online bots of the side
        uint32_t target = 0;    // the share of them that should end in the window
        uint32_t fit = 0;       // in the window and in a fitting zone already (left alone)
        uint32_t misplaced = 0; // in the window, in a zone that does not fit
        uint32_t outside = 0;   // outside the window
        uint32_t exempt = 0;    // exempt and not fitting (taken next pass)
        uint32_t reserve = 0;   // outside the window and kept there (the spread rest)
        uint32_t orders = 0;    // places and re-levels planned
    };

    struct Plan
    {
        Window window;
        std::vector<Order> orders; // in the order the pass carries them out
        SideCount accord, kindred;
        SideCount& Of(uint8_t side) { return side == SIDE_KINDRED ? kindred : accord; }
    };

    inline bool InFittingZone(Bot const& b, std::vector<Zone> const& zones)
    {
        for (Zone const& z : zones)
            if (z.id == b.zone)
                return ZoneFits(z, b.level, b.side);
        return false;
    }

    // The bots kept outside the window, spread over the levels outside it as the realm spreads them: one slot a
    // kept bot, the slots evenly over those levels, each filled by the nearest bot not taken yet (ties: the lower
    // level, then the lower guid). Returns the guids kept.
    inline std::set<uint32_t> SpreadReserve(std::vector<Bot const*> const& outside, uint32_t slots, Window const& w,
                                            uint8_t maxLevel)
    {
        std::set<uint32_t> kept;
        if (!slots || outside.empty())
            return kept;
        std::vector<uint8_t> levels;
        for (int l = 1; l <= int(maxLevel); ++l)
            if (!w.Contains(uint8_t(l)))
                levels.push_back(uint8_t(l));
        if (levels.empty())
            return kept;
        std::vector<Bot const*> pool = outside;
        std::sort(pool.begin(), pool.end(), [](Bot const* a, Bot const* b)
                  { return a->level != b->level ? a->level < b->level : a->guid < b->guid; });
        for (uint32_t i = 0; i < slots && !pool.empty(); ++i)
        {
            size_t const at = std::min(levels.size() - 1, size_t((i + 0.5) * levels.size() / slots));
            int const want = levels[at];
            size_t best = 0;
            for (size_t j = 1; j < pool.size(); ++j)
                if (std::abs(int(pool[j]->level) - want) < std::abs(int(pool[best]->level) - want))
                    best = j;
            kept.insert(pool[best]->guid);
            pool.erase(pool.begin() + best);
        }
        return kept;
    }

    // The pass's plan. `sharePct` of each side's online bots (rounded) should be in `w` and in a fitting zone; those
    // already there are left alone. In-window bots in a wrong zone get a place only (first: the cheapest move); then
    // bots outside the window get a level in it, the furthest from it first, each the window level its side has the
    // fewest bots at (ties: nearest the followed level `centre`, then the lower). The bots kept outside (the
    // reserve, 100 - sharePct) stay spread over the other levels (SpreadReserve); an exempt bot outside the window
    // is always kept (it is taken next pass). Exempt in-window bots in a wrong zone shrink the reserve by as many.
    inline Plan MakePlan(std::vector<Bot> const& bots, std::vector<Zone> const& zones, Window const& w, uint8_t centre,
                         uint32_t sharePct, uint8_t maxLevel)
    {
        Plan plan;
        plan.window = w;
        sharePct = std::min<uint32_t>(sharePct, 100);
        std::vector<Order> relevels;
        for (uint8_t side : {SIDE_ACCORD, SIDE_KINDRED})
        {
            SideCount& c = plan.Of(side);
            std::vector<Bot const*> misplaced, outside, outsideFree;
            std::map<uint8_t, uint32_t> atLevel; // window level -> bots of the side there (after the plan)
            uint32_t exemptMisplaced = 0, exemptOutside = 0;
            for (Bot const& b : bots)
            {
                if (b.side != side)
                    continue;
                ++c.bots;
                if (w.Contains(b.level))
                {
                    ++atLevel[b.level];
                    if (InFittingZone(b, zones))
                        ++c.fit;
                    else
                    {
                        ++c.misplaced;
                        if (b.exempt)
                            ++exemptMisplaced;
                        else
                            misplaced.push_back(&b);
                    }
                }
                else
                {
                    ++c.outside;
                    outside.push_back(&b);
                    if (b.exempt)
                        ++exemptOutside;
                    else
                        outsideFree.push_back(&b);
                }
            }
            c.exempt = exemptMisplaced + exemptOutside;
            c.target = (c.bots * sharePct + 50) / 100;
            if (c.fit >= c.target)
            {
                c.reserve = c.outside;
                continue;
            }
            uint32_t need = c.target - c.fit;
            // places first (sorted by guid: a stable plan)
            std::sort(misplaced.begin(), misplaced.end(), [](Bot const* a, Bot const* b) { return a->guid < b->guid; });
            for (Bot const* b : misplaced)
            {
                if (!need)
                    break;
                plan.orders.push_back({b->guid, Move::Place, b->level, side, 0});
                ++c.orders;
                --need;
            }
            // the reserve: the bots of the side not meant for the window, less the exempt in-window bots the plan
            // cannot place now; the exempt bots outside the window always stay
            uint32_t const keep = c.bots - c.target;
            uint32_t const slots = keep > exemptMisplaced ? keep - exemptMisplaced : 0;
            uint32_t const freeSlots = slots > exemptOutside ? slots - exemptOutside : 0;
            std::set<uint32_t> const kept = SpreadReserve(outsideFree, freeSlots, w, maxLevel);
            c.reserve = exemptOutside + uint32_t(kept.size());
            std::vector<Bot const*> movers;
            for (Bot const* b : outsideFree)
                if (!kept.count(b->guid))
                    movers.push_back(b);
            std::sort(movers.begin(), movers.end(), [&w](Bot const* a, Bot const* b)
                      {
                          uint8_t const da = w.Distance(a->level), db = w.Distance(b->level);
                          return da != db ? da > db : a->guid < b->guid;
                      });
            for (Bot const* b : movers)
            {
                if (!need)
                    break;
                uint8_t best = w.lo;
                for (int l = w.lo; l <= w.hi; ++l)
                {
                    uint8_t const lv = uint8_t(l);
                    uint32_t const n = atLevel[lv], nb = atLevel[best];
                    int const dl = std::abs(int(lv) - int(centre)), db = std::abs(int(best) - int(centre));
                    if (n < nb || (n == nb && dl < db))
                        best = lv;
                }
                ++atLevel[best];
                relevels.push_back({b->guid, Move::Relevel, best, side, w.Distance(b->level)});
                ++c.orders;
                --need;
            }
        }
        // the re-levels of both sides, the furthest from the window first
        std::stable_sort(relevels.begin(), relevels.end(), [](Order const& a, Order const& b) { return a.distance > b.distance; });
        plan.orders.insert(plan.orders.end(), relevels.begin(), relevels.end());
        return plan;
    }

    // The zone for a bot of `side` at `level`: of the zones whose band holds the level and that are its side's, the one
    // with the fewest bots now (`counts`: zone -> bots), skipping those at `cap` or more (0: no cap) and those in
    // `excluded` (no place found there); ties: the first in `zones`. 0: none.
    inline uint32_t PickZone(std::vector<Zone> const& zones, std::map<uint32_t, uint32_t> const& counts, uint8_t level,
                             uint8_t side, uint32_t cap, std::set<uint32_t> const& excluded)
    {
        uint32_t best = 0, bestCount = 0;
        for (Zone const& z : zones)
        {
            if (!ZoneFits(z, level, side) || excluded.count(z.id))
                continue;
            auto it = counts.find(z.id);
            uint32_t const n = it == counts.end() ? 0 : it->second;
            if (cap && n >= cap)
                continue;
            if (!best || n < bestCount)
            {
                best = z.id;
                bestCount = n;
            }
        }
        return best;
    }

    // How many orders a pass carries out by now: one every 1000 / perSecond ms (perSecond 0: 1).
    inline uint32_t Due(uint64_t elapsedMs, uint32_t perSecond)
    {
        perSecond = std::max<uint32_t>(1, perSecond);
        return uint32_t(elapsedMs * perSecond / 1000);
    }
}

#endif
