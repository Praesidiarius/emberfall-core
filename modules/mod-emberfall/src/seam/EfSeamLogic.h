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

#ifndef EMBERFALL_SEAM_LOGIC_H
#define EMBERFALL_SEAM_LOGIC_H

// The seam guard (Emberfall docs/design/kindred-map-split.md §3.4 item 4), without the core: the Kindred is two maps
// (1 north, 754 south), each drawing a one-tile margin of the other half whose terrain carries a "beyond the seam" area
// (ef_seam_area). What a player's tick does: remember where they stand on their own side, or send them back there (no
// spot on this map: home) when they stand in such an area. The script (emberfall_seam.cpp) feeds it the player's state;
// tests/seam_tests.cpp checks it standalone.

#include <cstdint>
#include <string>
#include <unordered_map>

namespace Emberfall::Seam
{
    // How often a player is looked at (ms); the core's own zone update runs once a second.
    constexpr uint32_t kCheckMs = 1000;
    // The first look after a login waits this long (the client is still loading the world).
    constexpr uint32_t kLoginGraceMs = 3000;

    // ef_seam_area: a "beyond the seam" area id and the map whose margin it is.
    using Areas = std::unordered_map<uint32_t, uint32_t>;

    // Is (map, zone or area) beyond the seam? A margin area may be a zone of its own or a subarea; both are asked.
    inline bool IsBeyond(Areas const& areas, uint32_t map, uint32_t zone, uint32_t area)
    {
        for (uint32_t id : { area, zone })
        {
            auto it = areas.find(id);
            if (it != areas.end() && it->second == map)
                return true;
        }
        return false;
    }

    struct Spot
    {
        uint32_t map = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f, o = 0.0f;
    };

    // The player as the guard sees them.
    struct Seen
    {
        uint32_t map = 0, zone = 0, area = 0;
        bool gm = false;          // GM mode on
        bool moved = false;       // on a flight path, a transport or in a vehicle: the path takes them, not their feet
        bool teleporting = false; // a teleport is under way (the spot is not theirs yet)
        bool falling = false;     // in the air: not a spot to put anyone back on
        Spot here;
    };

    // The last spot on the player's own side.
    struct Memory
    {
        bool has = false;
        Spot spot;
    };

    enum class Act : uint8_t
    {
        None,     // nothing to do
        Remember, // keep `here` as the last spot on their side
        SendBack, // beyond the seam: back to the remembered spot (same map)
        SendHome, // beyond the seam with no spot on this map: to the hearth
    };

    inline Act Decide(Areas const& areas, Seen const& p, Memory const& m)
    {
        if (p.gm || p.moved || p.teleporting)
            return Act::None;
        if (IsBeyond(areas, p.map, p.zone, p.area))
            return m.has && m.spot.map == p.map ? Act::SendBack : Act::SendHome;
        return p.falling ? Act::None : Act::Remember;
    }

    // One tick's bookkeeping: what Decide says, with the memory updated for Remember.
    inline Act Step(Areas const& areas, Seen const& p, Memory& m)
    {
        Act const act = Decide(areas, p, m);
        if (act == Act::Remember)
        {
            m.has = true;
            m.spot = p.here;
        }
        return act;
    }

    // The client's locale (the core's LocaleConstant numbers): enUS 0, frFR 2, deDE 3; everything else reads English.
    enum class Lang : uint8_t { En, Fr, De };
    inline Lang LangOf(uint32_t locale) { return locale == 3 ? Lang::De : locale == 2 ? Lang::Fr : Lang::En; }

    // What the player is told when the guard turns them back.
    inline std::string Message(Lang lang)
    {
        switch (lang)
        {
            case Lang::De: return "Hier endet das Land. Nimm ein Schiff.";
            case Lang::Fr: return "La terre s'arrête ici. Prenez un navire.";
            default: return "The ground ends here. Take a ship.";
        }
    }
}

#endif
