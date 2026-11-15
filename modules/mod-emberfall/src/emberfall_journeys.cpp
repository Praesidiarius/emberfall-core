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

// Emberfall's Class Journeys (docs/design/class-journeys/README.md): class quest lines that teach utility abilities
// outside the talent trees. The content build writes the steps and their quests (ef_journey_step, ef_journey_quest,
// ef_journey_cast, ef_journey_tame_credit; tools/py/emberfall_content/journeys.py); the quests' order is the world DB's
// own (each step's first quests are offered when the step before's mark is known and their own is not: conditions).
//
//   - A step is done when its mark is known (a learned spell, `character_spell`): its last quest's reward teaches the
//     mark and its second spell (OnPlayerCompleteQuest), and a login teaches them again to a character who has such a
//     quest rewarded and lost the spell (a GM's unlearn, a step whose spells changed).
//   - Characters from before the journey (once each, the first login with this script: ef_character_migration
//     `class_journeys`): a step's `existing` rule teaches it without its quests (level: at or above its level; tamed: a
//     hunter with Beastcraft or a companion). Nothing is taken away.
//   - Bots (mod-playerbots, Emberfall mode): every step at or below their level, at the bot's making and each level-up
//     (BotSteps, called where mod-playerbots applies a bot's talents).
//   - "Use it there" objectives (ef_journey_cast): a cast of the row's spell inside its zone or area, with the quest in
//     the log, gives the row's kill credit.
//   - The Hunter's taming asks TameGateLevel (a beast's rank against the gates' marks; the step's own quest in the log
//     opens it for that lesson) and CreditTame (ef_journey_tame_credit). The Hunter's own abilities (Beast at Heel, Call
//     from the Stable, Stable at Hand) are emberfall_journeys_hunter.cpp.
//   - `.efjourney state` the selected player's (or your own) steps; `.efjourney done N` teaches steps 1..N (tests);
//     `.efjourney reload`.
// Config: Emberfall.Journeys.Enable (1).

#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "EmberfallOwnData.h"
#include "Log.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerScript.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldScript.h"
#include "journeys/EfJourneyLogic.h"
#include "journeys/EfJourneys.h"

#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace Acore::ChatCommands;
namespace J = Emberfall::Journeys;

namespace
{
    constexpr char const* kMigration = "class_journeys";
    constexpr uint32 kSkillBeastcraft = 900;   // emberfall_taming.cpp: the hunters' Beastcraft

    struct QuestRow
    {
        uint8 cls = 0;
        uint8 step = 0;
        bool last = false;
    };

    bool s_enable = true;
    std::shared_mutex s_lock;
    std::vector<J::Step> s_steps;                              // by class, then step
    std::unordered_map<uint32, QuestRow> s_quests;
    std::unordered_map<uint32, std::vector<J::Cast>> s_casts;  // by spell
    std::vector<J::TameCredit> s_tameCredits;
    std::unordered_set<uint32> s_spells;                       // every mark and second spell

    void Load()
    {
        std::vector<J::Step> steps;
        std::unordered_map<uint32, QuestRow> quests;
        std::unordered_map<uint32, std::vector<J::Cast>> casts;
        std::vector<J::TameCredit> credits;
        std::unordered_set<uint32> spells;
        if (QueryResult r = WorldDatabase.Query("SELECT `class`, `step`, `level`, `spell`, `spell2`, `gate`, `existing` FROM "
                                                "`ef_journey_step` ORDER BY `class`, `step`"))
            do
            {
                Field* f = r->Fetch();
                J::Step s{f[0].Get<uint8>(), f[1].Get<uint8>(), f[2].Get<uint8>(), f[3].Get<uint32>(), f[4].Get<uint32>(),
                          f[5].Get<uint8>(), f[6].Get<uint8>()};
                for (uint32 spell : {s.spell, s.spell2})
                    if (spell)
                    {
                        if (!sSpellMgr->GetSpellInfo(spell))
                            LOG_ERROR("server.loading", ">> Emberfall journeys: class {} step {} names spell {}, not in Spell.dbc",
                                      s.cls, s.step, spell);
                        spells.insert(spell);
                    }
                steps.push_back(s);
            } while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query("SELECT `quest`, `class`, `step`, `last` FROM `ef_journey_quest`"))
            do
            {
                Field* f = r->Fetch();
                quests[f[0].Get<uint32>()] = {f[1].Get<uint8>(), f[2].Get<uint8>(), f[3].Get<uint8>() != 0};
            } while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query("SELECT `quest`, `spell`, `area`, `credit` FROM `ef_journey_cast`"))
            do
            {
                Field* f = r->Fetch();
                J::Cast c{f[0].Get<uint32>(), f[1].Get<uint32>(), f[2].Get<uint32>(), f[3].Get<uint32>()};
                casts[c.spell].push_back(c);
            } while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query("SELECT `creature`, `ranks`, `min_level` FROM `ef_journey_tame_credit`"))
            do
            {
                Field* f = r->Fetch();
                credits.push_back({f[0].Get<uint32>(), f[1].Get<uint8>(), f[2].Get<uint32>()});
            } while (r->NextRow());
        std::unique_lock<std::shared_mutex> guard(s_lock);
        s_steps = std::move(steps);
        s_quests = std::move(quests);
        s_casts = std::move(casts);
        s_tameCredits = std::move(credits);
        s_spells = std::move(spells);
        LOG_INFO("server.loading", ">> Emberfall journeys: {} ({} steps, {} quests, {} place objectives, {} tame credits)",
                 s_enable ? "on" : "off", s_steps.size(), s_quests.size(), s_casts.size(), s_tameCredits.size());
    }

    bool On() { return s_enable && Emberfall::OwnData(); }

    std::vector<J::Step> StepsOf(uint8 cls)
    {
        std::shared_lock<std::shared_mutex> guard(s_lock);
        std::vector<J::Step> out;
        for (J::Step const& s : s_steps)
            if (s.cls == cls)
                out.push_back(s);
        return out;
    }

    // Teaches what's missing of these spells; the count taught.
    uint32 TeachSpells(Player* player, std::vector<uint32> const& spells, char const* why)
    {
        uint32 n = 0;
        for (uint32 spell : spells)
            if (spell && !player->HasSpell(spell) && sSpellMgr->GetSpellInfo(spell))
            {
                player->learnSpell(spell);
                ++n;
            }
        if (n)
            LOG_INFO("module", "Emberfall journeys: {} ({}) learned {} journey spell(s): {}", player->GetName(),
                     player->GetGUID().GetCounter(), n, why);
        return n;
    }

    void TeachStep(Player* player, J::Step const& s, char const* why)
    {
        std::vector<uint32> spells;
        J::AddSpells(spells, s);
        TeachSpells(player, spells, why);
    }

    bool Tamed(Player* player)
    {
        if (player->HasSkill(kSkillBeastcraft) || player->GetPet())
            return true;
        if (PetStable const* stable = player->GetPetStable())
        {
            if (stable->CurrentPet || !stable->UnslottedPets.empty())
                return true;
            for (auto const& pet : stable->StabledPets)
                if (pet)
                    return true;
        }
        return false;
    }

    // A step whose last quest is rewarded: its spells (they came back if lost).
    void Repair(Player* player, std::vector<J::Step> const& steps)
    {
        std::vector<uint32> spells;
        {
            std::shared_lock<std::shared_mutex> guard(s_lock);
            for (auto const& [quest, row] : s_quests)
                if (row.last && row.cls == player->getClass() && player->IsQuestRewarded(quest))
                    for (J::Step const& s : steps)
                        if (s.step == row.step)
                            J::AddSpells(spells, s);
        }
        TeachSpells(player, spells, "a rewarded step");
    }

    // The characters from before the journey, once (README §2.5, hunter.md §3).
    void Migrate(Player* player, std::vector<J::Step> const& steps)
    {
        uint32 const guid = player->GetGUID().GetCounter();
        if (CharacterDatabase.Query("SELECT 1 FROM `ef_character_migration` WHERE `guid` = {} AND `name` = '{}'", guid, kMigration))
            return;
        J::Who who{player->getClass(), player->GetLevel(), false, true, player->getClass() == CLASS_HUNTER && Tamed(player)};
        TeachSpells(player, J::SpellsOwed(steps, who), "the journey came (existing character)");
        CharacterDatabase.Execute("INSERT IGNORE INTO `ef_character_migration` (`guid`, `name`) VALUES ({}, '{}')", guid, kMigration);
    }

    class emberfall_journeys_world : public WorldScript
    {
    public:
        emberfall_journeys_world()
            : WorldScript("emberfall_journeys_world", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_LOAD_CUSTOM_DATABASE_TABLE }) { }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            s_enable = sConfigMgr->GetOption<bool>("Emberfall.Journeys.Enable", true, false);
        }

        void OnLoadCustomDatabaseTable() override { Load(); }
    };

    class emberfall_journeys_player : public PlayerScript
    {
    public:
        emberfall_journeys_player()
            : PlayerScript("emberfall_journeys_player", { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST,
                PLAYERHOOK_ON_SPELL_CAST }) { }

        void OnPlayerLogin(Player* player) override
        {
            if (!On())
                return;
            std::vector<J::Step> const steps = StepsOf(player->getClass());
            if (steps.empty())
                return;
            Migrate(player, steps);
            Repair(player, steps);
        }

        // Player::RewardQuest's end: a step's last quest teaches the step.
        void OnPlayerCompleteQuest(Player* player, Quest const* quest) override
        {
            if (!On() || !quest)
                return;
            QuestRow row;
            {
                std::shared_lock<std::shared_mutex> guard(s_lock);
                auto it = s_quests.find(quest->GetQuestId());
                if (it == s_quests.end() || !it->second.last)
                    return;
                row = it->second;
            }
            if (row.cls != player->getClass())
                return;
            for (J::Step const& s : StepsOf(row.cls))
                if (s.step == row.step)
                    TeachStep(player, s, "the step's last quest");
        }

        // "Use it there" (ef_journey_cast): the cast's credit inside its place, with the quest in the log.
        void OnPlayerSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
        {
            if (!On() || !spell)
                return;
            uint32 const id = spell->GetSpellInfo()->Id;
            std::vector<J::Cast> mine;
            {
                std::shared_lock<std::shared_mutex> guard(s_lock);
                auto it = s_casts.find(id);
                if (it == s_casts.end())
                    return;
                mine = it->second;
            }
            for (J::Cast const& c : mine)
                if (J::CastCounts(c, id, player->GetZoneId(), player->GetAreaId())
                    && player->GetQuestStatus(c.quest) == QUEST_STATUS_INCOMPLETE)
                    player->KilledMonsterCredit(c.credit);
        }
    };

    class emberfall_journeys_commands : public CommandScript
    {
    public:
        emberfall_journeys_commands() : CommandScript("emberfall_journeys_commands") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable journeyTable =
            {
                { "state",  HandleState,  SEC_GAMEMASTER, Console::No },
                { "done",   HandleDone,   SEC_GAMEMASTER, Console::No },
                { "reload", HandleReload, SEC_GAMEMASTER, Console::Yes },
            };
            static ChatCommandTable commandTable =
            {
                { "efjourney", journeyTable },
            };
            return commandTable;
        }

        static Player* Who(ChatHandler* handler)
        {
            Player* p = handler->getSelectedPlayerOrSelf();
            if (!p)
                handler->SendSysMessage("EFJOURNEY select a player");
            return p;
        }

        static bool HandleState(ChatHandler* handler)
        {
            Player* p = Who(handler);
            if (!p)
                return true;
            std::vector<J::Step> const steps = StepsOf(p->getClass());
            handler->PSendSysMessage("EFJOURNEY {} class {} level {}: {} steps", p->GetName(), p->getClass(), p->GetLevel(), steps.size());
            for (J::Step const& s : steps)
                handler->PSendSysMessage("EFJOURNEY step {} (level {}): mark {} {}{}", s.step, s.level, s.spell,
                                         p->HasSpell(s.spell) ? "done" : (p->GetLevel() >= s.level ? "ready" : "ahead"),
                                         s.gate ? Acore::StringFormat(", gate {}", s.gate) : "");
            return true;
        }

        static bool HandleDone(ChatHandler* handler, uint8 upTo)
        {
            Player* p = Who(handler);
            if (!p)
                return true;
            for (J::Step const& s : StepsOf(p->getClass()))
                if (s.step <= upTo)
                    TeachStep(p, s, "a GM's .efjourney done");
            handler->PSendSysMessage("EFJOURNEY {}: steps 1-{} done", p->GetName(), upTo);
            return true;
        }

        static bool HandleReload(ChatHandler* handler)
        {
            Load();
            handler->SendSysMessage("EFJOURNEY reloaded");
            return true;
        }
    };
}

namespace Emberfall::Journeys
{
    uint32 TameGateLevel(Player* player, uint32 creatureRank)
    {
        if (!On() || !player)
            return 0;
        uint8 const gate = GateOfRank(creatureRank);
        if (gate == kGateNone)
            return 0;
        J::Step step;
        std::vector<uint32> lesson;
        {
            std::shared_lock<std::shared_mutex> guard(s_lock);
            J::Step const* s = GateStep(s_steps, player->getClass(), gate);
            if (!s)
                return 0;
            step = *s;
            for (auto const& [quest, row] : s_quests)
                if (row.cls == step.cls && row.step == step.step)
                    lesson.push_back(quest);
        }
        if (player->HasSpell(step.spell))
            return 0;
        for (uint32 quest : lesson)
            if (player->GetQuestStatus(quest) == QUEST_STATUS_INCOMPLETE)
                return 0;
        return step.level;
    }

    void CreditTame(Player* player, uint32 creatureRank, uint32 beastLevel)
    {
        if (!On() || !player)
            return;
        std::vector<uint32> credits;
        {
            std::shared_lock<std::shared_mutex> guard(s_lock);
            credits = TameCredits(s_tameCredits, creatureRank, beastLevel);
        }
        for (uint32 c : credits)
            player->KilledMonsterCredit(c);
    }

    void BotSteps(Player* bot)
    {
        if (!On() || !bot)
            return;
        J::Who who{bot->getClass(), bot->GetLevel(), true, false, false};
        TeachSpells(bot, SpellsOwed(StepsOf(bot->getClass()), who), "a bot's steps of its level");
    }

    bool IsJourneySpell(uint32 spell)
    {
        std::shared_lock<std::shared_mutex> guard(s_lock);
        return s_spells.count(spell) != 0;
    }
}

void AddEmberfallJourneysScripts()
{
    new emberfall_journeys_world();
    new emberfall_journeys_player();
    new emberfall_journeys_commands();
}
