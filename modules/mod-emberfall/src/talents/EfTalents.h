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

// Emberfall talents (docs/design/talents.md §3; WPs M1-M5): the tree data loaded from the ef_* world tables,
// the per-character state in the ef_character_* tables, the EFT protocol and the apply path. The hooks and
// GM commands that drive it are in src/emberfall_talents.cpp.

#pragma once

#include "Define.h"
#include "EfTalentModel.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

class ChatHandler;
class Player;

namespace Emberfall::Talents
{
    // Emberfall.Talents.Enable (worldserver.conf or AC_EMBERFALL_TALENTS_ENABLE; default 0), read at config load; a
    // `.reload config` that changes it loads or drops the trees at once.
    bool Enabled();
    void ReadConfig(bool reload);

    // Loads the ef_* world tables (at startup and on `.ef talents reload`); false and the system off for every
    // class when they are missing or inconsistent. Logs what was loaded.
    bool Load();

    // The loaded data while the system is on, else null.
    std::shared_ptr<ef::talents::EfTalentModel const> Data();

    // True when the system is on and the player's class has trees (the stock talent system is then off for it).
    bool Active(Player const* player);

    // Hooks.
    void OnLogin(Player* player);
    void OnLevelChanged(Player* player, uint8 oldLevel);
    // An EFT body (the text after `EFT\t`) the player sent to itself.
    void HandleMessage(Player* player, std::string_view body);
    // The switch cast (§1.5, §3.2): every tick, finishes or fails a running switch; a spell that went off; combat
    // cancels the cast.
    void OnUpdate(Player* player);
    void OnSpellCast(Player* player, uint32 spellId);
    void OnEnterCombat(Player* player);

    // GM commands (`.ef ...`): lines to print, or false with the reason in `out`.
    void Dump(Player* player, std::vector<std::string>& out);
    bool GmSetSpec(Player* player, uint32 specId, std::string& out);
    bool GmReset(Player* player, std::string& out);
}
