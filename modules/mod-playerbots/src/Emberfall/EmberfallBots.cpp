/*
 * Emberfall's part of mod-playerbots (the playerbots pilot): see EmberfallBots.h. Released under GNU GPL v2 or later.
 */

#include "EmberfallBots.h"

#include "CharacterDatabase.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotsDatabase.h"
#include "RandomPlayerbotMgr.h"
#include "Random.h"
#include "SpellMgr.h"
#include "StringConvert.h"
#include "Tokenize.h"

#include <map>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#if __has_include("EfTalents.h")
#include "EfTalents.h"
#define EF_BOTS_HAVE_TALENTS 1
#endif

namespace EmberfallBots
{
    namespace
    {
        bool s_enabled = false;
        std::map<uint32, Spec> s_specs;                       // spec id -> spec
        std::map<uint8, std::vector<uint32>> s_classSpecs;    // class -> its damage spec ids
        std::map<std::pair<uint8, uint8>, std::vector<std::string>> s_names; // (race, gender) -> free names
        std::mutex s_namesLock;
        std::unordered_map<uint32, uint32> s_botSpec;         // character guid -> spec id (ef_bot_character)
        std::shared_mutex s_botSpecLock;                      // bots update in the map threads

        std::set<uint32> ParseIds(std::string const& text)
        {
            std::set<uint32> out;
            for (std::string_view part : Acore::Tokenize(text, ',', false))
                if (Optional<uint32> v = Acore::StringTo<uint32>(part))
                    out.insert(*v);
            return out;
        }

        bool TableExists(char const* table)
        {
            QueryResult r = PlayerbotsDatabase.Query(
                "SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '{}'", table);
            return r && r->Fetch()[0].Get<uint64>() > 0;
        }
    }

    bool Enabled() { return s_enabled; }

    void Load()
    {
        s_enabled = false;
        s_specs.clear();
        s_classSpecs.clear();
        s_names.clear();
        if (!sConfigMgr->GetOption<bool>("Playerbots.Emberfall.Enable", false))
            return;
        for (char const* t : {"ef_bot_spec", "ef_bot_routine", "ef_bot_talent", "ef_bot_name", "ef_bot_character"})
            if (!TableExists(t))
            {
                LOG_ERROR("playerbots", "Emberfall bots: table {} is missing in the playerbots database; Emberfall mode stays off", t);
                return;
            }

        if (QueryResult r = PlayerbotsDatabase.Query(
                "SELECT spec_id, class_id, spec_key, ranged, weight, weapons, offhand, shield FROM ef_bot_spec"))
        {
            do
            {
                Field* f = r->Fetch();
                Spec s;
                s.id = f[0].Get<uint32>();
                s.classId = f[1].Get<uint8>();
                s.key = f[2].Get<std::string>();
                s.ranged = f[3].Get<uint8>() != 0;
                s.weight = std::max<uint32>(1, f[4].Get<uint32>());
                s.weapons = ParseIds(f[5].Get<std::string>());
                s.offhand = ParseIds(f[6].Get<std::string>());
                s.shield = f[7].Get<uint8>() != 0;
                s_classSpecs[s.classId].push_back(s.id);
                s_specs.emplace(s.id, std::move(s));
            } while (r->NextRow());
        }
        if (QueryResult r = PlayerbotsDatabase.Query(
                "SELECT spec_id, spell_id, kind, near_yd, below_pct, debuff_aura, buff_aura FROM ef_bot_routine ORDER BY spec_id, step"))
        {
            do
            {
                Field* f = r->Fetch();
                auto it = s_specs.find(f[0].Get<uint32>());
                if (it == s_specs.end())
                    continue;
                RoutineStep step;
                step.spell = f[1].Get<uint32>();
                step.kind = static_cast<StepKind>(f[2].Get<uint8>());
                step.nearYards = f[3].Get<float>();
                step.belowPct = f[4].Get<uint8>();
                step.debuffAura = f[5].Get<uint32>();
                step.buffAura = f[6].Get<uint32>();
                if (!sSpellMgr->GetSpellInfo(step.spell))
                {
                    LOG_ERROR("playerbots", "Emberfall bots: spec {} step spell {} does not exist", it->second.key, step.spell);
                    continue;
                }
                it->second.steps.push_back(step);
            } while (r->NextRow());
        }
        if (QueryResult r = PlayerbotsDatabase.Query("SELECT spec_id, node_id, node_rank FROM ef_bot_talent ORDER BY spec_id, step"))
        {
            do
            {
                Field* f = r->Fetch();
                auto it = s_specs.find(f[0].Get<uint32>());
                if (it != s_specs.end())
                    it->second.talents.emplace_back(f[1].Get<uint32>(), f[2].Get<uint32>());
            } while (r->NextRow());
        }
        uint32 names = 0;
        if (QueryResult r = PlayerbotsDatabase.Query("SELECT race, gender, name FROM ef_bot_name"))
        {
            do
            {
                Field* f = r->Fetch();
                s_names[{f[0].Get<uint8>(), f[1].Get<uint8>()}].push_back(f[2].Get<std::string>());
                ++names;
            } while (r->NextRow());
        }
        {
            std::unique_lock<std::shared_mutex> guard(s_botSpecLock);
            s_botSpec.clear();
            if (QueryResult r = PlayerbotsDatabase.Query("SELECT guid, spec_id FROM ef_bot_character"))
            {
                do
                {
                    Field* f = r->Fetch();
                    s_botSpec[f[0].Get<uint32>()] = f[1].Get<uint32>();
                } while (r->NextRow());
            }
        }
        s_enabled = !s_specs.empty();
        size_t steps = 0;
        for (auto const& [id, s] : s_specs)
            steps += s.steps.size();
        LOG_INFO("server.loading", ">> Emberfall bots: {} damage specs of {} classes, {} routine steps, {} names{}", s_specs.size(),
                 s_classSpecs.size(), steps, names, s_enabled ? "" : " (no specs: Emberfall mode stays off)");
    }

    bool HasSpecs(uint8 classId) { return s_classSpecs.count(classId) > 0; }

    Spec const* SpecOf(Player* bot)
    {
        if (!s_enabled || !bot)
            return nullptr;
        auto cls = s_classSpecs.find(bot->getClass());
        if (cls == s_classSpecs.end() || cls->second.empty())
            return nullptr;
        uint32 const guid = bot->GetGUID().GetCounter();
        {
            std::shared_lock<std::shared_mutex> guard(s_botSpecLock);
            auto stored = s_botSpec.find(guid);
            if (stored != s_botSpec.end())
            {
                auto it = s_specs.find(stored->second);
                if (it != s_specs.end() && it->second.classId == bot->getClass())
                    return &it->second;
            }
        }
        // A damage spec of the class, by weight, fixed for the bot's life (ef_bot_character).
        uint32 total = 0;
        for (uint32 id : cls->second)
            total += s_specs.at(id).weight;
        uint32 roll = urand(1, std::max<uint32>(1, total));
        uint32 chosen = cls->second.front();
        for (uint32 id : cls->second)
        {
            uint32 const w = s_specs.at(id).weight;
            if (roll <= w)
            {
                chosen = id;
                break;
            }
            roll -= w;
        }
        {
            std::unique_lock<std::shared_mutex> guard(s_botSpecLock);
            s_botSpec[guid] = chosen;
        }
        PlayerbotsDatabase.Execute("REPLACE INTO ef_bot_character (guid, spec_id) VALUES ({}, {})", guid, chosen);
        return &s_specs.at(chosen);
    }

    void ApplyTalents(Player* bot)
    {
#ifdef EF_BOTS_HAVE_TALENTS
        Spec const* spec = SpecOf(bot);
        if (!spec)
            return;
        std::string out;
        int32 const bought = Emberfall::Talents::BotBuild(bot, spec->id, spec->talents, out);
        if (bought < 0)
            LOG_DEBUG("playerbots", "Emberfall bots: {}", out);
#else
        (void)bot;
#endif
    }

    std::string TakeName(uint8 race, uint8 gender)
    {
        std::lock_guard<std::mutex> guard(s_namesLock);
        auto it = s_names.find({race, gender});
        if (it == s_names.end())
            return "";
        std::vector<std::string>& pool = it->second;
        while (!pool.empty())
        {
            uint32 const i = urand(0, pool.size() - 1);
            std::string name = pool[i];
            std::swap(pool[i], pool.back());
            pool.pop_back();
            if (ObjectMgr::CheckPlayerName(name) != CHAR_NAME_SUCCESS)
                continue;
            CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHECK_NAME);
            stmt->SetData(0, name);
            if (CharacterDatabase.Query(stmt))
                continue;
            return name;
        }
        return "";
    }

    int8 CanEquipWeapon(Player* bot, ItemTemplate const* proto)
    {
        Spec const* spec = SpecOf(bot);
        if (!spec || !proto || proto->Class != ITEM_CLASS_WEAPON)
            return -1;
        if (spec->weapons.empty())
            return -1;
        uint32 const sub = proto->SubClass;
        if (spec->weapons.count(sub))
            return 1;
        // an off-hand weapon (dual wield) only if the spec fights with two
        if (spec->offhand.count(sub) && proto->InventoryType != INVTYPE_2HWEAPON)
            return 1;
        return 0;
    }

    int8 CanEquipShield(Player* bot)
    {
        Spec const* spec = SpecOf(bot);
        if (!spec)
            return -1;
        return spec->shield ? 1 : 0;
    }
}
