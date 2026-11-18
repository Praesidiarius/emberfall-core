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

// Veins worked in swings (emberfall_vein_swings.cpp; the rule: EfVeinSwingsLogic.h): what the profession
// specialisations need of them.

#ifndef EMBERFALL_VEIN_SWINGS_H
#define EMBERFALL_VEIN_SWINGS_H

class GameObject;
class Player;
struct Loot;

namespace Emberfall::VeinSwings
{
    // a chest worked in swings (its template's max_opens above one)
    bool IsVein(GameObject const* go);
    // a swing of `go` after `player`'s first one on it since it spawned (Spell::EffectOpenLock sends the loot before
    // it puts the player on the object's skill-up list, so on the first swing the player is not on it yet): the
    // yield talents roll once per vein and player, on the first swing
    bool IsLaterSwing(GameObject const* go, Player const* player);
    // one stack of each ore in a vein's swing (EfVeinSwingsLogic.h Merge); idempotent
    void MergeLoot(GameObject const* go, Loot* loot);
}

#endif
