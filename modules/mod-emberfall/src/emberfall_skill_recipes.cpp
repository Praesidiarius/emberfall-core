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

// Emberfall: a recipe learned at a skill value arrives the moment the skill reaches it (Emberfall docs/design/
// professions.md §27; 2026-11-14, Nathanael: a miner at 146 who mined iron himself had no iron ingot in his Smelting
// window). A SkillLineAbility row with AcquireMethod SKILL_LINE_ABILITY_LEARNED_ON_SKILL_VALUE (Emberfall's
// `acquire_method: at_skill_value`: Mining's ingots, Enchanting's glyphs, Beastcraft's) is taught by
// Player::learnSkillRewardedSpells, which the core calls at login (_LoadSkills), on SetSkill (a trainer's rank, `.setskill`)
// and in UpdateSkillPro only when the skill crosses one of its bonusSkillLevels (75, 150, ... 450): the iron ingot at
// 105 came at the next login or at 150.
//
// Here every skill rise (OnPlayerUpdateSkill: Player::UpdateSkill and UpdateSkillPro, so gathering, crafting, fishing and
// weapon skill-ups alike) runs the same learnSkillRewardedSpells for the new value: what is due is learned at once, with
// the client's "You have learned" line (SMSG_LEARNED_SPELL); what is known already is left alone. No core change.

#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"

namespace
{
    class emberfall_skill_recipes_player : public PlayerScript
    {
    public:
        emberfall_skill_recipes_player() : PlayerScript("emberfall_skill_recipes_player", { PLAYERHOOK_ON_UPDATE_SKILL }) { }

        void OnPlayerUpdateSkill(Player* player, uint32 skillId, uint32 value, uint32 /*max*/, uint32 /*step*/, uint32 newValue) override
        {
            if (player && newValue > value)
                player->learnSkillRewardedSpells(skillId, newValue);
        }
    };
}

void AddEmberfallSkillRecipesScripts()
{
    new emberfall_skill_recipes_player();
}
