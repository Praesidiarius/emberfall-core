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

// Emberfall (PATCHES.md, "Module"): one main profession (Emberfall docs/design/professions.md §25, Nathanael
// 2026-11-06). The rule itself is data and configuration: the crafts are the only category-11 skill lines (Mining,
// Herbalism and Skinning are secondary, category 9) and MaxPrimaryTradeSkill is 1, so a trainer refuses a second craft.
// This script moves the characters from before, once each:
//
// At a character's first login with this script (no `one_profession` row in ef_character_migration for it): when it
// knows more category-11 lines than MaxPrimaryTradeSkill allows, it keeps the best (EfOneProfession.h: the higher skill,
// then the higher rank, then the lower line ID) and the others are unlearned as the professions window's Unlearn does
// (CMSG_UNLEARN_SKILL: Player::SetSkill 0, which takes the line's spells and recipes, and mod-emberfall's profession
// specialisations drop that line's ranks, OnPlayerSetSkill); its free profession points are set to what is left (the
// core's refund on an unlearn would count one too many: the load never took a point for the second craft). It gets a
// mail in the client's language (enUS, deDE, frFR) saying what it kept and lost. Gathering and secondary skills are
// never touched. Then the row is written, in one transaction with the character's save and the mail, so a crash before
// it leaves the character as it was and the next login does it again; after it, never again (a GM's character given
// more crafts later keeps them). On with Emberfall.OwnData = 1 and Emberfall.OneProfession.Enable (default 1).

#include "Config.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "EmberfallOwnData.h"
#include "Log.h"
#include "Mail.h"
#include "Player.h"
#include "PlayerScript.h"
#include "SharedDefines.h"
#include "SpellMgr.h"
#include "World.h"
#include "WorldConfig.h"
#include "WorldScript.h"
#include "WorldSession.h"
#include "professions/EfOneProfession.h"

using namespace Emberfall::Professions;

namespace
{
    constexpr char const* kMigration = "one_profession";
    bool s_enable = true;

    std::string NameOf(uint32 skill, Lang lang)
    {
        std::string name = CraftName(skill, lang);
        if (name.empty())
            if (SkillLineEntry const* line = sSkillLineStore.LookupEntry(skill))
                name = line->name[0];
        return name.empty() ? std::to_string(skill) : name;
    }

    void Migrate(Player* player)
    {
        uint32 const guid = player->GetGUID().GetCounter();
        if (CharacterDatabase.Query("SELECT 1 FROM `ef_character_migration` WHERE `guid` = {} AND `name` = '{}'", guid,
                                    kMigration))
            return;

        uint32 const limit = sWorld->getIntConfig(CONFIG_MAX_PRIMARY_TRADE_SKILL);
        std::vector<Craft> crafts;
        for (uint32 id = 0; id < sSkillLineStore.GetNumRows(); ++id)
            if (IsPrimaryProfessionSkill(id) && player->HasSkill(id))
                crafts.push_back({ id, player->GetPureSkillValue(id), player->GetPureMaxSkillValue(id) });
        Cut const cut = CutTo(crafts, limit);

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        if (!cut.drop.empty())
        {
            Lang const lang = LangOf(player->GetSession() ? uint32(player->GetSession()->GetSessionDbcLocale()) : 0);
            std::vector<std::string> dropped;
            for (Craft const& c : cut.drop)
            {
                player->SetSkill(c.skill, 0, 0, 0);
                dropped.push_back(NameOf(c.skill, lang));
                LOG_INFO("module", "Emberfall one profession: {} ({}) unlearned skill {} at {}/{}", player->GetName(), guid,
                         c.skill, c.value, c.max);
            }
            player->SetFreePrimaryProfessions(limit > cut.keep.size() ? uint16(limit - cut.keep.size()) : 0);

            std::string const kept = cut.keep.empty() ? std::string() : NameOf(cut.keep.front().skill, lang);
            Letter const letter = MailFor(lang, kept, JoinNames(dropped, lang));
            MailDraft(letter.subject, letter.body)
                .SendMailTo(trans, player, MailSender(player, MAIL_STATIONERY_GM), MAIL_CHECK_MASK_COPIED);
            player->SaveToDB(trans, false, false);
            LOG_INFO("module", "Emberfall one profession: {} ({}) keeps skill {} ({} unlearned)", player->GetName(), guid,
                     cut.keep.empty() ? 0 : cut.keep.front().skill, cut.drop.size());
        }
        trans->Append("INSERT IGNORE INTO `ef_character_migration` (`guid`, `name`) VALUES ({}, '{}')", guid, kMigration);
        CharacterDatabase.CommitTransaction(trans);
    }

    class emberfall_one_profession_world : public WorldScript
    {
    public:
        emberfall_one_profession_world() : WorldScript("emberfall_one_profession_world", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            s_enable = sConfigMgr->GetOption<bool>("Emberfall.OneProfession.Enable", true, false);
            if (Emberfall::OwnData())
                LOG_INFO("server.loading", ">> Emberfall one profession: login migration {} (MaxPrimaryTradeSkill {})",
                         s_enable ? "on" : "off", sWorld->getIntConfig(CONFIG_MAX_PRIMARY_TRADE_SKILL));
        }
    };

    class emberfall_one_profession_player : public PlayerScript
    {
    public:
        emberfall_one_profession_player() : PlayerScript("emberfall_one_profession_player", {
            PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_DELETE_FROM_DB }) { }

        void OnPlayerLogin(Player* player) override
        {
            if (s_enable && player && Emberfall::OwnData())
                Migrate(player);
        }

        void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 guid) override
        {
            trans->Append("DELETE FROM `ef_character_migration` WHERE `guid` = {}", guid);
        }
    };
}

void AddEmberfallOneProfessionScripts()
{
    new emberfall_one_profession_world();
    new emberfall_one_profession_player();
}
