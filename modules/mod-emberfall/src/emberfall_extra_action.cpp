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

// Emberfall (PATCHES.md, "Module"): the extra action button (Emberfall docs/contributing/quests.md "Moments: the extra
// action button"). A marker aura grants a player one spell for a while (src/extraaction/EfExtraActionLogic.h has the
// protocol); the client casts it like any spell, and the core's spellbook check asks this module first (the core's
// EmberfallExtraAction.h, PATCHES.md #34): the cast goes through while the player carries a marker granting that spell.
// Granting and taking the button is data (SmartAI: cast the marker on the players, remove it), so the module only
// answers that one question. On with Emberfall.OwnData = 1.

#include "EmberfallExtraAction.h"
#include "EmberfallOwnData.h"
#include "Player.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "extraaction/EfExtraActionLogic.h"

namespace
{
    bool HoldsExtraAction(Player const* player, uint32_t spellId)
    {
        if (!player || !spellId || !Emberfall::OwnData())
            return false;
        for (AuraEffect const* eff : player->GetAuraEffectsByType(SPELL_AURA_DUMMY))
        {
            Emberfall::ExtraAction::MarkerEffect e;
            e.auraType = SPELL_AURA_DUMMY;
            e.miscValue = eff->GetMiscValue();
            e.triggerSpell = eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell;
            if (Emberfall::ExtraAction::GrantedSpell(e) == spellId)
                return true;
        }
        return false;
    }
}

void AddEmberfallExtraActionScripts()
{
    Emberfall::ExtraAction::SetCheck(&HoldsExtraAction);
}
