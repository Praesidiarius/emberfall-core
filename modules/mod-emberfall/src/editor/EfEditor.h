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

// Emberfall's world editor on the build realm (Emberfall docs/design/world-editor.md §6-§7, phase A minimal): creature
// spawns (add, move, turn, delete, wander, respawn) and NPC moves in the editable zones, kept as operations in a
// database of their own (`Emberfall.Editor.Database`, build_editor) and applied in memory, live and again at every
// start. The world database is never written. Off unless Emberfall.Editor.Enable = 1 and the database is set; off, the
// module answers no EFE message at all.
//
// Config (worldserver.conf or AC_EMBERFALL_EDITOR_* environment):
//   Emberfall.Editor.Enable     0/1
//   Emberfall.Editor.Database   the editor database's name (build_editor); the realm's MySQL user owns it
//   (editor access: the realm's own auth database, table ef_editor_access, written by the account site)
//   Emberfall.Editor.Mode       realm (default) | local: a maintainer's server (§15): every account is a maintainer, no
//                               limits, Submit accepts the set at once (make editor-pull writes it into the YAML)
//   Emberfall.Editor.RefreshSeconds  access and review state re-read (default 60)

#pragma once

#include "Define.h"

#include <string>
#include <string_view>
#include <vector>

class Player;

namespace Emberfall::Editor
{
    void ReadConfig(bool reload);
    bool Enabled();
    // After the world is loaded: the release data (ef_editor_* world tables), the change sets, and the replay (§7.3).
    void OnStartup();
    void OnUpdate(uint32 diff);
    void OnLogin(Player* player);
    void OnLogout(Player* player);
    // The body of an `EFE\t` addon whisper the player sent to itself.
    void HandleMessage(Player* player, std::string_view body);
    // An addon whisper with the EFE prefix that was not to oneself (refused and logged).
    void HandleForeign(Player* player);
    // Re-read one account's editor access now (0: every account), and the review states (the site's nudge).
    void Refresh(uint32 accountId);
    void Status(std::vector<std::string>& lines);
}
