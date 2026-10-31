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

// Emberfall's buffs at any level (Emberfall feedback #237: Verdant Cocoon, SpellLevel 27 but learned at 10, failed with
// SPELL_FAILED_LOWLEVEL on level-10 group members). Spell::AddUnitTarget scales a player's positive aura to the target's
// level: a target more than 10 levels under the spell's SpellLevel gets a lower rank, and with none low enough it is
// dropped (Spell::prepare's aura scale mask, Spell::SelectSpellTargets: SPELL_FAILED_LOWLEVEL when no target is left).
// The class kits have no ranks; their values grow with the caster's level instead. Emberfall's compiler gives every kit
// aura cast on someone else SPELL_ATTR2_ALLOW_LOW_LEVEL_BUFF ("ignore aura scaling"); for such a spell this hook marks
// the target as scaled, and SpellInfo::GetAuraRankForLevel returns the spell itself for it (the attribute), so the
// target keeps the spell with its own values.

#include "AllSpellScript.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellInfo.h"

namespace
{
    class emberfall_aura_scaling : public AllSpellScript
    {
    public:
        emberfall_aura_scaling() : AllSpellScript("emberfall_aura_scaling", { ALLSPELLHOOK_ON_SCALE_AURA_UNIT_ADD }) { }

        void OnScaleAuraUnitAdd(Spell* spell, Unit* /*target*/, uint32 /*effectMask*/, bool /*checkIfValid*/,
                                bool /*implicit*/, uint8 auraScaleMask, TargetInfo& targetInfo) override
        {
            if (!spell || targetInfo.scaleAura || !auraScaleMask || targetInfo.effectMask != auraScaleMask)
                return;
            if (spell->GetSpellInfo()->HasAttribute(SPELL_ATTR2_ALLOW_LOW_LEVEL_BUFF))
                targetInfo.scaleAura = true;
        }
    };
}

void AddEmberfallAuraScalingScripts()
{
    new emberfall_aura_scaling();
}
