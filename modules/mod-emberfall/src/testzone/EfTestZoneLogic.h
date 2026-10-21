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

// `.eftest zone` without the core (src/emberfall_testzone.cpp; tests/testzone_tests.cpp): the levelling route and what
// a character arriving at a zone has. The data are Emberfall's generated world tables (Emberfall
// tools/py/emberfall_content/zone_route.py):
//
//   ef_test_zone        zone (key), name, sort (route order), area, level_min, level_max, races (race mask of a start
//                       zone; 0: shared), map, x, y, z, o (the main hub, a start zone's start), hub (its name)
//   ef_test_zone_quest  quest, zone, sort, leads_to (the zone a breadcrumb leads into), role (0 done, 1 breadcrumb into
//                       `leads_to`, 2 after such a breadcrumb in its chain)
//   ef_test_zone_taxi   zone, node (TaxiNodes)
//   ef_test_gear        class, slot (EQUIPMENT_SLOT_*), item, required_level

#ifndef EMBERFALL_TEST_ZONE_LOGIC_H
#define EMBERFALL_TEST_ZONE_LOGIC_H

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace Emberfall::TestZone
{
    enum Role : uint8_t { RoleDone = 0, RoleBreadcrumb = 1, RoleAfter = 2 };

    struct ZoneRow
    {
        std::string key;
        std::string name;
        uint32_t sort = 0;
        uint32_t area = 0;
        uint8_t levelMin = 1;
        uint8_t levelMax = 1;
        uint32_t races = 0;
        uint32_t map = 0;
        float x = 0, y = 0, z = 0, o = 0;
        std::string hub;
    };

    struct QuestRow
    {
        uint32_t quest = 0;
        std::string zone;
        uint32_t sort = 0;
        std::string into;
        uint8_t role = RoleDone;
    };

    struct TaxiRow
    {
        std::string zone;
        uint32_t node = 0;
    };

    struct GearRow
    {
        uint8_t cls = 0;
        uint8_t slot = 0;
        uint32_t item = 0;
        uint8_t requiredLevel = 0;
    };

    inline std::string Lower(std::string_view s)
    {
        std::string out(s);
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return out;
    }

    // Zones in route order (sort).
    inline std::vector<ZoneRow> Sorted(std::vector<ZoneRow> zones)
    {
        std::stable_sort(zones.begin(), zones.end(), [](ZoneRow const& a, ZoneRow const& b) { return a.sort < b.sort; });
        return zones;
    }

    // The zone a player means: the key exactly, else the one zone whose key or name starts with the text ("hightide"
    // -> hightide-bastion). nullptr when none or several match; `ambiguous` then says which.
    inline ZoneRow const* Find(std::vector<ZoneRow> const& zones, std::string_view text, std::string* ambiguous = nullptr)
    {
        std::string const want = Lower(text);
        if (want.empty())
            return nullptr;
        for (ZoneRow const& z : zones)
            if (Lower(z.key) == want)
                return &z;
        ZoneRow const* found = nullptr;
        uint32_t count = 0;
        std::string names;
        for (ZoneRow const& z : zones)
        {
            std::string const key = Lower(z.key), name = Lower(z.name);
            if (key.rfind(want, 0) == 0 || name.rfind(want, 0) == 0)
            {
                names += (names.empty() ? "" : ", ") + z.key;
                found = &z;
                ++count;
            }
        }
        if (count > 1)
        {
            if (ambiguous)
                *ambiguous = names;
            return nullptr;
        }
        return found;
    }

    inline uint32_t RaceBit(uint32_t race) { return race ? 1u << (race - 1) : 0; }

    // The zones before `target` on the route of a character of `race` (route order): its start zone(s), then the shared
    // zones before the target. None when the target is itself a start zone.
    inline std::vector<ZoneRow> Before(std::vector<ZoneRow> const& zones, uint32_t race, std::string_view target)
    {
        std::vector<ZoneRow> out;
        uint32_t const bit = RaceBit(race);
        std::vector<ZoneRow> const route = Sorted(zones);
        for (ZoneRow const& z : route)
        {
            if (z.key == target)
                return z.races ? std::vector<ZoneRow>{} : out;
            if (!z.races || (z.races & bit))
                out.push_back(z);
        }
        return {};
    }

    struct QuestPlan
    {
        std::vector<uint32_t> done;          // to complete, in route order
        std::vector<uint32_t> breadcrumbs;   // into the target: to the log
        std::vector<uint32_t> waiting;       // after a breadcrumb into the target: left alone
    };

    // What happens to the quests of the zones before the target.
    inline QuestPlan PlanQuests(std::vector<ZoneRow> const& before, std::vector<QuestRow> quests, std::string_view target)
    {
        std::map<std::string, uint32_t> order;
        for (ZoneRow const& z : before)
            order[z.key] = z.sort;
        std::stable_sort(quests.begin(), quests.end(), [&](QuestRow const& a, QuestRow const& b)
        {
            auto const ia = order.find(a.zone), ib = order.find(b.zone);
            uint32_t const za = ia == order.end() ? 0 : ia->second, zb = ib == order.end() ? 0 : ib->second;
            return za != zb ? za < zb : a.sort < b.sort;
        });
        QuestPlan plan;
        for (QuestRow const& q : quests)
        {
            if (!order.count(q.zone))
                continue;
            if (q.into == target && q.role == RoleBreadcrumb)
                plan.breadcrumbs.push_back(q.quest);
            else if (q.into == target && q.role == RoleAfter)
                plan.waiting.push_back(q.quest);
            else
                plan.done.push_back(q.quest);
        }
        return plan;
    }

    // The flight nodes of the zones before the target and of the target.
    inline std::vector<uint32_t> Nodes(std::vector<ZoneRow> const& before, std::string_view target,
                                       std::vector<TaxiRow> const& taxi)
    {
        std::vector<uint32_t> out;
        for (TaxiRow const& t : taxi)
        {
            bool wanted = t.zone == target;
            for (ZoneRow const& z : before)
                wanted = wanted || z.key == t.zone;
            if (wanted && std::find(out.begin(), out.end(), t.node) == out.end())
                out.push_back(t.node);
        }
        return out;
    }

    // Per equipment slot the class's item with the highest required level at or below `level` (the lower id on a tie).
    inline std::map<uint8_t, uint32_t> PickGear(std::vector<GearRow> const& rows, uint8_t cls, uint8_t level)
    {
        std::map<uint8_t, GearRow> best;
        for (GearRow const& r : rows)
        {
            if (r.cls != cls || r.requiredLevel > level)
                continue;
            auto const it = best.find(r.slot);
            if (it == best.end() || r.requiredLevel > it->second.requiredLevel ||
                (r.requiredLevel == it->second.requiredLevel && r.item < it->second.item))
                best[r.slot] = r;
        }
        std::map<uint8_t, uint32_t> out;
        for (auto const& [slot, r] : best)
            out[slot] = r.item;
        return out;
    }

    // A modest purse for the level, in copper: level squared silver (1 g at 10, 4 g at 20, 25 g at 50).
    inline uint32_t Purse(uint8_t level) { return uint32_t(level) * level * 100; }
}

#endif
