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

// Emberfall profession specialisations (Emberfall docs/design/professions.md §10). See EfProfSpecs.h.
//
// EFP protocol (addon whispers to oneself, like the talents' EFT; body `<ver>;<seq>;<cmd>;<fields>`, ver 1):
//   client -> server   HELLO;<data hash>                    -> STATE per known profession with trees, OK (ERR DATA)
//                      APPLY;<skill>;<t1>.<t2>.<t3>         -> STATE, OK; or ERR <code>;<detail> and nothing changes
//   server -> client   STATE;<skill>;<earned>;<spent>;<ranks>   (sent after HELLO, APPLY, a reset, a skill set/unset)
//                      GONE;<skill>                          the profession was unlearned: its ranks are gone
//                      RESET;<skill>;<tree>;<points>;<copper>  a tree was reset at the trainer
//                      NODES;<scan>;<part>;<total>;<lock,x,y,skill|...>   tracked nodes beyond sight (minimap range)
//                      OK;<seq>  ERR;<seq>;<code>;<detail>
// Error codes: DATA (hash), DECODE, SKILL (no such profession or not learned), RANK, PATH, GATE (TalentRules.h),
// BUDGET (more than skill / points_per_skill), LOWER (a stored rank would go down), THROTTLE.

#include "EfProfSpecs.h"
#include "EfProfModel.h"
#include "../talents/EftProtocol.h"

#include "CellImpl.h"
#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DataMap.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GossipDef.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Item.h"
#include "Log.h"
#include "LootMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Random.h"
#include "ScriptedGossip.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <chrono>
#include <memory>

using namespace ef::prof;
namespace eft = ef::eft;

namespace Emberfall::ProfSpecs
{
    namespace
    {
        constexpr std::string_view kPrefix = "EFP\t";
        constexpr char const* kStateKey = "emberfall_profspecs";
        constexpr uint32 kGossipSender = 0x45465052; // "EFPR": our reset lines in a trainer's menu
        constexpr uint32 kScanMs = 3000;              // far nodes for the minimap
        constexpr uint32 kSkillHerbalism = 182, kSkillMining = 186;
        constexpr uint32 kLockHerbalism = 2, kLockMining = 3;

        bool s_enabled = true;
        std::shared_ptr<Model const> s_model;
        uint32 s_generation = 0;
        // skill_discovery_template, keyed as the core keys it: the recipe spell (> 0) or -skill line.
        std::map<int32, std::vector<std::pair<uint32, uint32>>> s_discoveries; // key -> (spell, required skill)

        uint64 NowMs()
        {
            return static_cast<uint64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        class PlayerState : public DataMap::Base
        {
        public:
            bool loaded = false;
            uint32 generation = 0;
            Ranks ranks;
            std::set<uint32> granted;
            std::map<ObjectGuid, uint32> lootSeen; // node -> its loot's generation time (the yield roll once per loot)
            eft::RateLimit limit{20, 10000};
            uint32 scanTimer = 0, scanId = 0;
            std::string lastNodes;
        };

        uint32 SkillOfLock(uint32 lockType) { return lockType == kLockHerbalism ? kSkillHerbalism : lockType == kLockMining ? kSkillMining : 0; }

        // The gathering lock of a node: its lock type (2 herbalism, 3 mining; 0: none) and the skill it needs.
        uint32 GatherLock(GameObject const* go, uint32& required)
        {
            required = 0;
            if (!go || go->GetGoType() != GAMEOBJECT_TYPE_CHEST)
                return 0;
            LockEntry const* lock = sLockStore.LookupEntry(go->GetGOInfo()->GetLockId());
            if (!lock)
                return 0;
            for (uint32 i = 0; i < MAX_LOCK_CASE; ++i)
                if (lock->Type[i] == LOCK_KEY_SKILL && (lock->Index[i] == kLockHerbalism || lock->Index[i] == kLockMining))
                {
                    required = lock->Skill[i];
                    return lock->Index[i];
                }
            return 0;
        }

        void SendBody(Player* player, std::string const& body)
        {
            std::string const text = std::string(kPrefix) + body;
            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, text);
            player->SendDirectMessage(&data);
        }
        void SendOk(Player* player, uint32 seq) { SendBody(player, eft::body(0, "OK", {std::to_string(seq)})); }
        void SendErr(Player* player, uint32 seq, std::string const& code, std::string const& detail = {})
        {
            SendBody(player, eft::body(0, "ERR", {std::to_string(seq), code, detail}));
        }

        void SendState(Player* player, PlayerState const& st, Model const& m, uint32 skill)
        {
            uint32 const value = player->HasSkill(skill) ? player->GetPureSkillValue(skill) : 0;
            SendBody(player, eft::body(0, "STATE", {std::to_string(skill), std::to_string(m.Budget(value)),
                std::to_string(m.Spent(skill, st.ranks)), m.Encode(skill, st.ranks)}));
        }

        void SaveProfession(CharacterDatabaseTransaction trans, uint32 guid, Model const& m, uint32 skill, Ranks const& ranks)
        {
            std::string nodes;
            for (Tree const* t : m.TreesOf(skill))
                for (Node const& n : t->nodes)
                    nodes += (nodes.empty() ? "" : ",") + std::to_string(n.id);
            if (!nodes.empty())
                trans->Append("DELETE FROM `ef_character_prof_rank` WHERE `guid` = {} AND `node` IN ({})", guid, nodes);
            for (auto const& [node, rank] : ranks)
                if (m.SkillOfNode(node) == skill && rank)
                    trans->Append("INSERT INTO `ef_character_prof_rank` (`guid`, `node`, `rank`) VALUES ({}, {}, {})", guid, node, rank);
        }

        void Commit(CharacterDatabaseTransaction trans)
        {
            if (trans->GetSize())
                CharacterDatabase.CommitTransaction(trans);
        }

        // The rank spells the ranks teach, learned and unlearned to match (only spells the module taught are taken).
        void Reconcile(Player* player, PlayerState& st, Model const& m, CharacterDatabaseTransaction trans)
        {
            uint32 const guid = player->GetGUID().GetCounter();
            std::set<uint32> const expected = m.Expected(st.ranks);
            for (auto it = st.granted.begin(); it != st.granted.end();)
            {
                if (expected.count(*it))
                {
                    ++it;
                    continue;
                }
                player->removeSpell(*it, SPEC_MASK_ALL, false);
                player->RemoveAurasDueToSpell(*it);
                trans->Append("DELETE FROM `ef_character_prof_granted` WHERE `guid` = {} AND `spell` = {}", guid, *it);
                it = st.granted.erase(it);
            }
            for (uint32 spell : expected)
            {
                if (st.granted.count(spell) || !sSpellMgr->GetSpellInfo(spell))
                    continue;
                if (!player->HasSpell(spell))
                    player->learnSpell(spell);
                if (player->HasSpell(spell))
                {
                    st.granted.insert(spell);
                    trans->Append("REPLACE INTO `ef_character_prof_granted` (`guid`, `spell`) VALUES ({}, {})", guid, spell);
                }
            }
        }

        // Ranks that no longer hold (a profession unlearned, a skill lowered by a GM, a data change) are dropped, a
        // profession at a time.
        bool Revalidate(Player* player, PlayerState& st, Model const& m)
        {
            bool changed = m.Remap(st.ranks);
            for (uint32 skill : m.Skills())
            {
                if (!m.Spent(skill, st.ranks))
                    continue;
                bool drop = !player->HasSkill(skill);
                if (!drop)
                {
                    Check const c = m.Validate(skill, st.ranks, player->GetPureSkillValue(skill));
                    if (!c.ok())
                    {
                        LOG_INFO("module", "Emberfall profspecs: {} skill {}: stored ranks fail ({} {}), reset", player->GetName(), skill,
                            c.code, c.detail);
                        drop = true;
                    }
                }
                if (drop)
                {
                    for (Tree const* t : m.TreesOf(skill))
                        m.WipeTree(t->id, st.ranks);
                    changed = true;
                }
            }
            return changed;
        }

        void LoadState(Player* player, PlayerState& st, Model const& m)
        {
            uint32 const guid = player->GetGUID().GetCounter();
            st.ranks.clear();
            st.granted.clear();
            if (QueryResult r = CharacterDatabase.Query("SELECT `node`, `rank` FROM `ef_character_prof_rank` WHERE `guid` = {}", guid))
                do
                {
                    Field* f = r->Fetch();
                    st.ranks[f[0].Get<uint32>()] = f[1].Get<uint32>();
                } while (r->NextRow());
            if (QueryResult r = CharacterDatabase.Query("SELECT `spell` FROM `ef_character_prof_granted` WHERE `guid` = {}", guid))
                do
                    st.granted.insert(r->Fetch()[0].Get<uint32>());
                while (r->NextRow());
            st.loaded = true;
            st.generation = s_generation;
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            if (Revalidate(player, st, m))
                for (uint32 skill : m.Skills())
                    SaveProfession(trans, guid, m, skill, st.ranks);
            Reconcile(player, st, m, trans);
            Commit(trans);
        }

        PlayerState* State(Player* player)
        {
            if (!s_enabled || !s_model)
                return nullptr;
            PlayerState* st = player->CustomData.GetDefault<PlayerState>(kStateKey);
            if (!st->loaded || st->generation != s_generation)
                LoadState(player, *st, *s_model);
            return st;
        }

        void GiveItems(Player* player, uint32 item, uint32 count)
        {
            ItemPosCountVec dest;
            if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, item, count) != EQUIP_ERR_OK)
                return;
            if (Item* it = player->StoreNewItem(dest, item, true))
                player->SendNewItem(it, count, false, true);
        }

        // The far nodes (Far Sight, professions.md §10.5): tracked herbs or ore between the player's sight and the
        // ranks' extra yards, sent when the list changed.
        void Scan(Player* player, PlayerState& st, Model const& m)
        {
            uint32 const tracked = player->GetUInt32Value(PLAYER_TRACK_RESOURCES);
            float const sight = player->GetSightRange();
            float reach = 0.0f;
            uint32 locks = 0;
            uint32 bonus[4] = {};
            for (uint32 lock : {kLockHerbalism, kLockMining})
            {
                if (!(tracked & (1u << (lock - 1))))
                    continue;
                bonus[lock] = m.Sum(st.ranks, Effect::MinimapRange, SkillOfLock(lock));
                if (bonus[lock])
                {
                    locks |= 1u << lock;
                    reach = std::max(reach, sight + static_cast<float>(bonus[lock]));
                }
            }
            std::string entries;
            if (locks)
            {
                std::list<GameObject*> found;
                Acore::GameObjectInRangeCheck check(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), reach);
                Acore::GameObjectListSearcher<Acore::GameObjectInRangeCheck> searcher(player, found, check);
                Cell::VisitObjects(player, searcher, reach);
                std::vector<std::tuple<float, std::string>> list;
                for (GameObject* go : found)
                {
                    uint32 required = 0;
                    uint32 const lock = GatherLock(go, required);
                    if (!lock || !(locks & (1u << lock)) || !go->isSpawned() || go->getLootState() != GO_READY)
                        continue;
                    float const d = player->GetExactDist2d(go);
                    // what the client sees already it draws itself; the rest out to the ranks' extra yards
                    if (player->HaveAtClient(go) || d > sight + static_cast<float>(bonus[lock]))
                        continue;
                    list.emplace_back(d, std::to_string(lock) + "," + std::to_string(static_cast<int32>(go->GetPositionX())) + "," +
                        std::to_string(static_cast<int32>(go->GetPositionY())) + "," + std::to_string(required));
                }
                std::sort(list.begin(), list.end());
                for (auto const& [d, e] : list)
                    if (entries.size() + e.size() < 1200) // at most 8 messages
                        entries += (entries.empty() ? "" : "|") + e;
            }
            if (entries == st.lastNodes)
                return;
            st.lastNodes = entries;
            ++st.scanId;
            // split into bodies of at most 251 bytes
            std::vector<std::string> parts;
            std::string cur;
            size_t pos = 0;
            while (pos <= entries.size())
            {
                size_t const next = entries.find('|', pos);
                std::string const e = entries.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
                if (!cur.empty() && cur.size() + e.size() + 1 > 200)
                {
                    parts.push_back(cur);
                    cur.clear();
                }
                cur += (cur.empty() ? "" : "|") + e;
                if (next == std::string::npos)
                    break;
                pos = next + 1;
            }
            parts.push_back(cur);
            for (size_t i = 0; i < parts.size(); ++i)
                SendBody(player, eft::body(0, "NODES", {std::to_string(st.scanId), std::to_string(i + 1), std::to_string(parts.size()), parts[i]}));
        }
    }

    bool Enabled() { return s_enabled && s_model != nullptr; }

    void ReadConfig(bool reload)
    {
        bool const was = s_enabled;
        s_enabled = sConfigMgr->GetOption<bool>("Emberfall.ProfSpecs.Enable", true, false);
        if (reload && was != s_enabled)
            Load();
    }

    bool Load()
    {
        ++s_generation;
        s_model.reset();
        s_discoveries.clear();
        if (!s_enabled)
        {
            LOG_INFO("server.loading", ">> Emberfall profession specialisations: off (Emberfall.ProfSpecs.Enable = 0)");
            return false;
        }
        uint32 found = 0;
        if (QueryResult r = WorldDatabase.Query("SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME IN "
            "('ef_prof_meta', 'ef_prof_tree', 'ef_prof_node', 'ef_prof_node_spell', 'ef_prof_edge', 'ef_prof_node_target', 'ef_prof_recipe', "
            "'ef_prof_trainer')"))
            found = static_cast<uint32>(r->Fetch()[0].Get<uint64>());
        if (found != 8)
        {
            LOG_ERROR("server.loading", ">> Emberfall profession specialisations: {} of the 8 ef_prof_* world tables exist "
                "(modules/mod-emberfall/data/sql/db-world); the system stays off", found);
            return false;
        }
        auto m = std::make_shared<Model>();
        if (QueryResult r = WorldDatabase.Query("SELECT `name`, `value` FROM `ef_prof_meta`"))
            do
            {
                Field* f = r->Fetch();
                std::string const name = f[0].Get<std::string>(), value = f[1].Get<std::string>();
                if (name == "data_hash")
                    m->hash = value;
                else if (name == "points_per_skill")
                    m->pointsPerSkill = static_cast<uint32>(std::stoul(value));
                else if (name == "reset_fee_per_point")
                    m->feePerPoint = static_cast<uint32>(std::stoul(value));
            } while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query("SELECT `id`, `skill_line`, `sort`, `name` FROM `ef_prof_tree`"))
            do
            {
                Field* f = r->Fetch();
                Tree t;
                t.id = f[0].Get<uint32>();
                t.skill = f[1].Get<uint32>();
                t.sort = f[2].Get<uint32>();
                t.name = f[3].Get<std::string>();
                m->trees[t.id] = t;
            } while (r->NextRow());
        if (m->trees.empty() || m->hash.empty())
        {
            LOG_INFO("server.loading", ">> Emberfall profession specialisations: no trees in ef_prof_tree (the system stays off)");
            return false;
        }
        std::map<uint32, Node> nodes;
        if (QueryResult r = WorldDatabase.Query("SELECT `id`, `tree`, `ord`, `row`, `max_rank`, `gate`, `effect`, `per_rank`, `extra`, `active` FROM `ef_prof_node`"))
            do
            {
                Field* f = r->Fetch();
                Node n;
                n.id = f[0].Get<uint32>();
                n.tree = f[1].Get<uint32>();
                n.ord = f[2].Get<uint32>();
                n.row = f[3].Get<uint32>();
                n.maxRank = f[4].Get<uint32>();
                n.gate = f[5].Get<uint32>();
                n.effect = static_cast<Effect>(f[6].Get<uint32>());
                n.perRank = f[7].Get<uint32>();
                n.extra = f[8].Get<uint32>();
                n.active = f[9].Get<uint32>() != 0;
                nodes[n.id] = n;
            } while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query("SELECT `node`, `rank`, `spell` FROM `ef_prof_node_spell` ORDER BY `node`, `rank`"))
            do
            {
                Field* f = r->Fetch();
                if (auto it = nodes.find(f[0].Get<uint32>()); it != nodes.end())
                    it->second.spells.push_back(f[2].Get<uint32>());
            } while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query("SELECT `parent`, `child` FROM `ef_prof_edge` ORDER BY `child`, `parent`"))
            do
            {
                Field* f = r->Fetch();
                if (auto it = nodes.find(f[1].Get<uint32>()); it != nodes.end())
                    it->second.parents.push_back(f[0].Get<uint32>());
            } while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query("SELECT `node`, `kind`, `value` FROM `ef_prof_node_target`"))
            do
            {
                Field* f = r->Fetch();
                auto it = nodes.find(f[0].Get<uint32>());
                if (it == nodes.end())
                    continue;
                uint32 const kind = f[1].Get<uint32>(), value = f[2].Get<uint32>();
                (kind == kTargetItem ? it->second.items : kind == kTargetFamily ? it->second.families : it->second.targetSpells).insert(value);
            } while (r->NextRow());
        std::vector<std::string> problems;
        for (auto& [id, n] : nodes)
        {
            auto t = m->trees.find(n.tree);
            if (t == m->trees.end())
            {
                problems.push_back("node " + std::to_string(id) + ": unknown tree " + std::to_string(n.tree));
                continue;
            }
            for (uint32 spell : n.spells)
                if (!sSpellMgr->GetSpellInfo(spell))
                    problems.push_back("node " + std::to_string(id) + ": rank spell " + std::to_string(spell) + " is not in Spell.dbc");
            t->second.nodes.push_back(n);
        }
        if (QueryResult r = WorldDatabase.Query("SELECT `spell`, `skill_line`, `family`, `item` FROM `ef_prof_recipe`"))
            do
            {
                Field* f = r->Fetch();
                m->recipes[f[0].Get<uint32>()] = {f[1].Get<uint32>(), f[2].Get<uint32>(), f[3].Get<uint32>()};
            } while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query("SELECT `creature`, `skill_line` FROM `ef_prof_trainer`"))
            do
            {
                Field* f = r->Fetch();
                m->trainers[f[0].Get<uint32>()] = f[1].Get<uint32>();
            } while (r->NextRow());
        m->Index();
        for (std::string const& p : m->Problems())
            problems.push_back(p);
        if (!problems.empty())
        {
            for (std::string const& p : problems)
                LOG_ERROR("server.loading", ">> Emberfall profession specialisations: {}", p);
            LOG_ERROR("server.loading", ">> Emberfall profession specialisations: {} problem(s) in the ef_prof_* tables; the system stays off",
                problems.size());
            return false;
        }
        // The discoveries the Careful Reading's ranks roll again (the core keys reqSpell 0 by the discovered spell's line).
        if (QueryResult r = WorldDatabase.Query("SELECT `spellId`, `reqSpell`, `reqSkillValue` FROM `skill_discovery_template`"))
            do
            {
                Field* f = r->Fetch();
                uint32 const spell = f[0].Get<uint32>();
                int32 key = f[1].Get<int32>();
                if (key == 0)
                {
                    SkillLineAbilityMapBounds const b = sSpellMgr->GetSkillLineAbilityMapBounds(spell);
                    if (b.first == b.second)
                        continue;
                    key = -static_cast<int32>(b.first->second->SkillLine);
                }
                s_discoveries[key].emplace_back(spell, f[2].Get<uint32>());
            } while (r->NextRow());
        size_t nodeCount = 0;
        for (auto const& [id, t] : m->trees)
            nodeCount += t.nodes.size();
        LOG_INFO("server.loading", ">> Emberfall profession specialisations: {} trees, {} nodes, {} recipes, {} trainers (data {})",
            m->trees.size(), nodeCount, m->recipes.size(), m->trainers.size(), m->hash);
        s_model = m;
        return true;
    }

    void OnLogin(Player* player)
    {
        State(player);
    }

    void HandleMessage(Player* player, std::string_view body)
    {
        std::shared_ptr<Model const> const m = s_model;
        PlayerState* st = State(player);
        eft::ParseResult const p = eft::parse(body);
        if (!m || !st)
        {
            SendErr(player, p.seq, "DISABLED");
            return;
        }
        if (!p.ok())
        {
            SendErr(player, p.seq, p.error == eft::ParseError::Version ? "VERSION" : "DECODE");
            return;
        }
        if (!st->limit.allow(NowMs()))
        {
            SendErr(player, p.request.seq, "THROTTLE");
            return;
        }
        eft::Request const& req = p.request;
        if (req.cmd == "HELLO")
        {
            if (req.fields.empty() || req.fields[0] != m->hash)
            {
                SendErr(player, req.seq, "DATA", m->hash);
                return;
            }
            for (uint32 skill : m->Skills())
                if (player->HasSkill(skill))
                    SendState(player, *st, *m, skill);
            SendOk(player, req.seq);
            return;
        }
        if (req.cmd == "APPLY")
        {
            auto const skill = req.fields.size() >= 2 ? eft::number(req.fields[0], 100000) : std::nullopt;
            if (!skill)
            {
                SendErr(player, req.seq, "DECODE");
                return;
            }
            if (!m->HasSkill(*skill) || !player->HasSkill(*skill))
            {
                SendErr(player, req.seq, "SKILL", std::to_string(*skill));
                return;
            }
            std::optional<Ranks> ranks = m->Decode(*skill, req.fields[1]);
            if (!ranks)
            {
                SendErr(player, req.seq, "DECODE");
                return;
            }
            Check const c = m->Validate(*skill, *ranks, player->GetPureSkillValue(*skill), &st->ranks);
            if (!c.ok())
            {
                SendErr(player, req.seq, c.code, std::to_string(c.detail));
                return;
            }
            for (Tree const* t : m->TreesOf(*skill))
                m->WipeTree(t->id, st->ranks);
            for (auto const& [node, rank] : *ranks)
                st->ranks[node] = rank;
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            SaveProfession(trans, player->GetGUID().GetCounter(), *m, *skill, st->ranks);
            Reconcile(player, *st, *m, trans);
            Commit(trans);
            st->lastNodes = "?"; // the next scan sends the far nodes for the new ranks
            LOG_INFO("module", "Emberfall profspecs: {} skill {} -> {}", player->GetName(), *skill, m->Encode(*skill, st->ranks));
            SendState(player, *st, *m, *skill);
            SendOk(player, req.seq);
            return;
        }
        // unknown commands get no answer (as the talents' EFT)
    }

    void OnSetSkill(Player* player, uint32 skill, uint32 newValue)
    {
        std::shared_ptr<Model const> const m = s_model;
        if (!m || !m->HasSkill(skill))
            return;
        PlayerState* st = State(player);
        if (!st)
            return;
        if (newValue == 0)
        {
            if (m->Spent(skill, st->ranks))
            {
                for (Tree const* t : m->TreesOf(skill))
                    m->WipeTree(t->id, st->ranks);
                CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
                SaveProfession(trans, player->GetGUID().GetCounter(), *m, skill, st->ranks);
                Reconcile(player, *st, *m, trans);
                Commit(trans);
            }
            SendBody(player, eft::body(0, "GONE", {std::to_string(skill)}));
            return;
        }
        SendState(player, *st, *m, skill);
    }

    void OnBeforeSendLoot(Player* player, ObjectGuid lootGuid, Loot* loot)
    {
        std::shared_ptr<Model const> const m = s_model;
        if (!m || !loot || !lootGuid.IsGameObject())
            return;
        PlayerState* st = State(player);
        if (!st || st->ranks.empty())
            return;
        GameObject* go = ObjectAccessor::GetGameObject(*player, lootGuid);
        uint32 required = 0;
        uint32 const skill = SkillOfLock(GatherLock(go, required));
        if (!skill)
            return;
        uint32 const generation = go->GetLootGenerationTime();
        auto const seen = st->lootSeen.find(lootGuid);
        if (seen != st->lootSeen.end() && seen->second == generation)
            return; // this loot was rolled for already (the window opened again)
        st->lootSeen[lootGuid] = generation;
        if (st->lootSeen.size() > 256)
            st->lootSeen.erase(st->lootSeen.begin());
        for (LootItem& li : loot->items)
        {
            if (li.is_looted)
                continue;
            uint32 const chance = m->Sum(st->ranks, Effect::Yield, skill, [&](Node const& n) { return n.items.count(li.itemid) != 0; });
            if (!chance)
                continue;
            bool const hit = roll_chance_i(static_cast<int32>(std::min<uint32>(chance, 100)));
            LOG_INFO("module", "Emberfall profspecs: {} yield roll on {} ({}%): {}", player->GetName(), li.itemid, chance, hit ? "one more" : "no");
            if (hit && li.count < 255)
                ++li.count;
        }
    }

    void OnCraft(Player* player, SkillLineAbilityEntry const* ability)
    {
        std::shared_ptr<Model const> const m = s_model;
        if (!m || !ability)
            return;
        auto const recipe = m->recipes.find(ability->Spell);
        if (recipe == m->recipes.end())
            return;
        PlayerState* st = State(player);
        if (!st || st->ranks.empty())
            return;
        Model::Recipe const& r = recipe->second;
        uint32 const extra = m->Sum(st->ranks, Effect::CraftExtra, r.skill, [&](Node const& n) { return n.families.count(r.family) != 0; });
        if (extra && roll_chance_i(static_cast<int32>(std::min<uint32>(extra, 100))))
        {
            LOG_INFO("module", "Emberfall profspecs: {} steady hand on {} ({}%): a second item", player->GetName(), ability->Spell, extra);
            GiveItems(player, r.item, 1);
        }
        uint32 const batch = m->Sum(st->ranks, Effect::CraftBatch, r.skill);
        if (batch && roll_chance_i(static_cast<int32>(std::min<uint32>(batch, 100))))
        {
            uint32 more = 0;
            for (auto const& [node, rank] : st->ranks)
                if (Node const* n = m->NodeById(node); n && n->effect == Effect::CraftBatch && n->active)
                    more = std::max(more, n->extra);
            LOG_INFO("module", "Emberfall profspecs: {} double batch on {} ({}%): {} more", player->GetName(), ability->Spell, batch, more);
            if (more)
                GiveItems(player, r.item, more);
        }
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(ability->Spell);
        uint32 const bonus = m->Sum(st->ranks, Effect::Discovery, r.skill);
        if (spell && spell->Mechanic == MECHANIC_DISCOVERY && bonus)
        {
            bool const hit = roll_chance_i(static_cast<int32>(std::min<uint32>(bonus, 100)));
            uint32 const value = player->GetPureSkillValue(ability->SkillLine);
            std::vector<uint32> open;
            for (int32 key : {static_cast<int32>(ability->Spell), -static_cast<int32>(ability->SkillLine)})
                if (auto it = s_discoveries.find(key); it != s_discoveries.end())
                    for (auto const& [discovered, needs] : it->second)
                        if (needs <= value && !player->HasSpell(discovered))
                            open.push_back(discovered);
            LOG_INFO("module", "Emberfall profspecs: {} careful reading on {} ({}%, {} open): {}", player->GetName(), ability->Spell, bonus,
                open.size(), hit && !open.empty() ? "a discovery" : "no");
            if (hit && !open.empty())
                player->learnSpell(open[urand(0, static_cast<uint32>(open.size() - 1))]);
        }
    }

    void OnUpdate(Player* player, uint32 diff)
    {
        if (!s_model || !s_enabled)
            return;
        PlayerState* st = player->CustomData.Get<PlayerState>(kStateKey);
        if (!st || !st->loaded || (st->ranks.empty() && st->lastNodes.empty()))
            return;
        st->scanTimer += diff;
        if (st->scanTimer < kScanMs)
            return;
        st->scanTimer = 0;
        if (player->IsInWorld() && player->GetMap())
            Scan(player, *st, *s_model);
    }

    bool OnGossipHello(Player* player, Creature* creature)
    {
        std::shared_ptr<Model const> const m = s_model;
        if (!m || !creature)
            return false;
        auto const trainer = m->trainers.find(creature->GetEntry());
        if (trainer == m->trainers.end())
            return false;
        PlayerState* st = State(player);
        if (!st || !m->Spent(trainer->second, st->ranks))
            return false;
        player->PrepareGossipMenu(creature, creature->GetGossipMenuId(), true);
        for (Tree const* t : m->TreesOf(trainer->second))
        {
            uint32 const spent = m->SpentInTree(t->id, st->ranks);
            if (!spent)
                continue;
            uint32 const fee = spent * m->feePerPoint;
            std::string const text = "Unlearn " + t->name + " (" + std::to_string(spent) + (spent == 1 ? " point)" : " points)");
            std::string const box = "This takes back all " + std::to_string(spent) + " points spent in " + t->name +
                ". The other trees keep theirs; a tree is unlearned whole, never a node.";
            player->PlayerTalkClass->GetGossipMenu().AddMenuItem(-1, GOSSIP_ICON_TRAINER, text, kGossipSender, t->id, box, fee, false);
        }
        player->SendPreparedGossip(creature);
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action)
    {
        if (sender != kGossipSender)
            return false;
        std::shared_ptr<Model const> const m = s_model;
        PlayerState* st = State(player);
        CloseGossipMenuFor(player);
        if (!m || !st || !creature)
            return true;
        auto const trainer = m->trainers.find(creature->GetEntry());
        auto const tree = m->trees.find(action);
        if (trainer == m->trainers.end() || tree == m->trees.end() || tree->second.skill != trainer->second)
            return true;
        uint32 const spent = m->SpentInTree(action, st->ranks);
        uint32 const fee = spent * m->feePerPoint;
        if (!spent)
            return true;
        if (!player->HasEnoughMoney(fee))
        {
            player->SendBuyError(BUY_ERR_NOT_ENOUGHT_MONEY, creature, 0, 0);
            return true;
        }
        player->ModifyMoney(-static_cast<int32>(fee));
        m->WipeTree(action, st->ranks);
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        SaveProfession(trans, player->GetGUID().GetCounter(), *m, trainer->second, st->ranks);
        Reconcile(player, *st, *m, trans);
        Commit(trans);
        st->lastNodes = "?";
        LOG_INFO("module", "Emberfall profspecs: {} reset tree {} ({} points, {} copper)", player->GetName(), action, spent, fee);
        SendBody(player, eft::body(0, "RESET", {std::to_string(trainer->second), std::to_string(action), std::to_string(spent), std::to_string(fee)}));
        SendState(player, *st, *m, trainer->second);
        return true;
    }

    void Dump(Player* player, std::vector<std::string>& out)
    {
        std::shared_ptr<Model const> const m = s_model;
        PlayerState* st = State(player);
        if (!m || !st)
        {
            out.push_back("Emberfall profession specialisations: off.");
            return;
        }
        out.push_back("Profession specialisations of " + player->GetName() + " (data " + m->hash + ", sight " +
            std::to_string(static_cast<int32>(player->GetSightRange())) + " yd, far nodes sent " +
            std::to_string(st->lastNodes.empty() ? 0 : std::count(st->lastNodes.begin(), st->lastNodes.end(), '|') + 1) + "):");
        for (uint32 skill : m->Skills())
        {
            uint32 const value = player->HasSkill(skill) ? player->GetPureSkillValue(skill) : 0;
            out.push_back("  skill " + std::to_string(skill) + (player->HasSkill(skill) ? "" : " (not learned)") + ": " + std::to_string(value) +
                ", points " + std::to_string(m->Spent(skill, st->ranks)) + "/" + std::to_string(m->Budget(value)) + ", ranks " + m->Encode(skill, st->ranks));
        }
        out.push_back("  effects: yield " + std::to_string(m->Sum(st->ranks, Effect::Yield)) + ", minimap range " +
            std::to_string(m->Sum(st->ranks, Effect::MinimapRange)) + ", rarest first " + std::to_string(m->Sum(st->ranks, Effect::RarestFirst)) +
            ", gather speed " + std::to_string(m->Sum(st->ranks, Effect::GatherSpeed)) + ", craft extra " +
            std::to_string(m->Sum(st->ranks, Effect::CraftExtra)) + ", duration " + std::to_string(m->Sum(st->ranks, Effect::Duration)) +
            ", discovery " + std::to_string(m->Sum(st->ranks, Effect::Discovery)));
        // the gather spells the speed ranks modify, with the cast time the core gives them for this player
        std::set<uint32> gather;
        for (auto const& [id, t] : m->trees)
            for (Node const& n : t.nodes)
                if (n.effect == Effect::GatherSpeed)
                    gather.insert(n.targetSpells.begin(), n.targetSpells.end());
        std::string casts;
        for (uint32 spell : gather)
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spell))
                casts += (casts.empty() ? "" : ", ") + std::to_string(spell) + " " + std::to_string(info->CalcCastTime(player)) + " ms";
        out.push_back("  gather casts: " + (casts.empty() ? std::string("none") : casts));
        std::string spells;
        for (uint32 s : st->granted)
            spells += (spells.empty() ? "" : ",") + std::to_string(s);
        out.push_back("  rank spells: " + (spells.empty() ? std::string("none") : spells));
    }

    bool GmReset(Player* player, uint32 skill, std::string& out)
    {
        std::shared_ptr<Model const> const m = s_model;
        PlayerState* st = State(player);
        if (!m || !st)
        {
            out = "Emberfall profession specialisations: off.";
            return false;
        }
        for (uint32 s : m->Skills())
            if (!skill || s == skill)
            {
                for (Tree const* t : m->TreesOf(s))
                    m->WipeTree(t->id, st->ranks);
                CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
                SaveProfession(trans, player->GetGUID().GetCounter(), *m, s, st->ranks);
                Reconcile(player, *st, *m, trans);
                Commit(trans);
                if (player->HasSkill(s))
                    SendState(player, *st, *m, s);
            }
        out = "Profession specialisations reset" + (skill ? " for skill " + std::to_string(skill) : std::string()) + ".";
        return true;
    }
}
