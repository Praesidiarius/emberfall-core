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

// Emberfall talents (docs/design/talents.md §1.5, §3.6; WP M4): the pure parts of saved loadouts and per-spec
// action bars, without the core (tests/talent_tests.cpp runs them standalone).

#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <set>
#include <vector>

namespace ef::talents
{
    constexpr uint32_t kMaxActionButtons = 144; // Player.h MAX_ACTION_BUTTONS
    constexpr uint32_t kFirstBarButtons = 12;   // buttons 0-11: the first bar
    constexpr uint8_t kSpellButton = 0x00;      // ACTION_BUTTON_SPELL
    constexpr uint32_t kMaxLoadoutId = 255;     // ef_character_loadout.loadout is a TINYINT UNSIGNED

    // One action button as the core stores it (character_action, ef_character_actions).
    struct BarButton
    {
        uint32_t button = 0;
        uint32_t action = 0;
        uint32_t type = 0;

        bool operator==(const BarButton& o) const { return button == o.button && action == o.action && type == o.type; }
    };

    // The ID a new saved loadout of a spec gets: the smallest unused one from 1, or 0 when the spec already has
    // `max` loadouts (ERR LIMIT; the active one counts, §1.5).
    inline uint32_t NewLoadoutId(const std::set<uint32_t>& used, uint32_t max)
    {
        if (used.size() >= max)
            return 0;
        for (uint32_t id = 1; id <= kMaxLoadoutId; ++id)
            if (!used.count(id))
                return id;
        return 0;
    }

    // The bars of a spec entered for the first time (§1.5): a copy of the current bars without the spell buttons
    // whose spell the spec does not know, then each known `baseline` spell (in order) that is on no button goes
    // into the first empty button of the first bar (none left: it stays off the bars).
    inline std::vector<BarButton> FirstBars(const std::vector<BarButton>& current, const std::function<bool(uint32_t)>& known,
                                            const std::vector<uint32_t>& baseline)
    {
        std::vector<BarButton> out;
        for (const BarButton& b : current)
            if (b.button < kMaxActionButtons && (b.type != kSpellButton || known(b.action)))
                out.push_back(b);
        for (uint32_t spell : baseline)
        {
            if (!known(spell))
                continue;
            if (std::any_of(out.begin(), out.end(), [&](const BarButton& b) { return b.type == kSpellButton && b.action == spell; }))
                continue;
            for (uint32_t button = 0; button < kFirstBarButtons; ++button)
                if (std::none_of(out.begin(), out.end(), [&](const BarButton& b) { return b.button == button; }))
                {
                    out.push_back(BarButton{button, spell, kSpellButton});
                    break;
                }
        }
        std::sort(out.begin(), out.end(), [](const BarButton& a, const BarButton& b) { return a.button < b.button; });
        return out;
    }
}
