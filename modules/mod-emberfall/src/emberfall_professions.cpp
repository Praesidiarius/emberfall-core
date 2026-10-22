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

// Emberfall (PATCHES.md, "Module"): character XP from the primary professions (Emberfall docs/design/professions.md,
// the rank system; Sundered Vale progression.md §5). Every gather (a node, a skinned corpse) and every craft of a
// primary profession (SkillLine category 11) gives XP by its colour for the player: orange and yellow in full, green
// half, grey nothing. On with Emberfall.OwnData = 1 and Emberfall.Professions.Xp.Enable (default 1). The colours are
// the thresholds the core hands the hooks: gathering's are PATCHES.md #14's (+40/+80/+120 under own data), a recipe's
// its SkillLineAbility row (yellow TrivialSkillLineRankLow, green the midpoint, grey TrivialSkillLineRankHigh).

#include "Config.h"
#include "DBCStores.h"
#include "EmberfallOwnData.h"
#include "Log.h"
#include "Player.h"
#include "PlayerScript.h"
#include "SharedDefines.h"
#include "WorldScript.h"
#include "professions/EfProfessionXp.h"

using namespace Emberfall::Professions;

namespace
{
    bool s_enable = true;
    XpRules s_rules;

    bool IsPrimaryProfessionLine(uint32 skill)
    {
        SkillLineEntry const* line = sSkillLineStore.LookupEntry(skill);
        return line && line->categoryId == SKILL_CATEGORY_PROFESSION;
    }

    void Award(Player* player, uint32 skill, uint32 current, uint32 required, uint32 yellow, uint32 green, uint32 gray,
               char const* what)
    {
        if (!s_enable || !player || !Emberfall::OwnData() || !IsPrimaryProfessionLine(skill))
            return;
        Colour const c = ColourOf(current, required, yellow, green, gray);
        uint32 const xp = XpFor(s_rules, c, required);
        LOG_DEBUG("module", "Emberfall professions: {} {} skill {} {} requirement {}: {} -> {} XP", player->GetName(),
                  what, skill, current, required, ColourName(c), xp);
        if (xp)
            player->GiveXP(xp, nullptr);
    }

    class emberfall_professions_world : public WorldScript
    {
    public:
        emberfall_professions_world() : WorldScript("emberfall_professions_world", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            s_enable = sConfigMgr->GetOption<bool>("Emberfall.Professions.Xp.Enable", true, false);
            s_rules.base = sConfigMgr->GetOption<float>("Emberfall.Professions.Xp.Base", 70.0f, false);
            s_rules.perSkill = sConfigMgr->GetOption<float>("Emberfall.Professions.Xp.PerSkill", 0.06f, false);
            s_rules.greenPercent = sConfigMgr->GetOption<uint32>("Emberfall.Professions.Xp.GreenPercent", 50, false);
            if (Emberfall::OwnData())
                LOG_INFO("server.loading", ">> Emberfall profession XP: {} (base {}, {} per requirement point, green {}%)",
                         s_enable ? "on" : "off", s_rules.base, s_rules.perSkill, s_rules.greenPercent);
        }
    };

    class emberfall_professions_player : public PlayerScript
    {
    public:
        emberfall_professions_player() : PlayerScript("emberfall_professions_player", {
            PLAYERHOOK_ON_UPDATE_GATHERING_SKILL, PLAYERHOOK_ON_UPDATE_CRAFTING_SKILL }) { }

        // A node, a corpse: called once per gather (Spell::EffectOpenLock, Spell::EffectSkinning), before the roll.
        void OnPlayerUpdateGatheringSkill(Player* player, uint32 skill, uint32 current, uint32 gray, uint32 green,
                                          uint32 yellow, uint32& /*gain*/) override
        {
            // the requirement is the bottom of the orange band: yellow - 40 under own data (PATCHES.md #14)
            uint32 const required = yellow >= 40 ? yellow - 40 : 0;
            Award(player, skill, current, required, yellow, green, gray, "gathers");
        }

        // A craft (Player::UpdateCraftSkill), before the roll.
        void OnPlayerUpdateCraftingSkill(Player* player, SkillLineAbilityEntry const* ability, uint32 current,
                                         uint32& /*gain*/) override
        {
            if (!ability)
                return;
            uint32 const low = ability->TrivialSkillLineRankLow, high = ability->TrivialSkillLineRankHigh;
            Award(player, ability->SkillLine, current, ability->MinSkillLineRank, low, (low + high) / 2, high, "crafts");
        }
    };
}

void AddEmberfallProfessionsScripts()
{
    new emberfall_professions_world();
    new emberfall_professions_player();
}
