/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
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

#ifndef EMBERFALL_SKIN_LOOT_H
#define EMBERFALL_SKIN_LOOT_H

// EMBERFALL: a skin whoever killed the beast (PATCHES.md #45; Emberfall alpha1 feedback #709, 2026-11-22: anyone with the
// skill skins a corpse whose loot is all taken). Player::SendLoot looks the corpse's loot recipient up among the players
// online (Creature::GetLootRecipient) and returned when neither they nor their group were found, before it looked at
// the loot type. Spell::EffectSkinning had already taken the corpse's skinnable flag and counted the skill-up, so a
// skinner at the corpse of a killer who had logged out (a player, or a bot leaving the realm) got no skin, and the
// corpse could not be skinned again. The recipient matters for the kill's own loot (who may take it); the skin is the
// skinner's (SendLoot fills it for them and makes them the recipient), so a skin is sent whoever and wherever the killer
// is.
//
// The skin's reach: Player::SendLoot, WorldSession::HandleAutostoreLootItemOpcode and DoLootRelease ask a corpse's
// looter to stand within INTERACTION_DISTANCE (5.5 yd and the two bounding radii), while our skinning spells reach 8 yd
// and the Knife tree's Long Reach adds up to 4 (Emberfall content/data/professions/specs.yaml): a skin cast from 6-12 yd
// took the flag and counted the skill-up, and its loot was never sent (or could not be taken). A skin's loot is sent,
// taken and released within kReach (the longest skinning reach, 12 yd, with room for the spell's own range leeway);
// every other loot keeps INTERACTION_DISTANCE. No core header: modules/mod-emberfall/tests/skinloot_tests.cpp tests
// this file on its own.

namespace Emberfall::SkinLoot
{
    // Whether Player::SendLoot needs the corpse's loot recipient (or their group) online to send this loot: every
    // corpse loot but a skin (the requested type LOOT_SKINNING).
    constexpr bool NeedsRecipientOnline(bool skinning)
    {
        return !skinning;
    }

    // How far from a corpse its looter may stand for this loot: a skin within kReach, everything else within the
    // interaction distance the caller passes (INTERACTION_DISTANCE).
    constexpr float kReach = 15.0f;
    constexpr float LootReach(bool skinning, float interactionDistance)
    {
        return skinning && kReach > interactionDistance ? kReach : interactionDistance;
    }
}

#endif // EMBERFALL_SKIN_LOOT_H
