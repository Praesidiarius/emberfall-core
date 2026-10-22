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

// Emberfall: character XP from gathering and crafting (Emberfall docs/design/professions.md, the rank system; Sundered
// Vale's progression.md §5 and professions.md §4-5). Pure: the colour of an action from the skill and the action's
// thresholds, and the XP it is worth. No core types, so tests/professionxp_tests.cpp runs it without a server.

#ifndef EF_PROFESSION_XP_H
#define EF_PROFESSION_XP_H

#include <cmath>
#include <cstdint>

namespace Emberfall::Professions
{
    enum class Colour : uint8_t { Locked, Orange, Yellow, Green, Grey };

    struct XpRules
    {
        double base = 70.0;          // Emberfall.Professions.Xp.Base
        double perSkill = 0.06;      // Emberfall.Professions.Xp.PerSkill: per point of the action's requirement
        uint32_t greenPercent = 50;  // Emberfall.Professions.Xp.GreenPercent
    };

    // The colour of an action of requirement `required` (yellow/green/grey: the skill values the colours start at)
    // for a skill of `skill`. Grey also when the thresholds are missing (gray 0).
    inline Colour ColourOf(uint32_t skill, uint32_t required, uint32_t yellow, uint32_t green, uint32_t gray)
    {
        if (gray == 0 || skill >= gray)
            return Colour::Grey;
        if (skill < required)
            return Colour::Locked;
        if (skill >= green)
            return Colour::Green;
        if (skill >= yellow)
            return Colour::Yellow;
        return Colour::Orange;
    }

    // The XP an action of requirement `required` and colour `c` gives: base + perSkill x requirement, rounded, in
    // full for orange and yellow, greenPercent of it for green, nothing for grey (or locked).
    inline uint32_t XpFor(XpRules const& r, Colour c, uint32_t required)
    {
        double const full = std::floor(r.base + r.perSkill * double(required) + 0.5);
        switch (c)
        {
            case Colour::Orange:
            case Colour::Yellow:
                return full > 0 ? uint32_t(full) : 0;
            case Colour::Green:
                return full > 0 ? uint32_t(std::floor(full * r.greenPercent / 100.0 + 0.5)) : 0;
            default:
                return 0;
        }
    }

    inline char const* ColourName(Colour c)
    {
        switch (c)
        {
            case Colour::Orange: return "orange";
            case Colour::Yellow: return "yellow";
            case Colour::Green: return "green";
            case Colour::Grey: return "grey";
            default: return "locked";
        }
    }
}

#endif
