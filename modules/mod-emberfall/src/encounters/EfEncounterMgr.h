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

// The mini-encounter framework's server side (Emberfall docs/design/mini-encounters.md §5, §7), as the kinds see it:
// a kind (fishing 'F' now, taming 'T' later) registers its handler, opens and ends encounters through the book (one per
// player), and sends EFN messages. emberfall_encounters.cpp owns the EFN chat hook, the throttle, the book, the item
// groups and the frees (logout, map change, death).

#ifndef EMBERFALL_ENCOUNTER_MGR_H
#define EMBERFALL_ENCOUNTER_MGR_H

#include "EfEncounterLogic.h"
#include "EfEncounterWire.h"

#include <optional>
#include <string>
#include <string_view>

class Player;

namespace Emberfall::Encounters
{
    // Why the framework frees an encounter.
    enum class FreeReason { Logout, MapChange, Died };

    // A kind's handler. Called on the player's map thread, outside the framework's lock.
    class Kind
    {
    public:
        virtual ~Kind() = default;
        // Whether the kind is switched on (its own config switch); an off kind's messages get ERR;NOENC.
        virtual bool Enabled() const = 0;
        // Every EFN message of this kind from the player's own client (HELLO included).
        virtual void OnMessage(Player* player, efenc::Message const& msg) = 0;
        // The framework ends the player's open encounter of this kind (logout: the player is leaving the world).
        virtual void OnFree(Player* player, Open const& open, FreeReason reason) = 0;
    };

    // Registers a kind's handler for its letter (once, at script load; the handler lives as long as the server).
    void RegisterKind(char kind, Kind* handler);

    // Emberfall.Encounters.Enable.
    bool Enabled();

    // Opens an encounter for the player (nullopt: one is open already).
    std::optional<Open> Begin(Player* player, char kind, std::uint64_t subject);
    // Ends the player's encounter `id` (0: whichever).
    void Finish(Player* player, std::uint32_t id = 0);
    std::optional<Open> Current(Player* player);

    // Sends an EFN body (without the prefix) to the player's own client; logged for `.efenc dump`.
    void Send(Player* player, std::string const& body);
    // ERR;<code>;<detail>
    void SendErr(Player* player, std::uint32_t seq, char kind, std::string_view code, std::string const& detail = {});
    // The next sequence number of the server's messages to this player.
    std::uint32_t NextSeq(Player* player);

    // ef_enc_item_group (read only after boot or `.effish reload`/`.efenc reload`).
    ItemGroups const& Groups();
}

#endif
