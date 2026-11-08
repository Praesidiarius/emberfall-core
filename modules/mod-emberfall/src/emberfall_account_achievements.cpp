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

// Account-wide achievements (Emberfall content/data/achievements.yaml). 3.3.5 knows only per-character achievements;
// an Achievement.dbc row with our flag 0x8000 (EF_ACHIEVEMENT_FLAG_ACCOUNT: the bit later clients give account-wide
// achievements, unused in 3.3.5) is shared by every character of the account:
//
// - Earned by one character (OnPlayerAchievementComplete): the account keeps it (ef_account_achievement, the
//   characters database; it outlives a deleted character), and every other character of the account gets the row in
//   character_achievement with the same date, so it loads with that character as if earned there (quietly: no toast,
//   no guild line for an alt that was not played).
// - At login (OnPlayerLogin): what the account has and this character lacks (a character made later, or one that was
//   in the world when another earned it) is granted a second later (Player::CompletedAchievement: the toast, and the
//   criteria of what builds on it, a continent's exploration); what this character has and the account lacks (earned
//   before this module) goes into the account, and to the others as above.
//
// Criteria progress stays per character (a zone half explored by one character and half by another is not explored).
// A game master in GM mode earns nothing (the core's rule): the grant waits for a login out of it.
//
// Storage: data/sql/db-characters/mod_emberfall_2026_11_08_00_account_achievements.sql (the table, and every
// character's achievement criteria checked once at its next login, AT_LOGIN_CHECK_ACHIEVS, so what was explored or
// reached before the achievements existed counts).

#include "AchievementMgr.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "Log.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "WorldSession.h"

#include <chrono>
#include <unordered_set>
#include <vector>

using namespace std::chrono_literals;

namespace
{
    constexpr uint32 EF_ACHIEVEMENT_FLAG_ACCOUNT = 0x00008000;

    bool IsAccountWide(AchievementEntry const* entry)
    {
        return entry && (entry->flags & EF_ACHIEVEMENT_FLAG_ACCOUNT);
    }

    // The account keeps `achievement` (its first date stays); every other character of the account gets it.
    void Share(uint32 account, ObjectGuid::LowType earner, uint32 achievement, uint32 date)
    {
        CharacterDatabase.Execute("INSERT IGNORE INTO `ef_account_achievement` (`account`, `achievement`, `date`) "
                                  "VALUES ({}, {}, {})", account, achievement, date);
        CharacterDatabase.Execute("INSERT IGNORE INTO `character_achievement` (`guid`, `achievement`, `date`) "
                                  "SELECT `guid`, {}, {} FROM `characters` WHERE `account` = {} AND `guid` <> {}",
                                  achievement, date, account, earner);
    }

    class emberfall_account_achievements_player : public PlayerScript
    {
    public:
        emberfall_account_achievements_player()
            : PlayerScript("emberfall_account_achievements_player", { PLAYERHOOK_ON_ACHI_COMPLETE, PLAYERHOOK_ON_LOGIN }) { }

        void OnPlayerAchievementComplete(Player* player, AchievementEntry const* achievement) override
        {
            if (!IsAccountWide(achievement) || !player->GetSession())
                return;
            Share(player->GetSession()->GetAccountId(), player->GetGUID().GetCounter(), achievement->ID,
                  uint32(GameTime::GetGameTime().count()));
        }

        void OnPlayerLogin(Player* player) override
        {
            if (!player->GetSession())
                return;
            uint32 const account = player->GetSession()->GetAccountId();
            ObjectGuid::LowType const guid = player->GetGUID().GetCounter();

            std::unordered_set<uint32> shared;
            if (QueryResult r = CharacterDatabase.Query("SELECT `achievement` FROM `ef_account_achievement` WHERE `account` = {}",
                                                        account))
            {
                do
                    shared.insert(r->Fetch()[0].Get<uint32>());
                while (r->NextRow());
            }

            // earned here before the account kept it: the account and the others get it
            for (auto const& [id, data] : player->GetAchievementMgr()->GetCompletedAchievements())
                if (!shared.count(id) && IsAccountWide(sAchievementStore.LookupEntry(id)))
                    Share(account, guid, id, uint32(data.date));

            // kept by the account, missing here: granted once the client has its achievement list
            std::vector<uint32> missing;
            for (uint32 id : shared)
                if (!player->HasAchieved(id) && IsAccountWide(sAchievementStore.LookupEntry(id)))
                    missing.push_back(id);
            if (missing.empty())
                return;
            LOG_DEBUG("module", "Emberfall account achievements: {} gets {} of account {}'s", player->GetName(),
                      missing.size(), account);
            player->m_Events.AddEventAtOffset([player, missing]
            {
                for (uint32 id : missing)
                    if (AchievementEntry const* entry = sAchievementStore.LookupEntry(id))
                        player->CompletedAchievement(entry);
            }, 1s);
        }
    };
}

void AddEmberfallAccountAchievementsScripts()
{
    new emberfall_account_achievements_player();
}
