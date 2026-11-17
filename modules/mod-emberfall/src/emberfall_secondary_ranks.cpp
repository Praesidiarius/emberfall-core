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

// Emberfall (PATCHES.md, "Module"): the secondary professions on the primaries' steps (Emberfall
// docs/design/secondary-professions.md §10, Nathanael 2026-11-16, feedback #596). The data moved Cooking, First Aid and
// Beastcraft from 75/150/225/300 (SkillTiers 2) to the primaries' 75/175/325/500 (SkillTiers 1) and every recipe's skill
// with them; this script moves the characters, once each:
//
// At a character's first login with this script (no `secondary_ranks_v2` row in ef_character_migration for it), in the
// world (OnPlayerLogin: nothing is taught or changed while Player::LoadFromDB runs), each of the three skills its save
// holds on the old steps (character_skills: the load itself already raised the cap to the rank spell's new one, not
// the value) is set to its value moved by the rule and the new cap of its rank (EfSecondaryRanks.h Migrate: 141/150 ->
// 163/175, 300/300 -> 500/500; Apprentice skills do not move). Player::SetSkill teaches what the new value reaches
// (Beastcraft's recipes come with the value) and mod-emberfall's talents load after it (this script is registered
// before the profession talents' and the taming's): the talent budget at the moved value is never below the old one
// (the rule's tests), so every spent point still fits. The character reads one line per moved skill in its client's
// language. The save and the row go in one transaction: a crash before leaves the save as it was and the next login
// does it again; after, never again. Bots log in through the same hook. On with Emberfall.OwnData = 1 and
// Emberfall.SecondaryRanks.Enable (default 1).

#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "EmberfallOwnData.h"
#include "Log.h"
#include "Player.h"
#include "PlayerScript.h"
#include "WorldScript.h"
#include "WorldSession.h"
#include "professions/EfSecondaryRanks.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace SR = Emberfall::Professions::SecondaryRanks;
using Emberfall::Professions::Lang;

namespace
{
    bool s_enable = true;

    void Migrate(Player* player)
    {
        uint32 const guid = player->GetGUID().GetCounter();
        if (CharacterDatabase.Query("SELECT 1 FROM `ef_character_migration` WHERE `guid` = {} AND `name` = '{}'", guid,
                                    SR::kMigration))
            return;

        // what the last save wrote: the load changed the cap already (the rank spells' SpellLearnSkill), not the value
        std::map<uint32, std::pair<uint32, uint32>> stored;
        if (QueryResult r = CharacterDatabase.Query("SELECT `skill`, `value`, `max` FROM `character_skills` WHERE `guid` = {} "
                                                    "AND `skill` IN ({}, {}, {})", guid, SR::kSkills[0], SR::kSkills[1], SR::kSkills[2]))
            do
            {
                Field* f = r->Fetch();
                stored[f[0].Get<uint16>()] = { f[1].Get<uint16>(), f[2].Get<uint16>() };
            } while (r->NextRow());

        Lang const lang = Emberfall::Professions::LangOf(player->GetSession() ? uint32(player->GetSession()->GetSessionDbcLocale()) : 0);
        std::vector<std::string> lines;
        for (uint32 skill : SR::kSkills)
        {
            auto const s = stored.find(skill);
            if (s == stored.end() || !player->HasSkill(skill))
                continue;
            auto const moved = SR::Migrate(s->second.first, s->second.second, player->GetPureMaxSkillValue(skill));
            if (!moved)
                continue;
            player->SetSkill(skill, moved->step, moved->value, moved->cap);
            lines.push_back(SR::LoginLine(lang, SR::SkillName(skill, lang), s->second.first, s->second.second, moved->value, moved->cap));
            LOG_INFO("module", "Emberfall secondary ranks: {} ({}) skill {} {}/{} -> {}/{}", player->GetName(), guid, skill,
                     s->second.first, s->second.second, moved->value, moved->cap);
        }

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        if (!lines.empty())
            player->SaveToDB(trans, false, false);
        trans->Append("INSERT IGNORE INTO `ef_character_migration` (`guid`, `name`) VALUES ({}, '{}')", guid, SR::kMigration);
        CharacterDatabase.CommitTransaction(trans);
        if (WorldSession* session = player->GetSession())
            for (std::string const& line : lines)
                ChatHandler(session).SendSysMessage(line);
    }

    class emberfall_secondary_ranks_world : public WorldScript
    {
    public:
        emberfall_secondary_ranks_world() : WorldScript("emberfall_secondary_ranks_world", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            s_enable = sConfigMgr->GetOption<bool>("Emberfall.SecondaryRanks.Enable", true, false);
            if (Emberfall::OwnData())
                LOG_INFO("server.loading", ">> Emberfall secondary ranks: login migration {}", s_enable ? "on" : "off");
        }
    };

    class emberfall_secondary_ranks_player : public PlayerScript
    {
    public:
        emberfall_secondary_ranks_player() : PlayerScript("emberfall_secondary_ranks_player", { PLAYERHOOK_ON_LOGIN }) { }

        // the ef_character_migration rows go with the character (emberfall_one_profession.cpp's OnPlayerDeleteFromDB)
        void OnPlayerLogin(Player* player) override
        {
            if (s_enable && player && Emberfall::OwnData())
                Migrate(player);
        }
    };
}

void AddEmberfallSecondaryRanksScripts()
{
    new emberfall_secondary_ranks_world();
    new emberfall_secondary_ranks_player();
}
