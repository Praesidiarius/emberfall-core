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
//                      FREE;<skill>;<points>                 the profession's trees were reset for free: the trees changed
//                                                            (the talents v2 migration) or the skill fell below what the
//                                                            spent points need (a specialisation given up, professions.md §28)
//                      NODES;<scan>;<part>;<total>;<lock,x,y,skill|...>   tracked nodes beyond sight (minimap range)
//                      OK;<seq>  ERR;<seq>;<code>;<detail>
// Error codes: DATA (hash), DECODE, SKILL (no such profession or not learned), RANK, PATH, GATE (TalentRules.h),
// BUDGET (more than skill / points_per_skill), LOWER (a stored rank would go down), THROTTLE.

#include "EfProfSpecs.h"
#include "EfProfModel.h"
#include "EfProfText.h"
#include "../talents/EftProtocol.h"
#include "../veins/EfVeinSwings.h"

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
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "ScriptedGossip.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <array>
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
        constexpr uint32 kPickSender = 0x45465053;   // "EFPS": pick a specialisation (action: its spell)
        constexpr uint32 kDropSender = 0x45465044;   // "EFPD": give the specialisation up (action: the craft's skill)
        // ef_character_migration: every character's talents reset once, free, when the trees shrank to rank 3's budget
        constexpr char const* kTalentsV2 = "prof_talents_v2";
        // ef_prof_text kinds: a tree's name, a specialisation's name and its text (what it makes)
        constexpr uint32 kTextTree = 1, kTextSpecName = 2, kTextSpecWhat = 3;
        constexpr uint32 kScanMs = 3000;              // far nodes for the minimap
        constexpr uint32 kSkillHerbalism = 182, kSkillMining = 186;
        constexpr uint32 kLockHerbalism = 2, kLockMining = 3;

        bool s_enabled = true;
        std::shared_ptr<Model const> s_model;
        uint32 s_generation = 0;
        // skill_discovery_template, keyed as the core keys it: the recipe spell (> 0) or -skill line.
        std::map<int32, std::vector<std::pair<uint32, uint32>>> s_discoveries; // key -> (spell, required skill)
        // What the made items do when used (a bandage's heal, an antidote's cure): item spell -> the recipe family,
        // for potency and cleanse (feedback #204); and every spell a potency node names, so the damage and heal
        // hooks skip all others at once.
        std::map<uint32, uint32> s_itemSpellFamily;
        std::set<uint32> s_potencySpells;
        // ef_prof_text: (kind, id) -> enUS, frFR, deDE (the Lang order); an empty translation reads English
        std::map<std::pair<uint32, uint32>, std::array<std::string, 3>> s_texts;

        using text::Lang;
        Lang LangOf(Player* player)
        {
            return Emberfall::Professions::LangOf(player->GetSession() ? uint32(player->GetSession()->GetSessionDbcLocale()) : 0);
        }
        std::string Text(uint32 kind, uint32 id, Lang lang, std::string const& fallback)
        {
            auto const it = s_texts.find({kind, id});
            if (it == s_texts.end())
                return fallback;
            std::string const& t = it->second[static_cast<size_t>(lang)];
            return !t.empty() ? t : !it->second[0].empty() ? it->second[0] : fallback;
        }
        std::string CraftLabel(uint32 skill, Lang lang)
        {
            std::string name = Emberfall::Professions::CraftName(skill, lang);
            if (name.empty())
                if (SkillLineEntry const* line = sSkillLineStore.LookupEntry(skill))
                    name = line->name[0];
            return name;
        }

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
            // a loot source -> (its loot's generation, the loot type): the yield and upgrade rolls once per loot, not
            // again when the window opens again (a node's generation time, a corpse's death, a disenchanted item's own GUID)
            std::map<ObjectGuid, std::pair<uint32, uint8>> lootSeen;
            eft::RateLimit limit{20, 10000};
            uint32 scanTimer = 0, scanId = 0;
            std::string lastNodes;
            // a craft's specialisation (its spell, 0: none) and how often it was given up (the price ladder's step)
            struct Spec
            {
                uint32 spell = 0, unlearns = 0;
            };
            std::map<uint32, Spec> specs;   // skill -> its specialisation
            // free resets the client has not been told about yet (skill, points back): sent with the next HELLO
            std::vector<std::pair<uint32, uint32>> freeResets;
            uint64 lastSpecRefusal = 0;   // the last "needs the specialisation" line (one every few seconds)
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
            SendBody(player, eft::body(0, "STATE", {std::to_string(skill), std::to_string(m.Budget(skill, value)),
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

        void SaveSpec(CharacterDatabaseTransaction trans, uint32 guid, uint32 skill, PlayerState::Spec const& sp)
        {
            trans->Append("REPLACE INTO `ef_character_prof_spec` (`guid`, `skill_line`, `spell`, `unlearns`) VALUES ({}, {}, {}, {})", guid,
                skill, sp.spell, sp.unlearns);
        }

        // The specialisation spells match the stored pick: the active one known, the craft's others not (a spell
        // learned by hand or left from a pick undone is taken away; a craft not known has none).
        void ReconcileSpecs(Player* player, PlayerState& st, Model const& m)
        {
            for (auto const& [spell, sp] : m.specs)
            {
                auto const it = st.specs.find(sp.skill);
                bool const want = it != st.specs.end() && it->second.spell == spell && player->HasSkill(sp.skill);
                if (want && !player->HasSpell(spell))
                    player->learnSpell(spell);
                else if (!want && player->HasSpell(spell))
                {
                    player->removeSpell(spell, SPEC_MASK_ALL, false);
                    player->RemoveAurasDueToSpell(spell);
                }
            }
        }

        // Wipes a profession's trees for free and remembers to tell the client (FREE); the points it took back.
        uint32 FreeReset(PlayerState& st, Model const& m, uint32 skill)
        {
            uint32 back = 0;
            for (Tree const* t : m.TreesOf(skill))
                back += m.WipeTree(t->id, st.ranks);
            if (back)
                st.freeResets.emplace_back(skill, back);
            return back;
        }

        void SendFreeResets(Player* player, PlayerState& st)
        {
            for (auto const& [skill, points] : st.freeResets)
                SendBody(player, eft::body(0, "FREE", {std::to_string(skill), std::to_string(points)}));
            st.freeResets.clear();
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
                    Check const c = m.ValidateStored(skill, st.ranks, player->GetPureSkillValue(skill));
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
            st.specs.clear();
            if (QueryResult r = CharacterDatabase.Query("SELECT `skill_line`, `spell`, `unlearns` FROM `ef_character_prof_spec` WHERE `guid` = {}", guid))
                do
                {
                    Field* f = r->Fetch();
                    st.specs[f[0].Get<uint32>()] = {f[1].Get<uint32>(), f[2].Get<uint32>()};
                } while (r->NextRow());
            st.loaded = true;
            st.generation = s_generation;
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            // The talents v2 migration (professions.md §28): the trees shrank (35 -> 21 points, points from rank 3's
            // skill on), so no spend from before still fits; every profession's trees are reset once, free.
            bool changed = false;
            if (!CharacterDatabase.Query("SELECT 1 FROM `ef_character_migration` WHERE `guid` = {} AND `name` = '{}'", guid, kTalentsV2))
            {
                for (uint32 skill : m.Skills())
                    if (uint32 const back = FreeReset(st, m, skill))
                    {
                        changed = true;
                        LOG_INFO("module", "Emberfall profspecs: {} skill {}: talents v2, {} points back", player->GetName(), skill, back);
                    }
                trans->Append("INSERT IGNORE INTO `ef_character_migration` (`guid`, `name`) VALUES ({}, '{}')", guid, kTalentsV2);
            }
            if (Revalidate(player, st, m) || changed)
                for (uint32 skill : m.Skills())
                    SaveProfession(trans, guid, m, skill, st.ranks);
            Reconcile(player, st, m, trans);
            ReconcileSpecs(player, st, m);
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

        // The profession whose loot this is (0: none of ours) and the loot's generation: a herb or ore node's lock, a
        // catch (the bobber or the pool), a skinned beast (its loot skill), a looted corpse (0 skill: every profession's
        // corpse yields, the meat and cloth of Cooking, Tailoring and First Aid), a disenchanted item. `any` is set for
        // a corpse.
        uint32 LootSkill(Player* player, ObjectGuid lootGuid, Loot const* loot, uint32& generation, bool& any)
        {
            generation = 0;
            any = false;
            if (lootGuid.IsGameObject())
            {
                GameObject* go = ObjectAccessor::GetGameObject(*player, lootGuid);
                if (!go)
                    return 0;
                generation = go->GetLootGenerationTime();
                if (loot->loot_type == LOOT_FISHING)
                    return SKILL_FISHING;
                uint32 required = 0;
                return SkillOfLock(GatherLock(go, required));
            }
            if (lootGuid.IsCreatureOrVehicle())
            {
                Creature* creature = ObjectAccessor::GetCreature(*player, lootGuid);
                if (!creature)
                    return 0;
                generation = static_cast<uint32>(creature->GetRespawnTime());
                if (loot->loot_type == LOOT_SKINNING)
                    return creature->GetCreatureTemplate()->GetRequiredLootSkill();
                if (loot->loot_type == LOOT_CORPSE)
                {
                    any = true;
                    return 0;
                }
                return 0;
            }
            if (lootGuid.IsItem() && loot->loot_type == LOOT_DISENCHANTING)
                return SKILL_ENCHANTING;
            return 0;
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
        s_texts.clear();
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
                // per profession: `points_from:<skill>`, `points_per_skill:<skill>` (talents from rank 3, §28)
                else if (name.rfind("points_from:", 0) == 0)
                    m->pointsFrom[static_cast<uint32>(std::stoul(name.substr(12)))] = static_cast<uint32>(std::stoul(value));
                else if (name.rfind("points_per_skill:", 0) == 0)
                    m->stepOf[static_cast<uint32>(std::stoul(name.substr(17)))] = static_cast<uint32>(std::stoul(value));
                // the specialisations' unlearn prices in copper, comma-separated (the last one repeats)
                else if (name == "spec_unlearn_copper")
                {
                    size_t pos = 0;
                    while (pos < value.size())
                    {
                        size_t const end = value.find(',', pos);
                        m->unlearnCopper.push_back(static_cast<uint32>(std::stoul(value.substr(pos, end - pos))));
                        if (end == std::string::npos)
                            break;
                        pos = end + 1;
                    }
                }
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
        // The crafts' specialisations and the trainer lines' names (professions.md §28; the 2026-11-14 tables: a world
        // without them has no specialisations, the rest works)
        uint32 rework = 0;
        if (QueryResult r = WorldDatabase.Query("SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME IN "
            "('ef_prof_specialisation', 'ef_prof_text')"))
            rework = static_cast<uint32>(r->Fetch()[0].Get<uint64>());
        if (rework == 2)
        {
            if (QueryResult r = WorldDatabase.Query("SELECT `spell`, `skill_line`, `sort`, `back_to`, `needs_cap`, `key` FROM `ef_prof_specialisation`"))
                do
                {
                    Field* f = r->Fetch();
                    Specialisation sp;
                    sp.spell = f[0].Get<uint32>();
                    sp.skill = f[1].Get<uint32>();
                    sp.sort = f[2].Get<uint32>();
                    sp.backTo = f[3].Get<uint32>();
                    sp.needsCap = f[4].Get<uint32>();
                    sp.key = f[5].Get<std::string>();
                    if (!sSpellMgr->GetSpellInfo(sp.spell))
                        problems.push_back("specialisation " + std::to_string(sp.spell) + " is not in Spell.dbc");
                    m->specs[sp.spell] = sp;
                } while (r->NextRow());
            if (QueryResult r = WorldDatabase.Query("SELECT `kind`, `id`, `locale`, `text` FROM `ef_prof_text`"))
                do
                {
                    Field* f = r->Fetch();
                    std::string const loc = f[2].Get<std::string>();
                    size_t const slot = loc == "frFR" ? size_t(Lang::Fr) : loc == "deDE" ? size_t(Lang::De) : size_t(Lang::En);
                    s_texts[{f[0].Get<uint32>(), f[1].Get<uint32>()}][slot] = f[3].Get<std::string>();
                } while (r->NextRow());
        }
        else
            LOG_WARN("server.loading", ">> Emberfall profession specialisations: ef_prof_specialisation or ef_prof_text missing "
                "(mod_emberfall_2026_11_14_00_prof_rework.sql); no specialisations at the last rank");
        m->Index();
        for (std::string const& p : m->Problems())
            problems.push_back(p);
        // the made items' use spells by family (potency, cleanse) and the spells the potency nodes name
        s_itemSpellFamily.clear();
        s_potencySpells.clear();
        for (auto const& [spell, recipe] : m->recipes)
            if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(recipe.item))
                for (auto const& use : proto->Spells)
                    if (use.SpellId > 0)
                        s_itemSpellFamily[static_cast<uint32>(use.SpellId)] = recipe.family;
        for (auto const& [id, t] : m->trees)
            for (Node const& n : t.nodes)
                if (n.effect == Effect::Potency && n.active)
                {
                    s_potencySpells.insert(n.targetSpells.begin(), n.targetSpells.end());
                    for (auto const& [itemSpell, family] : s_itemSpellFamily)
                        if (n.families.count(family))
                            s_potencySpells.insert(itemSpell);
                }
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
        LOG_INFO("server.loading", ">> Emberfall profession specialisations: {} trees, {} nodes, {} recipes, {} trainers, {} last-rank "
            "specialisations, unlearn ladder {} steps (data {})", m->trees.size(), nodeCount, m->recipes.size(), m->trainers.size(),
            m->specs.size(), m->unlearnCopper.size(), m->hash);
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
            SendFreeResets(player, *st);
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
        // A skill set before the state is loaded (Player::LoadFromDB: the known rank spells' addSpell sets their
        // skills; a login migration that moves several skills, the secondary ranks' 141/150 -> 163/175 and 300/300 ->
        // 500/500, secondary-professions.md §10) waits for the login: the state loads and re-validates at OnLogin, after
        // those migrations. Loaded here, it judged the talents against skills not yet moved and could wipe them. Once
        // loaded (every login loads it), a set or unset is handled here as before.
        PlayerState* st = s_enabled ? player->CustomData.Get<PlayerState>(kStateKey) : nullptr;
        if (!st || !st->loaded)
            return;
        st = State(player);   // a reload after a data reload (the generation)
        if (!st)
            return;
        if (newValue == 0)
        {
            auto const spec = st->specs.find(skill);
            bool const hadSpec = spec != st->specs.end() && spec->second.spell;
            if (m->Spent(skill, st->ranks) || hadSpec)
            {
                for (Tree const* t : m->TreesOf(skill))
                    m->WipeTree(t->id, st->ranks);
                CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
                SaveProfession(trans, player->GetGUID().GetCounter(), *m, skill, st->ranks);
                Reconcile(player, *st, *m, trans);
                if (hadSpec)   // the profession unlearned: its specialisation goes too (the price ladder's count stays)
                {
                    spec->second.spell = 0;
                    SaveSpec(trans, player->GetGUID().GetCounter(), skill, spec->second);
                    ReconcileSpecs(player, *st, *m);
                }
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
        if (!m || !loot)
            return;
        PlayerState* st = State(player);
        if (!st || st->ranks.empty())
            return;
        uint32 generation = 0;
        bool any = false;
        uint32 const skill = LootSkill(player, lootGuid, loot, generation, any);
        if (!skill && !any)
            return;
        // a vein worked in swings (emberfall_vein_swings.cpp): its ore as one stack before the rolls, and the yield
        // once per vein and player, on the first swing (a full vein's extra ore as when it was one gather); an
        // upgrade rolls on every swing's ore (the same share of the ore as before)
        bool laterSwing = false;
        if (lootGuid.IsGameObject())
            if (GameObject* go = ObjectAccessor::GetGameObject(*player, lootGuid))
            {
                Emberfall::VeinSwings::MergeLoot(go, loot);
                laterSwing = Emberfall::VeinSwings::IsLaterSwing(go, player);
            }
        std::pair<uint32, uint8> const key{generation, static_cast<uint8>(loot->loot_type)};
        auto const seen = st->lootSeen.find(lootGuid);
        if (seen != st->lootSeen.end() && seen->second == key)
            return; // this loot was rolled for already (the window opened again)
        st->lootSeen[lootGuid] = key;
        if (st->lootSeen.size() > 256)
            st->lootSeen.erase(st->lootSeen.begin());
        // a corpse's loot: only the nodes of the professions whose materials corpses drop (Cooking's meat, Tailoring's and
        // First Aid's cloth and glands), never a skinner's or a herbalist's on a stray leather or herb
        auto const takes = [&](Node const& n) {
            uint32 const s = m->SkillOfNode(n.id);
            return !any || s == SKILL_COOKING || s == SKILL_TAILORING || s == SKILL_FIRST_AID;
        };
        for (LootItem& li : loot->items)
        {
            if (li.is_looted)
                continue;
            // an upgrade first (a chalk aspect come out silver, a leather as its hide), node by node, then the yield
            for (auto const& [n, chance] : m->Invested(st->ranks, Effect::Upgrade, skill, [&](Node const& x) { return x.items.count(li.itemid) != 0 && takes(x); }))
            {
                bool const hit = roll_chance_i(static_cast<int32>(std::min<uint32>(chance, 100)));
                LOG_INFO("module", "Emberfall profspecs: {} upgrade roll on {} ({}%): {}", player->GetName(), li.itemid, chance,
                    hit ? std::to_string(li.itemid + n->extra) : std::string("no"));
                if (hit && sObjectMgr->GetItemTemplate(li.itemid + n->extra))
                {
                    li.itemid += n->extra;
                    break;
                }
            }
            uint32 const chance = laterSwing ? 0 : m->Sum(st->ranks, Effect::Yield, skill, [&](Node const& n) { return n.items.count(li.itemid) != 0 && takes(n); });
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
        auto const [batch, more] = m->Batch(st->ranks, r.skill, r.family);
        if (batch && roll_chance_i(static_cast<int32>(std::min<uint32>(batch, 100))))
        {
            LOG_INFO("module", "Emberfall profspecs: {} double batch on {} ({}%): {} more", player->GetName(), ability->Spell, batch, more);
            if (more)
                GiveItems(player, r.item, more);
        }
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(ability->Spell);
        // a reagent back, node by node: one of the node's items the recipe took (none named: the one it takes most of)
        if (spell)
            for (auto const& [n, chance] : m->Invested(st->ranks, Effect::CraftRefund, r.skill, [&](Node const& x) { return Model::TakesFamily(x, r.family); }))
            {
                uint32 back = 0, most = 0;
                for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
                {
                    uint32 const reagent = spell->Reagent[i] > 0 ? static_cast<uint32>(spell->Reagent[i]) : 0;
                    uint32 const count = spell->ReagentCount[i];
                    if (reagent && count > most && (n->items.empty() || n->items.count(reagent)))
                    {
                        back = reagent;
                        most = count;
                    }
                }
                if (!back)
                    continue;
                bool const hit = roll_chance_i(static_cast<int32>(std::min<uint32>(chance, 100)));
                LOG_INFO("module", "Emberfall profspecs: {} refund roll on {} (node {}, {}%): {}", player->GetName(), ability->Spell, n->id, chance,
                    hit ? std::to_string(back) : std::string("no"));
                if (hit)
                    GiveItems(player, back, 1);
            }
        // the item made as the one `extra` IDs on (a chalk glyph come out silver): the one made goes, the better one comes
        for (auto const& [n, chance] : m->Invested(st->ranks, Effect::Upgrade, r.skill, [&](Node const& x) { return x.items.count(r.item) != 0; }))
        {
            bool const hit = roll_chance_i(static_cast<int32>(std::min<uint32>(chance, 100)));
            LOG_INFO("module", "Emberfall profspecs: {} upgrade roll on {} ({}%): {}", player->GetName(), r.item, chance, hit ? "brighter" : "no");
            if (hit && player->HasItemCount(r.item, 1) && sObjectMgr->GetItemTemplate(r.item + n->extra))
            {
                player->DestroyItemCount(r.item, 1, true);
                GiveItems(player, r.item + n->extra, 1);
            }
            break;
        }
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

    void OnTick(Unit* target, Unit* caster, uint32& amount, SpellInfo const* spellInfo)
    {
        if (!spellInfo || !amount || !caster || !target || !s_potencySpells.count(spellInfo->Id))
            return;
        Player* player = caster->ToPlayer();
        std::shared_ptr<Model const> const m = s_model;
        PlayerState* st = player && m ? State(player) : nullptr;
        if (!st || st->ranks.empty())
            return;
        auto const fam = s_itemSpellFamily.find(spellInfo->Id);
        uint32 const pct = m->Potency(st->ranks, spellInfo->Id, fam == s_itemSpellFamily.end() ? 0 : fam->second, target->GetHealthPct());
        if (pct)
            amount = Model::Raise(amount, pct);
    }

    void OnSpellDamage(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo)
    {
        if (damage <= 0)
            return;
        uint32 amount = static_cast<uint32>(damage);
        OnTick(target, attacker, amount, spellInfo);
        damage = static_cast<int32>(amount);
    }

    void OnSpellCast(Player* player, Spell* spell)
    {
        std::shared_ptr<Model const> const m = s_model;
        if (!m || !spell || s_itemSpellFamily.empty())
            return;
        auto const fam = s_itemSpellFamily.find(spell->GetSpellInfo()->Id);
        if (fam == s_itemSpellFamily.end())
            return;
        PlayerState* st = State(player);
        if (!st || st->ranks.empty())
            return;
        uint32 const chance = m->Sum(st->ranks, Effect::Cleanse, 0, [&](Node const& n) { return n.families.count(fam->second) != 0; });
        if (!chance)
            return;
        Unit* target = spell->m_targets.GetUnitTarget();
        if (!target)
            target = player;
        bool const hit = roll_chance_i(static_cast<int32>(std::min<uint32>(chance, 100)));
        // the first poison or bleed on the target (an aura of a harmful spell whose dispel is poison or whose mechanic,
        // or one of its effects', is bleeding)
        Aura* found = nullptr;
        if (hit)
            for (auto const& [id, app] : target->GetAppliedAuras())
            {
                Aura* aura = app->GetBase();
                SpellInfo const* info = aura->GetSpellInfo();
                if (app->IsPositive() || aura->IsPassive())
                    continue;
                if (info->Dispel == DISPEL_POISON || (info->GetAllEffectsMechanicMask() & (1 << MECHANIC_BLEED)))
                {
                    found = aura;
                    break;
                }
            }
        LOG_INFO("module", "Emberfall profspecs: {} cleanse roll on {} ({}%): {}", player->GetName(), spell->GetSpellInfo()->Id, chance,
            found ? std::to_string(found->GetId()) : hit ? std::string("nothing to draw") : std::string("no"));
        if (found)
            target->RemoveAura(found, AURA_REMOVE_BY_ENEMY_SPELL);
    }

    void OnRepair(Player* player, float& discountMod)
    {
        std::shared_ptr<Model const> const m = s_model;
        PlayerState* st = m ? State(player) : nullptr;
        if (!st || st->ranks.empty())
            return;
        float const factor = m->RepairFactor(st->ranks);
        if (factor < 1.0f)
            discountMod *= factor;
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

    // The trainer's own menu, then ours: an unlearn line per talent tree with points, and for a craft at its last rank
    // the specialisation lines (pick one, or give the active one up; professions.md §28), in the client's language.
    bool OnGossipHello(Player* player, Creature* creature)
    {
        std::shared_ptr<Model const> const m = s_model;
        if (!m || !creature)
            return false;
        auto const trainer = m->trainers.find(creature->GetEntry());
        if (trainer == m->trainers.end())
            return false;
        PlayerState* st = State(player);
        if (!st)
            return false;
        uint32 const skill = trainer->second;
        Lang const lang = LangOf(player);
        struct Line
        {
            uint32 sender, action, money;
            std::string text, box;
        };
        std::vector<Line> lines;
        for (Tree const* t : m->TreesOf(skill))
        {
            uint32 const spent = m->SpentInTree(t->id, st->ranks);
            if (!spent)
                continue;
            std::string const name = Text(kTextTree, t->id, lang, t->name);
            lines.push_back({kGossipSender, t->id, spent * m->feePerPoint, text::UnlearnTreeLine(lang, name, spent),
                             text::UnlearnTreeBox(lang, name, spent)});
        }
        if (player->HasSkill(skill))
        {
            auto const own = st->specs.find(skill);
            uint32 const active = own == st->specs.end() ? 0 : own->second.spell;
            if (Specialisation const* sp = active ? m->Spec(active) : nullptr)
            {
                std::string const name = Text(kTextSpecName, sp->spell, lang, sp->key);
                uint32 const price = m->UnlearnCopper(own->second.unlearns);
                lines.push_back({kDropSender, skill, price, text::DropLine(lang, name),
                                 text::DropBox(lang, name, CraftLabel(skill, lang), price, sp->backTo)});
            }
            else
                for (Specialisation const* sp : m->SpecsOf(skill))
                    if (Model::CanPick(*sp, player->GetPureMaxSkillValue(skill), 0))
                        lines.push_back({kPickSender, sp->spell, 0, text::PickLine(lang, Text(kTextSpecName, sp->spell, lang, sp->key)),
                                         text::PickBox(lang, Text(kTextSpecWhat, sp->spell, lang, ""))});
        }
        if (lines.empty())
            return false;
        player->PrepareGossipMenu(creature, creature->GetGossipMenuId(), true);
        for (Line const& l : lines)
            player->PlayerTalkClass->GetGossipMenu().AddMenuItem(-1, GOSSIP_ICON_TRAINER, l.text, l.sender, l.action, l.box, l.money, false);
        player->SendPreparedGossip(creature);
        return true;
    }

    namespace
    {
        void ResetTree(Player* player, Creature* creature, PlayerState& st, Model const& m, uint32 skill, uint32 tree)
        {
            auto const t = m.trees.find(tree);
            if (t == m.trees.end() || t->second.skill != skill)
                return;
            uint32 const spent = m.SpentInTree(tree, st.ranks);
            uint32 const fee = spent * m.feePerPoint;
            if (!spent)
                return;
            if (!player->HasEnoughMoney(fee))
            {
                player->SendBuyError(BUY_ERR_NOT_ENOUGHT_MONEY, creature, 0, 0);
                return;
            }
            player->ModifyMoney(-static_cast<int32>(fee));
            m.WipeTree(tree, st.ranks);
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            SaveProfession(trans, player->GetGUID().GetCounter(), m, skill, st.ranks);
            Reconcile(player, st, m, trans);
            Commit(trans);
            st.lastNodes = "?";
            LOG_INFO("module", "Emberfall profspecs: {} reset tree {} ({} points, {} copper)", player->GetName(), tree, spent, fee);
            SendBody(player, eft::body(0, "RESET", {std::to_string(skill), std::to_string(tree), std::to_string(spent), std::to_string(fee)}));
            SendState(player, st, m, skill);
        }

        void PickSpec(Player* player, PlayerState& st, Model const& m, uint32 skill, uint32 spell)
        {
            Specialisation const* sp = m.Spec(spell);
            PlayerState::Spec& own = st.specs[skill];
            if (!sp || sp->skill != skill || !player->HasSkill(skill) || !Model::CanPick(*sp, player->GetPureMaxSkillValue(skill), own.spell))
                return;
            own.spell = spell;
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            SaveSpec(trans, player->GetGUID().GetCounter(), skill, own);
            Commit(trans);
            ReconcileSpecs(player, st, m);
            Lang const lang = LangOf(player);
            ChatHandler(player->GetSession()).SendSysMessage(text::Picked(lang, Text(kTextSpecName, spell, lang, sp->key)));
            LOG_INFO("module", "Emberfall profspecs: {} picked specialisation {} ({}) of skill {}", player->GetName(), spell, sp->key, skill);
        }

        // Giving the specialisation up: the price ladder's next step, the spell gone, the skill back to the rank's
        // start, and the talent trees reset for free when their spend no longer fits the lower skill's budget.
        void DropSpec(Player* player, Creature* creature, PlayerState& st, Model const& m, uint32 skill)
        {
            auto const own = st.specs.find(skill);
            Specialisation const* sp = own == st.specs.end() ? nullptr : m.Spec(own->second.spell);
            if (!sp || sp->skill != skill)
                return;
            uint32 const price = m.UnlearnCopper(own->second.unlearns);
            Lang const lang = LangOf(player);
            if (!player->HasEnoughMoney(price))
            {
                player->SendBuyError(BUY_ERR_NOT_ENOUGHT_MONEY, creature, 0, 0);
                ChatHandler(player->GetSession()).SendSysMessage(text::NotEnoughMoney(lang, price));
                return;
            }
            player->ModifyMoney(-static_cast<int32>(price));
            std::string const name = Text(kTextSpecName, sp->spell, lang, sp->key);
            own->second.spell = 0;
            ++own->second.unlearns;
            ReconcileSpecs(player, st, m);
            if (player->HasSkill(skill) && player->GetPureSkillValue(skill) > sp->backTo)
                player->SetSkill(skill, player->GetSkillStep(skill), sp->backTo, player->GetPureMaxSkillValue(skill));
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            uint32 const value = player->HasSkill(skill) ? player->GetPureSkillValue(skill) : 0;
            if (m.Spent(skill, st.ranks) > m.Budget(skill, value))
            {
                FreeReset(st, m, skill);
                SaveProfession(trans, player->GetGUID().GetCounter(), m, skill, st.ranks);
                Reconcile(player, st, m, trans);
                st.lastNodes = "?";
            }
            SaveSpec(trans, player->GetGUID().GetCounter(), skill, own->second);
            Commit(trans);
            SendFreeResets(player, st);
            SendState(player, st, m, skill);
            ChatHandler(player->GetSession()).SendSysMessage(text::Dropped(lang, name, CraftLabel(skill, lang), price, value));
            LOG_INFO("module", "Emberfall profspecs: {} gave up specialisation {} of skill {} for {} copper (unlearn {}), skill now {}",
                player->GetName(), sp->spell, skill, price, own->second.unlearns, value);
        }
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action)
    {
        if (sender != kGossipSender && sender != kPickSender && sender != kDropSender)
            return false;
        std::shared_ptr<Model const> const m = s_model;
        PlayerState* st = State(player);
        CloseGossipMenuFor(player);
        if (!m || !st || !creature)
            return true;
        auto const trainer = m->trainers.find(creature->GetEntry());
        if (trainer == m->trainers.end())
            return true;
        if (sender == kGossipSender)
            ResetTree(player, creature, *st, *m, trainer->second, action);
        else if (sender == kPickSender)
            PickSpec(player, *st, *m, trainer->second, action);
        else if (action == trainer->second)
            DropSpec(player, creature, *st, *m, trainer->second);
        return true;
    }

    void OnCheckCast(Spell* spell, SpellCastResult& res)
    {
        std::shared_ptr<Model const> const m = s_model;
        if (!m || m->specs.empty() || !spell || res != SPELL_CAST_OK)
            return;
        SpellInfo const* info = spell->GetSpellInfo();
        Specialisation const* sp = info && info->CasterAuraSpell ? m->Spec(info->CasterAuraSpell) : nullptr;
        Player* player = sp && spell->GetCaster() ? spell->GetCaster()->ToPlayer() : nullptr;
        if (!player || player->HasAura(sp->spell))
            return;
        res = SPELL_FAILED_CASTER_AURASTATE;
        PlayerState* st = State(player);
        uint64 const now = NowMs();
        if (st && now - st->lastSpecRefusal < 3000)
            return;
        if (st)
            st->lastSpecRefusal = now;
        Lang const lang = LangOf(player);
        ChatHandler(player->GetSession()).SendSysMessage(text::NeedsSpec(lang, Text(kTextSpecName, sp->spell, lang, sp->key)));
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
                ", points " + std::to_string(m->Spent(skill, st->ranks)) + "/" + std::to_string(m->Budget(skill, value)) + ", ranks " + m->Encode(skill, st->ranks));
        }
        for (auto const& [skill, sp] : st->specs)
        {
            Specialisation const* spec = m->Spec(sp.spell);
            out.push_back("  specialisation of skill " + std::to_string(skill) + ": " + (spec ? spec->key + " (" + std::to_string(sp.spell) + ")" : std::string("none")) +
                ", given up " + std::to_string(sp.unlearns) + " time(s), next costs " + std::to_string(m->UnlearnCopper(sp.unlearns)) + " copper");
        }
        out.push_back("  effects: yield " + std::to_string(m->Sum(st->ranks, Effect::Yield)) + ", minimap range " +
            std::to_string(m->Sum(st->ranks, Effect::MinimapRange)) + ", rarest first " + std::to_string(m->Sum(st->ranks, Effect::RarestFirst)) +
            ", gather speed " + std::to_string(m->Sum(st->ranks, Effect::GatherSpeed)) + ", craft extra " +
            std::to_string(m->Sum(st->ranks, Effect::CraftExtra)) + ", duration " + std::to_string(m->Sum(st->ranks, Effect::Duration)) +
            ", discovery " + std::to_string(m->Sum(st->ranks, Effect::Discovery)) + ", refund " +
            std::to_string(m->Sum(st->ranks, Effect::CraftRefund)) + ", upgrade " + std::to_string(m->Sum(st->ranks, Effect::Upgrade)) +
            ", skill " + std::to_string(m->Sum(st->ranks, Effect::SkillBonus)) + ", potency " + std::to_string(m->Sum(st->ranks, Effect::Potency)) +
            ", cleanse " + std::to_string(m->Sum(st->ranks, Effect::Cleanse)) + ", repair discount " +
            std::to_string(m->Sum(st->ranks, Effect::RepairDiscount)) + "%");
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
