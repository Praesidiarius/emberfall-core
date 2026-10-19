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

// Emberfall's world editor (Emberfall docs/design/world-editor.md). See EfEditor.h; the protocol and the pure rules
// are EfEditorLogic.h. Everything here runs on the world thread: CMSG_MESSAGECHAT is PROCESS_THREADUNSAFE, like the
// GM commands this replaces (.npc add/move/delete), and OnStartup/OnUpdate are world hooks.
//
// The never-trust-the-client rule: every request runs §6.3's checklist in order (access, parse, set, zone, lock,
// presence, whitelist, ground, values, density, rate) and is audited (ef_audit), accepted or refused. A position the
// client sends is only a wish: the stored Z is the map's ground.

#include "EfEditor.h"
#include "EfEditorLogic.h"

#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "CreatureData.h"
#include "DataMap.h"
#include "DatabaseEnv.h"
#include "GameGraveyard.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SharedDefines.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <chrono>
#include <ctime>
#include <map>
#include <set>

using namespace ef::editor;
namespace eft = ef::eft;

namespace Emberfall::Editor
{
    namespace
    {
        constexpr char const* kSessionKey = "emberfall_editor";

        struct Config
        {
            bool enable = false;
            bool local = false;
            std::string db;
            uint32 refreshMs = 60000;
            Limits limits;
        } s_config;

        bool s_ready = false;          // enabled and the release data and change sets are loaded

        struct ZoneInfo
        {
            std::string key, name;
            uint32 map = 0, area = 0, blockLo = 0, blockHi = 0;
        };
        std::map<std::string, ZoneInfo> s_zones;
        struct SpawnInfo
        {
            std::string zone;
            bool editable = false;
        };
        std::map<std::pair<char, uint32>, SpawnInfo> s_spawns;          // release spawns of the editable zones
        std::map<std::string, std::map<uint32, bool>> s_entries;          // zone -> entry -> named
        std::map<uint32, Pos> s_release;                                  // spawn id -> where the release has it

        struct ChangeSet
        {
            uint32 id = 0, owner = 0;
            std::string ownerName, title, description, zone, state;
            std::vector<Op> ops;
            uint64 firstOpMs = 0;
        };
        std::map<uint32, ChangeSet> s_sets;
        std::map<std::pair<char, uint32>, uint32> s_locks;                // object -> its open set
        std::map<uint32, CreatureData> s_backup;                          // deleted spawns' data (undo, withdraw)

        struct Access
        {
            Level level = Level::None;
            bool frozen = false;
        };
        std::map<uint32, Access> s_access;
        std::map<uint32, Rate> s_rates;
        std::set<ObjectGuid> s_editors;                                   // players who got ROLE this session
        uint32 s_nextTemp = kTempIdBase, s_nextSet = 1, s_refreshTimer = 0;

        class Session : public DataMap::Base
        {
        public:
            bool role = false, mode = false;
            uint32 active = 0, scanId = 0;
            eft::Reassembly submit;
            eft::RateLimit requests{40, 10000};
            float savedSpeed = 1.0f;
        };

        uint64 NowMs()
        {
            return static_cast<uint64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        }

        std::string Table(char const* name) { return "`" + s_config.db + "`.`" + name + "`"; }

        std::string Escaped(std::string text)
        {
            CharacterDatabase.EscapeString(text);
            return text;
        }

        std::string CsName(uint32 id)
        {
            char buffer[16];
            std::snprintf(buffer, sizeof buffer, "cs-%04u", id);
            return buffer;
        }

        // --------------------------------------------------------------------------------------- the wire

        void SendBody(Player* player, std::string const& body)
        {
            std::string const text = std::string(kPrefix) + body;
            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, text);
            player->SendDirectMessage(&data);
        }
        void SendOk(Player* player, uint32 seq, std::string const& id = {}) { SendBody(player, eft::body(0, "OK", {std::to_string(seq), id})); }
        void SendErr(Player* player, uint32 seq, Code code, std::string const& detail = {})
        {
            SendBody(player, eft::body(0, "ERR", {std::to_string(seq), codeName(code), eft::encodeName(detail)}));
        }

        void Audit(Player* player, std::string_view action, Code result, Fields detail)
        {
            if (!s_ready && s_config.db.empty())
                return;
            detail["map"] = Value::of(player->GetMapId());
            detail["px"] = Value::of(std::round(player->GetPositionX() * 10) / 10);
            detail["py"] = Value::of(std::round(player->GetPositionY() * 10) / 10);
            CharacterDatabase.Execute("INSERT INTO " + Table("ef_audit") +
                " (`at`, `account`, `character_guid`, `action`, `result`, `detail_json`) VALUES (NOW(), {}, {}, '{}', '{}', '{}')",
                player->GetSession()->GetAccountId(), player->GetGUID().GetCounter(), Escaped(std::string(action)),
                codeName(result), Escaped(toJson(detail)));
        }

        Session& SessionOf(Player* player) { return *player->CustomData.GetDefault<Session>(kSessionKey); }

        Access AccessOf(uint32 account)
        {
            if (s_config.local)
                return {Level::Maintainer, false};
            auto it = s_access.find(account);
            return it == s_access.end() ? Access{} : it->second;
        }

        Limits LimitsOf(Level level) { return s_config.local ? s_config.limits.scaled(Level::Maintainer) : s_config.limits.scaled(level); }

        Rate& RateOf(uint32 account, Level level)
        {
            Rate& r = s_rates[account];
            r.configure(LimitsOf(level));
            return r;
        }

        // --------------------------------------------------------------------------------------- spawns

        Map* LiveMap(uint32 mapId) { return sMapMgr->FindBaseNonInstanceMap(mapId); }

        std::vector<Creature*> LiveCreatures(CreatureData const& data, uint32 id)
        {
            std::vector<Creature*> out;
            if (Map* map = LiveMap(data.mapid))
            {
                auto range = map->GetCreatureBySpawnIdStore().equal_range(id);
                for (auto it = range.first; it != range.second; ++it)
                    out.push_back(it->second);
            }
            return out;
        }

        bool IsTemp(uint32 id) { return id >= kTempIdBase && id <= kTempIdMax; }

        Fields StateOf(CreatureData const& d)
        {
            Fields o = posObject({d.posX, d.posY, d.posZ, d.orientation});
            o["wander"] = Value::of(std::round(d.wander_distance * 10) / 10);
            o["respawn"] = Value::of(d.spawntimesecs);
            o["entry"] = Value::of(d.id);
            return o;
        }

        void Relocate(uint32 id, CreatureData& data, Pos const& p)
        {
            sObjectMgr->RemoveCreatureFromGrid(id, &data);
            data.posX = p.x, data.posY = p.y, data.posZ = p.z, data.orientation = p.o;
            sObjectMgr->AddCreatureToGrid(id, &data);
            for (Creature* c : LiveCreatures(data, id))
            {
                c->SetHomePosition(p.x, p.y, p.z, p.o);
                if (c->IsAlive())
                {
                    c->CombatStop(true);
                    c->NearTeleportTo(p.x, p.y, p.z, p.o);
                    c->GetMotionMaster()->Initialize();
                }
            }
        }

        void SetWander(uint32 id, CreatureData& data, float wander)
        {
            data.wander_distance = wander;
            data.movementType = wander > 0 ? RANDOM_MOTION_TYPE : IDLE_MOTION_TYPE;
            for (Creature* c : LiveCreatures(data, id))
            {
                c->SetWanderDistance(wander);
                c->SetDefaultMovementType(MovementGeneratorType(data.movementType));
                if (c->IsAlive())
                    c->GetMotionMaster()->Initialize();
            }
        }

        void SetRespawn(uint32 id, CreatureData& data, uint32 seconds)
        {
            data.spawntimesecs = seconds;
            for (Creature* c : LiveCreatures(data, id))
                c->SetRespawnDelay(seconds);
        }

        // CreatureData has a const member (SpawnData::type): no assignment operator, so field by field.
        void Assign(CreatureData& d, CreatureData const& s)
        {
            d.spawnId = s.spawnId, d.mapid = s.mapid, d.phaseMask = s.phaseMask;
            d.posX = s.posX, d.posY = s.posY, d.posZ = s.posZ, d.orientation = s.orientation;
            d.spawnMask = s.spawnMask, d.ScriptId = s.ScriptId, d.dbData = s.dbData, d.spawnGroupId = s.spawnGroupId, d.poolId = s.poolId;
            d.id = s.id, d.id2 = s.id2, d.id3 = s.id3, d.displayid = s.displayid, d.equipmentId = s.equipmentId;
            d.spawntimesecs = s.spawntimesecs, d.wander_distance = s.wander_distance, d.currentwaypoint = s.currentwaypoint;
            d.curhealth = s.curhealth, d.curmana = s.curmana, d.movementType = s.movementType, d.npcflag = s.npcflag;
            d.unit_flags = s.unit_flags, d.dynamicflags = s.dynamicflags;
        }

        void Backup(uint32 id, CreatureData const& data)
        {
            s_backup.erase(id);
            Assign(s_backup[id], data);
        }

        // A spawn that the editor creates (an add, or an undone delete): its data, then the unit when the grid is loaded.
        bool Spawn(uint32 id, CreatureData const& from)
        {
            if (sObjectMgr->GetCreatureData(id))
                return false;
            CreatureData& data = sObjectMgr->NewOrExistCreatureData(id);
            Assign(data, from);
            data.spawnId = id;
            sObjectMgr->AddCreatureToGrid(id, &data);
            Map* map = LiveMap(data.mapid);
            if (map && map->IsGridLoaded(data.posX, data.posY))
            {
                Creature* c = new Creature();
                if (!c->LoadCreatureFromDB(id, map, true))
                {
                    delete c;
                    LOG_WARN("module", "Emberfall.Editor: spawn {} has data but its unit did not load", id);
                }
            }
            return true;
        }

        CreatureData NewData(uint32 entry, uint32 mapId, Pos const& p, float wander, uint32 respawn)
        {
            CreatureData d;
            d.id = entry;
            d.mapid = uint16(mapId);
            d.phaseMask = PHASEMASK_NORMAL;
            d.spawnMask = 1;
            d.posX = p.x, d.posY = p.y, d.posZ = p.z, d.orientation = p.o;
            d.spawntimesecs = respawn;
            d.wander_distance = wander;
            d.movementType = wander > 0 ? RANDOM_MOTION_TYPE : IDLE_MOTION_TYPE;
            int8 equipment = 1;
            d.equipmentId = sObjectMgr->GetEquipmentInfo(entry, equipment) ? 1 : 0;
            d.dbData = false;
            return d;
        }

        void Despawn(uint32 id)
        {
            CreatureData const* data = sObjectMgr->GetCreatureData(id);
            if (!data)
                return;
            Backup(id, *data);
            for (Creature* c : LiveCreatures(*data, id))
            {
                c->CombatStop(true);
                c->AddObjectToRemoveList();
            }
            sObjectMgr->DeleteCreatureData(id);
        }

        // Applies one op (live, or at replay). The op's `to` is the truth; `from` is for undo.
        bool Apply(Op const& op)
        {
            uint32 const id = op.target;
            switch (op.op)
            {
                case OpKind::Add:
                {
                    auto p = posOf(op.to);
                    auto entry = op.to.find("entry");
                    auto map = op.to.find("map");
                    if (!p || entry == op.to.end() || map == op.to.end())
                        return false;
                    if (s_backup.count(id) && s_backup[id].id == uint32(entry->second.number))
                    {
                        CreatureData d;
                        Assign(d, s_backup[id]);   // an undone delete: the spawn as it was, where `to` says
                        d.posX = p->x, d.posY = p->y, d.posZ = p->z, d.orientation = p->o;
                        return Spawn(id, d);
                    }
                    auto w = op.to.find("wander");
                    auto r = op.to.find("respawn");
                    float const wander = w != op.to.end() ? float(w->second.number) : 0.0f;
                    uint32 const respawn = r != op.to.end() ? uint32(r->second.number) : 120;
                    return Spawn(id, NewData(uint32(entry->second.number), uint32(map->second.number), *p, wander, respawn));
                }
                case OpKind::Move:
                {
                    CreatureData* data = const_cast<CreatureData*>(sObjectMgr->GetCreatureData(id));
                    auto p = posOf(op.to);
                    if (!data || !p)
                        return false;
                    Relocate(id, *data, *p);
                    return true;
                }
                case OpKind::Set:
                {
                    CreatureData* data = const_cast<CreatureData*>(sObjectMgr->GetCreatureData(id));
                    if (!data)
                        return false;
                    for (auto const& [k, v] : op.to)
                    {
                        if (k == "wander" && v.type == Value::Type::Number)
                            SetWander(id, *data, float(v.number));
                        else if (k == "respawn" && v.type == Value::Type::Number)
                            SetRespawn(id, *data, uint32(v.number));
                    }
                    return true;
                }
                case OpKind::Delete:
                    if (!sObjectMgr->GetCreatureData(id))
                        return false;
                    Despawn(id);
                    return true;
            }
            return false;
        }

        // --------------------------------------------------------------------------------------- markers

        std::string FlagsFor(uint32 viewer, char kind, uint32 id, CreatureData const& data)
        {
            std::string flags;
            auto lock = s_locks.find({kind, id});
            if (lock != s_locks.end())
            {
                auto set = s_sets.find(lock->second);
                bool const mine = set != s_sets.end() && set->second.owner == viewer;
                flags += mine ? flag::Mine : flag::Pending;
                if (!mine)
                    flags += flag::Locked;
            }
            auto spawn = s_spawns.find({kind, id});
            bool const editable = IsTemp(id) || (spawn != s_spawns.end() && spawn->second.editable);
            if (!editable)
                flags += flag::ReadOnly;
            if (IsTemp(id))
                flags += flag::Added;
            if (spawn != s_spawns.end())
            {
                auto zone = s_entries.find(spawn->second.zone);
                if (zone != s_entries.end())
                {
                    auto e = zone->second.find(data.id);
                    if (e != zone->second.end() && e->second)
                        flags += flag::Named;
                }
            }
            bool alive = false;
            for (Creature* c : LiveCreatures(data, id))
                alive = alive || c->IsAlive();
            if (!alive)
                flags += flag::Dead;
            return flags;
        }

        char KindOf(uint32 id, CreatureData const& data)
        {
            if (s_spawns.count({'n', id}))
                return 'n';
            return id == data.id ? 'n' : 'c';   // an NPC's GUID is its template id (npcs.yaml)
        }

        Marker MarkerOf(uint32 viewer, uint32 id, CreatureData const& data)
        {
            Marker m;
            char const k = KindOf(id, data);
            m.kind = k == 'n' ? Kind::Npc : Kind::Creature;
            m.id = id, m.entry = data.id;
            m.pos = {data.posX, data.posY, data.posZ, data.orientation};
            m.wander = data.wander_distance, m.respawn = data.spawntimesecs;
            m.flags = FlagsFor(viewer, k, id, data);
            for (Creature* c : LiveCreatures(data, id))
                if (c->IsAlive())
                {
                    m.lowguid = c->GetGUID().GetCounter();
                    break;
                }
            return m;
        }

        // DELTA (or GONE) to every editor within 250 yd of the change, each with their own flags.
        void Broadcast(char kind, uint32 id, uint32 mapId, Pos const& at)
        {
            CreatureData const* data = sObjectMgr->GetCreatureData(id);
            for (auto it = s_editors.begin(); it != s_editors.end();)
            {
                Player* p = ObjectAccessor::FindPlayer(*it);
                if (!p)
                {
                    it = s_editors.erase(it);
                    continue;
                }
                ++it;
                if (p->GetMapId() != mapId || p->GetExactDist2d(at.x, at.y) > kMaxScanRadius + 50)
                    continue;
                if (data)
                    SendBody(p, eft::body(0, "DELTA", {encodeMarker(MarkerOf(p->GetSession()->GetAccountId(), id, *data))}));
                else
                    SendBody(p, eft::body(0, "GONE", {std::string(1, kind), std::to_string(id)}));
            }
        }

        // --------------------------------------------------------------------------------------- change sets

        void SendSet(Player* player, ChangeSet const& cs)
        {
            size_t active = 0;
            for (Op const& op : cs.ops)
                active += !op.undone;
            SendBody(player, eft::body(0, "CS", {std::to_string(cs.id), cs.state, std::to_string(active), eft::encodeName(cs.title)}));
        }

        // OPS: the set's ops for the panel, newest last: n,op,k,id,entry,detail,undone (detail: moved yards, field>value).
        void SendOps(Player* player, ChangeSet const& cs)
        {
            std::vector<std::string> entries;
            size_t const first = cs.ops.size() > 60 ? cs.ops.size() - 60 : 0;
            for (size_t i = first; i < cs.ops.size(); ++i)
            {
                Op const& op = cs.ops[i];
                std::string detail;
                if (op.op == OpKind::Move)
                {
                    auto a = posOf(op.from), b = posOf(op.to);
                    if (a && b)
                        detail = fixed(dist2d(*a, *b), 1) + ":" + fixed(b->o, 2);
                }
                else if (op.op == OpKind::Set)
                    for (auto const& [k, v] : op.to)
                    {
                        auto f = op.from.find(k);
                        detail = k + ":" + (f != op.from.end() && f->second.type == Value::Type::Number ? numberText(f->second.number) : "") +
                                 ":" + numberText(v.number);
                    }
                uint32 entry = 0;
                for (Fields const* o : {&op.to, &op.from})
                    if (auto e = o->find("entry"); e != o->end())
                        entry = uint32(e->second.number);
                if (!entry)
                    if (CreatureData const* d = sObjectMgr->GetCreatureData(op.target))
                        entry = d->id;
                entries.push_back(std::to_string(op.seq) + "," + opName(op.op) + "," + std::string(1, char(op.kind)) + "," +
                                  std::to_string(op.target) + "," + std::to_string(entry) + "," + detail + "," + (op.undone ? "1" : "0"));
            }
            for (std::string const& body : pages("OPS", {std::to_string(cs.id)}, entries, 16))
                SendBody(player, body);
        }

        Player* OnlineOwner(ChangeSet const& cs)
        {
            for (ObjectGuid const& g : s_editors)
                if (Player* p = ObjectAccessor::FindPlayer(g))
                    if (p->GetSession()->GetAccountId() == cs.owner)
                        return p;
            return nullptr;
        }

        void StoreState(ChangeSet const& cs)
        {
            CharacterDatabase.DirectExecute("UPDATE " + Table("ef_changeset") + " SET `state` = '{}'{} WHERE `id` = {}",
                Escaped(cs.state), cs.state == state::Submitted ? std::string(", `submitted_at` = NOW()") : std::string(), cs.id);
        }

        void Lock(char kind, uint32 id, uint32 set)
        {
            if (s_locks.count({kind, id}))
                return;
            s_locks[{kind, id}] = set;
            CharacterDatabase.DirectExecute("REPLACE INTO " + Table("ef_lock") + " (`kind`, `target_id`, `changeset_id`) VALUES ('{}', {}, {})",
                std::string(1, kind), id, set);
        }

        void Unlock(uint32 set)
        {
            for (auto it = s_locks.begin(); it != s_locks.end();)
                it = it->second == set ? s_locks.erase(it) : std::next(it);
            CharacterDatabase.DirectExecute("DELETE FROM " + Table("ef_lock") + " WHERE `changeset_id` = {}", set);
        }

        // Reverts a set's active ops live, newest first (withdraw, reject, freeze).
        void RevertLive(ChangeSet& cs)
        {
            for (size_t i = cs.ops.size(); i-- > 0;)
            {
                Op const& op = cs.ops[i];
                if (op.undone)
                    continue;
                Op const inv = inverse(op);
                CreatureData const* d = sObjectMgr->GetCreatureData(op.target);
                uint32 const mapId = d ? d->mapid : 0;
                Pos const at = d ? Pos{d->posX, d->posY, d->posZ, d->orientation} : Pos{};
                Apply(inv);
                Broadcast(char(op.kind), op.target, mapId, at);
            }
        }

        void StoreOp(ChangeSet const& cs, Op const& op)
        {
            CharacterDatabase.DirectExecute("INSERT INTO " + Table("ef_op") +
                " (`changeset_id`, `seq`, `op`, `kind`, `target_id`, `temp_id`, `from_json`, `to_json`, `created_at`, `undone`)"
                " VALUES ({}, {}, '{}', '{}', {}, {}, '{}', '{}', NOW(), 0)",
                cs.id, op.seq, opName(op.op), std::string(1, char(op.kind)), op.target,
                op.tempId ? std::to_string(op.tempId) : std::string("NULL"), Escaped(toJson(op.from)), Escaped(toJson(op.to)));
        }

        // --------------------------------------------------------------------------------------- validation (§6.3)

        struct Target
        {
            char kind = 'c';
            uint32 id = 0;
            CreatureData* data = nullptr;
            std::string zone;
        };

        ZoneInfo const* ZoneAt(uint32 mapId, Pos const& p)
        {
            Map* map = LiveMap(mapId);
            if (!map)
                map = sMapMgr->CreateBaseMap(mapId);
            if (!map)
                return nullptr;
            uint32 const zone = map->GetZoneId(PHASEMASK_NORMAL, p.x, p.y, p.z);
            for (auto const& [key, z] : s_zones)
                if (z.map == mapId && z.area == zone)
                    return &z;
            return nullptr;
        }

        // Step 4 for a target: an editable release spawn, or a spawn the editor added; its zone.
        Code CheckTarget(char kind, uint32 id, Target& t)
        {
            t.kind = kind, t.id = id;
            t.data = const_cast<CreatureData*>(sObjectMgr->GetCreatureData(id));
            if (!t.data)
                return Code::Unknown;
            if (IsTemp(id))
            {
                if (kind != 'c')
                    return Code::ReadOnly;
                auto lock = s_locks.find({kind, id});
                if (lock == s_locks.end())
                    return Code::ReadOnly;
                t.zone = s_sets[lock->second].zone;
                return Code::Ok;
            }
            auto s = s_spawns.find({kind, id});
            if (s == s_spawns.end() || !s->second.editable)
                return Code::ReadOnly;
            t.zone = s->second.zone;
            return Code::Ok;
        }

        // Steps 6 and 8: the editor stands near; Z becomes the ground's.
        Code CheckPlace(Player* player, uint32 mapId, Pos& p, bool landCreature)
        {
            if (player->GetMapId() != mapId || player->GetExactDist2d(p.x, p.y) > kPresence)
                return Code::Presence;
            Map* map = player->GetMap();
            float const ground = map->GetHeight(player->GetPhaseMask(), p.x, p.y, p.z + 3.0f, true, 25.0f);
            Code const g = checkGround(p.z, ground);
            if (g != Code::Ok)
                return g;
            p.z = std::round(ground * 100) / 100;
            if (landCreature && map->IsUnderWater(player->GetPhaseMask(), p.x, p.y, p.z, 2.0f))
                return Code::Water;
            return Code::Ok;
        }

        // Step 10.
        Density CheckNeighbours(uint32 mapId, Pos const& at, uint32 self, Limits const& l)
        {
            std::vector<Neighbour> near;
            for (auto const& [id, d] : sObjectMgr->GetAllCreatureData())
            {
                if (d.mapid != mapId || std::fabs(d.posX - at.x) > 31 || std::fabs(d.posY - at.y) > 31)
                    continue;
                Neighbour n;
                n.pos = {d.posX, d.posY, d.posZ, 0};
                n.self = id == self;
                if (CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(d.id))
                {
                    uint32 const service = UNIT_NPC_FLAG_INNKEEPER | UNIT_NPC_FLAG_FLIGHTMASTER | UNIT_NPC_FLAG_SPIRITHEALER |
                                           UNIT_NPC_FLAG_SPIRITGUIDE;
                    n.service = (t->npcflag & service) != 0;
                    n.questGiver = (t->npcflag & UNIT_NPC_FLAG_QUESTGIVER) != 0;
                    n.hostile = t->npcflag == 0;
                }
                near.push_back(n);
            }
            for (auto const& [gid, g] : sGraveyard->GetGraveyardData())
                if (g.Map == mapId && std::fabs(g.x - at.x) < 31 && std::fabs(g.y - at.y) < 31)
                {
                    Neighbour n;
                    n.pos = {g.x, g.y, g.z, 0};
                    n.service = true;
                    near.push_back(n);
                }
            return checkDensity(at, near, l);
        }

        ChangeSet* ActiveSet(Player* player, Session& s, Code& code)
        {
            auto it = s_sets.find(s.active);
            if (it == s_sets.end() || it->second.owner != player->GetSession()->GetAccountId())
            {
                code = Code::Set;
                return nullptr;
            }
            if (!state::editable(it->second.state))
            {
                code = Code::State;
                return nullptr;
            }
            code = Code::Ok;
            return &it->second;
        }

        // Steps 5 and 11 for an op on an object, then the op is applied, stored and broadcast.
        Code Commit(Player* player, ChangeSet& cs, Level level, Op op, Pos const& at, std::string& id)
        {
            uint32 const account = player->GetSession()->GetAccountId();
            auto lock = s_locks.find({char(op.kind), op.target});
            if (lock != s_locks.end() && lock->second != cs.id)
                return Code::Lock;
            Limits const l = LimitsOf(level);
            bool const newObject = lock == s_locks.end();
            if (newObject && netObjects(cs.ops) + 1 > l.opsPerSet)
                return Code::Limit;
            Rate& rate = RateOf(account, level);
            uint64 const now = NowMs();
            if (!rate.allows(op.op, now))
                return Code::Rate;
            if (!Apply(op))
                return Code::Internal;
            rate.record(op.op, now);
            op.seq = cs.ops.empty() ? 1 : cs.ops.back().seq + 1;
            op.createdMs = now;
            if (cs.firstOpMs == 0)
                cs.firstOpMs = now;
            // a new op makes the undone ops after the last active one final (no redo past it)
            cs.ops.push_back(op);
            StoreOp(cs, op);
            Lock(char(op.kind), op.target, cs.id);
            Broadcast(char(op.kind), op.target, player->GetMapId(), at);
            id = std::to_string(op.target);
            return Code::Ok;
        }

        // --------------------------------------------------------------------------------------- requests

        void SendRole(Player* player, Access const& a)
        {
            std::string zones;
            for (auto const& [key, z] : s_zones)
                zones += (zones.empty() ? "" : ",") + key + ":" + std::to_string(z.map) + ":" + std::to_string(z.area);
            Limits const l = LimitsOf(a.level);
            Rate& r = RateOf(player->GetSession()->GetAccountId(), a.level);
            uint64 const now = NowMs();
            size_t open = 0;
            for (auto const& [id, cs] : s_sets)
                open += cs.owner == player->GetSession()->GetAccountId() && state::open(cs.state);
            std::string const limits = std::to_string(l.opsPerDay - std::min<uint32>(l.opsPerDay, uint32(r.day.count(now)))) + "," +
                std::to_string(l.openSets > open ? l.openSets - open : 0) + "," + std::to_string(l.opsPerSet);
            SendBody(player, eft::body(0, "ROLE", {std::to_string(uint32(a.level)), zones, limits, s_config.local ? "local" : "realm"}));
            // PAL;<zone>;<p>/<t>;<entry:named|...>: the creatures the editor may place (the zone brief's)
            for (auto const& [key, entries] : s_entries)
            {
                std::vector<std::string> list;
                for (auto const& [entry, named] : entries)
                    list.push_back(std::to_string(entry) + ":" + (named ? "1" : "0"));
                for (std::string const& body : pages("PAL", {key}, list, 8))
                    SendBody(player, body);
            }
        }

        void SetMode(Player* player, Session& s, bool on)
        {
            if (s.mode == on)
                return;
            s.mode = on;
            if (on)
            {
                s.savedSpeed = player->GetSpeedRate(MOVE_RUN);
                player->SetCanFly(true);
                player->SetSpeed(MOVE_RUN, 3.0f, true);
                player->SetSpeed(MOVE_FLIGHT, 3.0f, true);
                player->SetUnitFlag(UNIT_FLAG_NON_ATTACKABLE);
                player->SetImmuneToNPC(true);
                player->CombatStop(true);
            }
            else
            {
                player->SetCanFly(false);
                player->SetSpeed(MOVE_RUN, s.savedSpeed > 0 ? s.savedSpeed : 1.0f, true);
                player->SetSpeed(MOVE_FLIGHT, 1.0f, true);
                player->RemoveUnitFlag(UNIT_FLAG_NON_ATTACKABLE);
                player->SetImmuneToNPC(false);
            }
        }

        void DoScan(Player* player, uint32 seq, ef::editor::Scan const& sc)
        {
            Session& s = SessionOf(player);
            if (player->GetMapId() != sc.map || player->GetExactDist2d(sc.x, sc.y) > kMaxScanRadius)
            {
                SendErr(player, seq, Code::Presence);
                return;
            }
            uint32 const viewer = player->GetSession()->GetAccountId();
            std::vector<std::string> entries;
            for (auto const& [id, d] : sObjectMgr->GetAllCreatureData())
            {
                if (d.mapid != sc.map || std::hypot(d.posX - sc.x, d.posY - sc.y) > sc.radius)
                    continue;
                entries.push_back(encodeMarker(MarkerOf(viewer, id, d)));
            }
            s.scanId = (s.scanId % 9999) + 1;
            for (std::string const& body : pages("MARK", {std::to_string(s.scanId)}, entries))
                SendBody(player, body);
            SendOk(player, seq, std::to_string(s.scanId));
        }

        void Handle(Player* player, Session& s, Access const& a, eft::Request const& rq)
        {
            uint32 const seq = rq.seq;
            uint32 const account = player->GetSession()->GetAccountId();
            std::vector<std::string> const& f = rq.fields;
            auto refuse = [&](Code code, std::string const& detail = {}) {
                SendErr(player, seq, code, detail);
                Audit(player, rq.cmd, code, {{"seq", Value::of(seq)}, {"detail", Value::str(detail.substr(0, 120))}});
            };
            if (rq.cmd == "MODE")
            {
                if (f.size() != 1 || (f[0] != "on" && f[0] != "off"))
                    return refuse(Code::Decode);
                SetMode(player, s, f[0] == "on");
                SendOk(player, seq);
                return;
            }
            if (rq.cmd == "SCAN")
            {
                auto sc = parseScan(f);
                if (!sc)
                    return refuse(Code::Decode);
                DoScan(player, seq, *sc);
                return;
            }
            if (rq.cmd == "NEW")
            {
                std::string title = f.size() == 1 ? eft::decodeName(f[0]).value_or("") : "";
                if (checkText(title, kMinTitle, kMaxTitle) != Code::Ok)
                    return refuse(Code::Text);
                ZoneInfo const* zone = ZoneAt(player->GetMapId(), {player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), 0});
                if (!zone)
                    return refuse(Code::Zone);
                size_t open = 0;
                for (auto const& [id, cs] : s_sets)
                    open += cs.owner == account && state::open(cs.state);
                if (open >= LimitsOf(a.level).openSets)
                    return refuse(Code::Limit, "open sets");
                ChangeSet cs;
                cs.id = s_nextSet++;
                cs.owner = account;
                cs.ownerName = player->GetName();
                cs.title = title;
                cs.zone = zone->key;
                cs.state = std::string(state::Draft);
                CharacterDatabase.DirectExecute("INSERT INTO " + Table("ef_changeset") +
                    " (`id`, `owner`, `owner_name`, `title`, `description`, `zone`, `state`, `created_at`) VALUES ({}, {}, '{}', '{}', '', '{}', 'draft', NOW())",
                    cs.id, account, Escaped(cs.ownerName), Escaped(title), Escaped(cs.zone));
                s_sets[cs.id] = cs;
                s.active = cs.id;
                SendSet(player, cs);
                SendOk(player, seq, std::to_string(cs.id));
                Audit(player, "NEW", Code::Ok, {{"cs", Value::of(cs.id)}});
                return;
            }
            if (rq.cmd == "OPEN" || rq.cmd == "UNDO" || rq.cmd == "REDO" || rq.cmd == "WITHDRAW" || rq.cmd == "SUBMIT")
            {
                auto id = f.empty() ? std::nullopt : eft::number(f[0]);
                auto it = id ? s_sets.find(*id) : s_sets.end();
                if (it == s_sets.end() || it->second.owner != account)
                    return refuse(Code::Set);
                ChangeSet& cs = it->second;
                if (rq.cmd == "OPEN")
                {
                    if (!state::open(cs.state))
                        return refuse(Code::State);
                    s.active = cs.id;
                    SendSet(player, cs);
                    SendOps(player, cs);
                    SendOk(player, seq, std::to_string(cs.id));
                    return;
                }
                if (rq.cmd == "WITHDRAW")
                {
                    if (!state::open(cs.state))
                        return refuse(Code::State);
                    RevertLive(cs);
                    cs.state = std::string(state::Withdrawn);
                    StoreState(cs);
                    Unlock(cs.id);
                    if (s.active == cs.id)
                        s.active = 0;
                    SendSet(player, cs);
                    SendOk(player, seq, std::to_string(cs.id));
                    Audit(player, "WITHDRAW", Code::Ok, {{"cs", Value::of(cs.id)}});
                    return;
                }
                if (!state::editable(cs.state))
                    return refuse(Code::State);
                if (rq.cmd == "SUBMIT")
                {
                    // SUBMIT;<cs>;<p>/<t>;<description part>
                    if (f.size() != 3)
                        return refuse(Code::Decode);
                    auto part = eft::partOf(f[1]);
                    if (!part)
                        return refuse(Code::Decode);
                    auto status = s.submit.feed(seq, part->first, part->second, f[2], NowMs());
                    if (status == eft::Reassembly::Status::Incomplete)
                        return;
                    if (status == eft::Reassembly::Status::Error)
                        return refuse(Code::Decode, "chunk");
                    std::string description = eft::decodeName(s.submit.payload()).value_or("\x01");
                    if (checkText(description, 0, kMaxDescription) != Code::Ok)
                        return refuse(Code::Text);
                    bool empty = true;
                    for (Op const& op : cs.ops)
                        empty = empty && op.undone;
                    if (empty)
                        return refuse(Code::Limit, "no changes");
                    Rate& rate = RateOf(account, a.level);
                    if (rate.submits.full(NowMs()))
                        return refuse(Code::Rate, "submits");
                    rate.submits.add(NowMs());
                    cs.description = description;
                    cs.state = std::string(s_config.local ? state::Accepted : state::Submitted);
                    CharacterDatabase.DirectExecute("UPDATE " + Table("ef_changeset") +
                        " SET `description` = '{}', `state` = '{}', `submitted_at` = NOW() WHERE `id` = {}",
                        Escaped(description), cs.state, cs.id);
                    if (s_config.local)
                        Unlock(cs.id);   // accepted: replayed until make editor-pull marks it saved
                    s.active = 0;
                    SendSet(player, cs);
                    SendOk(player, seq, std::to_string(cs.id));
                    Audit(player, "SUBMIT", Code::Ok, {{"cs", Value::of(cs.id)}});
                    return;
                }
                bool const undo = rq.cmd == "UNDO";
                auto index = undo ? undoIndex(cs.ops) : redoIndex(cs.ops);
                if (!index)
                    return refuse(Code::Unknown, undo ? "nothing to undo" : "nothing to redo");
                Op& op = cs.ops[*index];
                CreatureData const* d = sObjectMgr->GetCreatureData(op.target);
                Pos at = d ? Pos{d->posX, d->posY, d->posZ, 0} : posOf(op.from).value_or(posOf(op.to).value_or(Pos{}));
                if (!Apply(undo ? inverse(op) : op))
                    return refuse(Code::Internal);
                op.undone = undo;
                CharacterDatabase.DirectExecute("UPDATE " + Table("ef_op") + " SET `undone` = {} WHERE `changeset_id` = {} AND `seq` = {}",
                    undo ? 1 : 0, cs.id, op.seq);
                Broadcast(char(op.kind), op.target, player->GetMapId(), at);
                SendSet(player, cs);
                SendOps(player, cs);
                SendOk(player, seq, std::to_string(op.target));
                Audit(player, rq.cmd, Code::Ok, {{"cs", Value::of(cs.id)}, {"seq", Value::of(op.seq)}});
                return;
            }

            // ---- the ops: ADD, MOVE, SET, DEL
            Code code = Code::Ok;
            ChangeSet* cs = ActiveSet(player, s, code);
            if (!cs)
                return refuse(code);
            ZoneInfo const& zone = s_zones[cs->zone];
            Limits const l = LimitsOf(a.level);
            std::string id;
            if (rq.cmd == "ADD")
            {
                auto add = parseAdd(f);
                if (!add)
                    return refuse(Code::Decode);
                if (add->kind != Kind::Creature)
                    return refuse(Code::Whitelist, "only creatures are added");
                Pos p = add->pos;
                ZoneInfo const* at = ZoneAt(player->GetMapId(), p);
                if (!at || at->key != cs->zone)
                    return refuse(Code::Zone);
                auto entries = s_entries.find(cs->zone);
                if (entries == s_entries.end() || !entries->second.count(add->entry))
                    return refuse(Code::Whitelist);
                bool const named = entries->second.at(add->entry);
                if (named && a.level != Level::Maintainer)
                    return refuse(Code::Named);
                if (Code c = CheckPlace(player, zone.map, p, true); c != Code::Ok)
                    return refuse(c);
                if (Density d = CheckNeighbours(zone.map, p, 0, l); d.code != Code::Ok)
                    return refuse(d.code);
                CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(add->entry);
                if (!t)
                    return refuse(Code::Whitelist);
                if (s_nextTemp > kTempIdMax)
                    return refuse(Code::Internal, "temporary ids exhausted");
                Op op;
                op.op = OpKind::Add;
                op.kind = Kind::Creature;
                op.target = op.tempId = s_nextTemp;
                op.to = posObject(p);
                op.to["entry"] = Value::of(add->entry);
                op.to["map"] = Value::of(zone.map);
                op.to["wander"] = Value::of(named ? 0 : 6);
                op.to["respawn"] = Value::of(120);
                code = Commit(player, *cs, a.level, op, p, id);
                if (code == Code::Ok)
                    ++s_nextTemp;
            }
            else if (rq.cmd == "MOVE")
            {
                auto mv = parseMove(f);
                if (!mv)
                    return refuse(Code::Decode);
                Target t;
                if (Code c = CheckTarget(char(mv->kind), mv->id, t); c != Code::Ok)
                    return refuse(c);
                if (t.zone != cs->zone)
                    return refuse(Code::Zone, "another zone's set");
                Pos p = mv->pos;
                Pos const old{t.data->posX, t.data->posY, t.data->posZ, t.data->orientation};
                ZoneInfo const* at = ZoneAt(t.data->mapid, p);
                if (!at || at->key != cs->zone || !ZoneAt(t.data->mapid, old))
                    return refuse(Code::Zone);
                if (player->GetExactDist2d(old.x, old.y) > kPresence)
                    return refuse(Code::Presence, "the object");
                if (Code c = CheckPlace(player, t.data->mapid, p, mv->kind == Kind::Creature); c != Code::Ok)
                    return refuse(c);
                if (mv->kind == Kind::Npc)
                {
                    auto r = s_release.find(mv->id);
                    if (r != s_release.end())
                        if (Code c = checkNpcMove(r->second, p, a.level, l); c != Code::Ok)
                            return refuse(c);
                }
                else if (dist2d(old, p) > 0.5f)
                    if (Density d = CheckNeighbours(t.data->mapid, p, mv->id, l); d.code != Code::Ok)
                        return refuse(d.code);
                Op op;
                op.op = OpKind::Move;
                op.kind = mv->kind;
                op.target = mv->id;
                op.from = posObject(old);
                op.to = posObject(p);
                code = Commit(player, *cs, a.level, op, p, id);
            }
            else if (rq.cmd == "SET")
            {
                auto st = parseSet(f);
                if (!st)
                    return refuse(Code::Decode);
                if (st->kind != Kind::Creature)
                    return refuse(Code::ReadOnly, "NPCs move and turn only");
                Target t;
                if (Code c = CheckTarget('c', st->id, t); c != Code::Ok)
                    return refuse(c);
                if (t.zone != cs->zone)
                    return refuse(Code::Zone, "another zone's set");
                Pos const where{t.data->posX, t.data->posY, t.data->posZ, 0};
                if (player->GetMapId() != t.data->mapid || player->GetExactDist2d(where.x, where.y) > kPresence)
                    return refuse(Code::Presence);
                bool named = false;
                if (auto z = s_entries.find(cs->zone); z != s_entries.end())
                    if (auto e = z->second.find(t.data->id); e != z->second.end())
                        named = e->second;
                Op op;
                op.op = OpKind::Set;
                op.kind = Kind::Creature;
                op.target = st->id;
                if (st->field == SetField::Wander)
                {
                    if (Code c = checkWander(st->value, l); c != Code::Ok)
                        return refuse(c);
                    if (named && a.level != Level::Maintainer)
                        return refuse(Code::Named);
                    float const w = std::round(st->value * 10) / 10;
                    op.from["wander"] = Value::of(std::round(t.data->wander_distance * 10) / 10);
                    op.to["wander"] = Value::of(w);
                }
                else
                {
                    if (Code c = checkRespawn(st->value, named, a.level, l); c != Code::Ok)
                        return refuse(c);
                    op.from["respawn"] = Value::of(t.data->spawntimesecs);
                    op.to["respawn"] = Value::of(uint32(st->value));
                }
                if (op.from == op.to)
                {
                    SendOk(player, seq, std::to_string(st->id));
                    return;
                }
                code = Commit(player, *cs, a.level, op, where, id);
            }
            else if (rq.cmd == "DEL")
            {
                auto del = parseDel(f);
                if (!del)
                    return refuse(Code::Decode);
                if (del->kind != Kind::Creature)
                    return refuse(Code::ReadOnly, "NPCs are not removed");
                Target t;
                if (Code c = CheckTarget('c', del->id, t); c != Code::Ok)
                    return refuse(c);
                if (t.zone != cs->zone)
                    return refuse(Code::Zone, "another zone's set");
                Pos const where{t.data->posX, t.data->posY, t.data->posZ, t.data->orientation};
                if (player->GetMapId() != t.data->mapid || player->GetExactDist2d(where.x, where.y) > kPresence)
                    return refuse(Code::Presence);
                bool named = false;
                if (auto z = s_entries.find(cs->zone); z != s_entries.end())
                    if (auto e = z->second.find(t.data->id); e != z->second.end())
                        named = e->second;
                if (named && a.level != Level::Maintainer)
                    return refuse(Code::Named);
                Op op;
                op.op = OpKind::Delete;
                op.kind = Kind::Creature;
                op.target = del->id;
                op.from = StateOf(*t.data);
                op.from["map"] = Value::of(t.data->mapid);
                code = Commit(player, *cs, a.level, op, where, id);
            }
            else
                return refuse(Code::Decode, "unknown command");

            if (code != Code::Ok)
                return refuse(code);
            SendSet(player, *cs);
            SendOps(player, *cs);
            SendOk(player, seq, id);
            Audit(player, rq.cmd, Code::Ok, {{"cs", Value::of(cs->id)}, {"id", Value::str(id)}});
        }

        // --------------------------------------------------------------------------------------- loading and replay

        bool TableExists(char const* table, bool world)
        {
            QueryResult r = world ? WorldDatabase.Query("SHOW TABLES LIKE '{}'", table)
                                  : CharacterDatabase.Query("SHOW TABLES FROM `{}` LIKE '{}'", s_config.db, table);
            return r != nullptr;
        }

        void LoadAccess(uint32 only)
        {
            QueryResult r = only ? LoginDatabase.Query("SELECT `account_id`, `level`, `frozen` FROM `ef_editor_access` WHERE `account_id` = {}", only)
                                 : LoginDatabase.Query("SELECT `account_id`, `level`, `frozen` FROM `ef_editor_access`");
            std::map<uint32, Access> fresh;
            if (r)
                do
                {
                    Field* f = r->Fetch();
                    uint8 const level = f[1].Get<uint8>();
                    fresh[f[0].Get<uint32>()] = {Level(std::min<uint8>(level, 3)), f[2].Get<uint8>() != 0};
                } while (r->NextRow());
            if (only)
            {
                if (fresh.count(only))
                    s_access[only] = fresh[only];
                else
                    s_access.erase(only);
            }
            else
                s_access = std::move(fresh);
        }

        // Access lost or frozen: editor mode closes at once (the UI hides on ROLE 0); a frozen account's open sets are
        // reverted live and no longer replayed (§13.1).
        void EnforceAccess()
        {
            for (ObjectGuid const& g : std::vector<ObjectGuid>(s_editors.begin(), s_editors.end()))
            {
                Player* p = ObjectAccessor::FindPlayer(g);
                if (!p)
                    continue;
                Access const a = AccessOf(p->GetSession()->GetAccountId());
                if (a.level == Level::None || a.frozen)
                {
                    Session& s = SessionOf(p);
                    SetMode(p, s, false);
                    s.role = false;
                    s.active = 0;
                    SendBody(p, eft::body(0, "ROLE", {"0", "", "", ""}));
                    s_editors.erase(g);
                }
            }
            for (auto& [id, cs] : s_sets)
            {
                auto acc = s_access.find(cs.owner);
                bool const frozen = !s_config.local && acc != s_access.end() && acc->second.frozen;
                if (frozen && state::open(cs.state))
                {
                    bool anyLock = false;
                    for (auto const& [k, set] : s_locks)
                        anyLock = anyLock || set == cs.id;
                    if (anyLock)
                    {
                        RevertLive(cs);
                        Unlock(cs.id);
                        LOG_INFO("module", "Emberfall.Editor: {} of frozen account {} reverted live", CsName(cs.id), cs.owner);
                    }
                }
            }
        }

        // The site writes review states (§10): rejected sets are reverted live, the owner hears about every change.
        void RefreshStates()
        {
            if (s_sets.empty())
                return;
            QueryResult r = CharacterDatabase.Query("SELECT `id`, `state` FROM " + Table("ef_changeset") + " WHERE `updated_at` > NOW() - INTERVAL 1 DAY");
            if (!r)
                return;
            do
            {
                Field* f = r->Fetch();
                uint32 const id = f[0].Get<uint32>();
                std::string const st = f[1].Get<std::string>();
                auto it = s_sets.find(id);
                if (it == s_sets.end() || it->second.state == st || !state::known(st))
                    continue;
                ChangeSet& cs = it->second;
                bool const wasLive = state::replayed(cs.state);
                cs.state = st;
                if (wasLive && (st == state::Rejected || st == state::Withdrawn))
                {
                    RevertLive(cs);
                    Unlock(cs.id);
                }
                if (st == state::Accepted || st == state::Released)
                    Unlock(cs.id);
                if (Player* p = OnlineOwner(cs))
                    SendSet(p, cs);
                LOG_INFO("module", "Emberfall.Editor: {} is {} (site)", CsName(cs.id), st);
            } while (r->NextRow());
        }

        void LoadRelease()
        {
            s_zones.clear(), s_spawns.clear(), s_entries.clear(), s_release.clear();
            if (QueryResult r = WorldDatabase.Query("SELECT `zone`, `name`, `map`, `area`, `block_lo`, `block_hi` FROM `ef_editor_zone`"))
                do
                {
                    Field* f = r->Fetch();
                    ZoneInfo z{f[0].Get<std::string>(), f[1].Get<std::string>(), f[2].Get<uint32>(), f[3].Get<uint32>(), f[4].Get<uint32>(), f[5].Get<uint32>()};
                    s_zones[z.key] = z;
                } while (r->NextRow());
            if (QueryResult r = WorldDatabase.Query("SELECT `kind`, `guid`, `zone`, `editable` FROM `ef_editor_spawn`"))
                do
                {
                    Field* f = r->Fetch();
                    std::string const k = f[0].Get<std::string>();
                    s_spawns[{k.empty() ? 'c' : k[0], f[1].Get<uint32>()}] = {f[2].Get<std::string>(), f[3].Get<uint8>() != 0};
                } while (r->NextRow());
            if (QueryResult r = WorldDatabase.Query("SELECT `zone`, `entry`, `named` FROM `ef_editor_entry`"))
                do
                {
                    Field* f = r->Fetch();
                    s_entries[f[0].Get<std::string>()][f[1].Get<uint32>()] = f[2].Get<uint8>() != 0;
                } while (r->NextRow());
            for (auto const& [key, info] : s_spawns)
                if (CreatureData const* d = sObjectMgr->GetCreatureData(key.second))
                    s_release[key.second] = {d->posX, d->posY, d->posZ, d->orientation};
        }

        // §7.1 and §7.3: every accepted set not yet released (by id), then every open set of an account that is not
        // frozen, in the order of its first op; each op skipped when already in effect, a conflict marks its set.
        void ReplayAll()
        {
            s_sets.clear(), s_locks.clear();
            std::map<uint32, std::vector<Op>> ops;
            uint32 maxTemp = 0;
            if (QueryResult r = CharacterDatabase.Query("SELECT `changeset_id`, `seq`, `op`, `kind`, `target_id`, IFNULL(`temp_id`, 0), "
                    "IFNULL(`final_id`, 0), `from_json`, `to_json`, UNIX_TIMESTAMP(`created_at`), `undone` FROM " + Table("ef_op") +
                    " ORDER BY `changeset_id`, `seq`"))
                do
                {
                    Field* f = r->Fetch();
                    Op op;
                    auto kind = opKind(f[2].Get<std::string>());
                    std::string const k = f[3].Get<std::string>();
                    auto from = fromJson(f[7].Get<std::string>()), to = fromJson(f[8].Get<std::string>());
                    if (!kind || k.empty() || !from || !to)
                    {
                        LOG_ERROR("module", "Emberfall.Editor: op {}/{} does not parse; skipped", f[0].Get<uint32>(), f[1].Get<uint32>());
                        continue;
                    }
                    op.seq = f[1].Get<uint32>(), op.op = *kind, op.kind = Kind(k[0]), op.target = f[4].Get<uint32>();
                    op.tempId = f[5].Get<uint32>(), op.finalId = f[6].Get<uint32>(), op.from = *from, op.to = *to;
                    op.createdMs = f[9].Get<uint64>() * 1000, op.undone = f[10].Get<uint8>() != 0;
                    maxTemp = std::max(maxTemp, op.tempId);
                    ops[f[0].Get<uint32>()].push_back(op);
                } while (r->NextRow());
            s_nextTemp = std::max<uint32>(kTempIdBase, maxTemp + 1);

            uint32 maxSet = 0;
            if (QueryResult r = CharacterDatabase.Query("SELECT `id`, `owner`, IFNULL(`owner_name`, ''), `title`, IFNULL(`description`, ''), `zone`, `state` FROM " +
                    Table("ef_changeset") + " ORDER BY `id`"))
                do
                {
                    Field* f = r->Fetch();
                    ChangeSet cs;
                    cs.id = f[0].Get<uint32>(), cs.owner = f[1].Get<uint32>(), cs.ownerName = f[2].Get<std::string>();
                    cs.title = f[3].Get<std::string>(), cs.description = f[4].Get<std::string>(), cs.zone = f[5].Get<std::string>();
                    cs.state = f[6].Get<std::string>();
                    cs.ops = std::move(ops[cs.id]);
                    for (Op const& op : cs.ops)
                        if (cs.firstOpMs == 0 || op.createdMs < cs.firstOpMs)
                            cs.firstOpMs = op.createdMs;
                    maxSet = std::max(maxSet, cs.id);
                    if (state::replayed(cs.state) || state::open(cs.state))
                        s_sets[cs.id] = std::move(cs);
                } while (r->NextRow());
            s_nextSet = maxSet + 1;

            std::vector<ChangeSet*> order;
            for (auto& [id, cs] : s_sets)
                if (cs.state == state::Accepted)
                    order.push_back(&cs);
            std::vector<ChangeSet*> open;
            for (auto& [id, cs] : s_sets)
            {
                auto acc = s_access.find(cs.owner);
                bool const frozen = !s_config.local && acc != s_access.end() && acc->second.frozen;
                if (state::open(cs.state) && !frozen)
                    open.push_back(&cs);
            }
            std::stable_sort(open.begin(), open.end(), [](ChangeSet* a, ChangeSet* b) { return a->firstOpMs < b->firstOpMs; });
            order.insert(order.end(), open.begin(), open.end());

            // a temporary id whose add shipped in a release under its final id: later ops follow the final id
            std::map<uint32, uint32> finals;
            for (ChangeSet* cs : order)
                for (Op const& op : cs->ops)
                    if (op.op == OpKind::Add && op.finalId && sObjectMgr->GetCreatureData(op.finalId))
                        finals[op.tempId] = op.finalId;

            uint32 applied = 0, skipped = 0, conflicts = 0;
            for (ChangeSet* cs : order)
            {
                bool conflict = false;
                for (Op op : cs->ops)
                {
                    if (op.undone)
                        continue;
                    if (auto fin = finals.find(op.target); fin != finals.end())
                    {
                        if (op.op == OpKind::Add)
                        {
                            ++skipped;   // released
                            continue;
                        }
                        op.target = fin->second;
                    }
                    CreatureData const* d = sObjectMgr->GetCreatureData(op.target);
                    Replay how = Replay::Apply;
                    if (op.op == OpKind::Add)
                        how = d ? Replay::AlreadyDone : Replay::Apply;
                    else if (op.op == OpKind::Delete)
                    {
                        if (!d)
                            how = Replay::AlreadyDone;
                        else if (auto was = posOf(op.from); was && dist2d(*was, {d->posX, d->posY, 0, 0}) > 0.5f && !IsTemp(op.target))
                            how = Replay::Conflict;
                    }
                    else if (!d)
                        how = Replay::Conflict;
                    else
                        how = replayState(op, StateOf(*d));
                    if (how == Replay::Apply && Apply(op))
                        ++applied;
                    else if (how == Replay::Conflict)
                    {
                        ++conflicts;
                        conflict = true;
                        LOG_WARN("module", "Emberfall.Editor: {} op {} ({} {}) meets a newer state; skipped", CsName(cs->id), op.seq,
                            opName(op.op), op.target);
                    }
                    else
                        ++skipped;
                    if (state::open(cs->state) && how != Replay::Conflict)
                        s_locks[{char(op.kind), op.target}] = cs->id;
                }
                if (conflict && state::open(cs->state) && cs->state != state::Conflict)
                {
                    cs->state = std::string(state::Conflict);
                    StoreState(*cs);
                }
            }
            CharacterDatabase.DirectExecute("DELETE FROM " + Table("ef_lock"));
            for (auto const& [key, set] : s_locks)
                CharacterDatabase.DirectExecute("INSERT INTO " + Table("ef_lock") + " (`kind`, `target_id`, `changeset_id`) VALUES ('{}', {}, {})",
                    std::string(1, key.first), key.second, set);

            // the day's rate windows survive a restart (ops' server timestamps)
            s_rates.clear();
            uint64 const now = NowMs();
            for (auto const& [id, cs] : s_sets)
                for (Op const& op : cs.ops)
                    if (op.createdMs + 86400000 > now)
                    {
                        Rate& r = s_rates[cs.owner];
                        r.day.add(op.createdMs);
                        if (op.op == OpKind::Add)
                            r.adds.add(op.createdMs);
                        if (op.op == OpKind::Delete)
                            r.deletes.add(op.createdMs);
                    }
            LOG_INFO("server.loading", ">> Emberfall.Editor: {} zone(s), {} editable spawn(s), {} set(s) replayed: {} op(s) applied, {} already in effect, {} conflict(s)",
                s_zones.size(), s_spawns.size(), order.size(), applied, skipped, conflicts);
        }
    }

    void ReadConfig(bool /*reload*/)
    {
        s_config.enable = sConfigMgr->GetOption<bool>("Emberfall.Editor.Enable", false);
        s_config.db = sConfigMgr->GetOption<std::string>("Emberfall.Editor.Database", "");
        s_config.local = sConfigMgr->GetOption<std::string>("Emberfall.Editor.Mode", "realm") == "local";
        s_config.refreshMs = 1000 * std::max<uint32>(5, sConfigMgr->GetOption<uint32>("Emberfall.Editor.RefreshSeconds", 60));
        for (char c : s_config.db)
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
            {
                LOG_ERROR("module", "Emberfall.Editor.Database '{}' is not a plain database name: the editor stays off", s_config.db);
                s_config.db.clear();
                break;
            }
    }

    bool Enabled() { return s_ready; }

    void OnStartup()
    {
        s_ready = false;
        if (!s_config.enable)
            return;
        if (s_config.db.empty())
        {
            LOG_ERROR("module", "Emberfall.Editor.Enable = 1 without Emberfall.Editor.Database: the editor stays off");
            return;
        }
        for (char const* t : {"ef_changeset", "ef_op", "ef_lock", "ef_audit"})
            if (!TableExists(t, false))
            {
                LOG_ERROR("module", "Emberfall.Editor: {}.{} is missing (data/sql/db-editor): the editor stays off", s_config.db, t);
                return;
            }
        for (char const* t : {"ef_editor_zone", "ef_editor_spawn", "ef_editor_entry"})
            if (!TableExists(t, true))
            {
                LOG_ERROR("module", "Emberfall.Editor: world table {} is missing (dist/server/editor_world.sql): the editor stays off", t);
                return;
            }
        if (!s_config.local && !LoginDatabase.Query("SHOW TABLES LIKE 'ef_editor_access'"))
        {
            LOG_ERROR("module", "Emberfall.Editor: auth table ef_editor_access is missing: the editor stays off");
            return;
        }
        LoadRelease();
        if (!s_config.local)
            LoadAccess(0);
        ReplayAll();
        s_ready = true;
        LOG_INFO("server.loading", ">> Emberfall.Editor: on ({} mode, database {})", s_config.local ? "local" : "realm", s_config.db);
    }

    void OnUpdate(uint32 diff)
    {
        if (!s_ready)
            return;
        s_refreshTimer += diff;
        if (s_refreshTimer < s_config.refreshMs)
            return;
        s_refreshTimer = 0;
        Refresh(0);
    }

    void Refresh(uint32 accountId)
    {
        if (!s_ready)
            return;
        if (!s_config.local)
            LoadAccess(accountId);
        EnforceAccess();
        RefreshStates();
    }

    void OnLogin(Player* /*player*/) { }

    void OnLogout(Player* player)
    {
        if (!s_ready)
            return;
        Session& s = SessionOf(player);
        SetMode(player, s, false);
        s_editors.erase(player->GetGUID());
    }

    void HandleForeign(Player* player)
    {
        if (!s_ready)
            return;
        LOG_WARN("module", "Emberfall.Editor: account {} ({}) sent an EFE message to someone else: refused", player->GetSession()->GetAccountId(),
            player->GetName());
        Audit(player, "FOREIGN", Code::Access, {});
    }

    void HandleMessage(Player* player, std::string_view body)
    {
        if (!s_ready)
            return; // the editor does not exist here: no answer, ever (§8.3)
        uint32 const account = player->GetSession()->GetAccountId();
        Access const a = AccessOf(account);
        eft::ParseResult const parsed = eft::parse(body);
        if (a.level == Level::None || a.frozen)
        {
            // HELLO from a non-editor: silence (the UI stays hidden). Anything else is a forged or patched client.
            Code const code = Code::Access;
            if (!parsed.ok() || parsed.request.cmd != "HELLO")
            {
                LOG_WARN("module", "Emberfall.Editor: account {} ({}) without editor access sent '{}': refused", account, player->GetName(),
                    parsed.ok() ? parsed.request.cmd : std::string("?"));
                SendErr(player, parsed.seq, code);
            }
            Audit(player, parsed.ok() ? parsed.request.cmd : "DECODE", code, {{"frozen", Value::of(a.frozen ? 1 : 0)}});
            return;
        }
        Session& s = SessionOf(player);
        if (!s.requests.allow(NowMs()))
        {
            SendErr(player, parsed.seq, Code::Rate, "requests");
            return;
        }
        if (!parsed.ok())
        {
            SendErr(player, parsed.seq, Code::Decode);
            Audit(player, "DECODE", Code::Decode, {});
            return;
        }
        eft::Request const& rq = parsed.request;
        if (rq.cmd == "HELLO")
        {
            s.role = true;
            s_editors.insert(player->GetGUID());
            SendRole(player, a);
            for (auto const& [id, cs] : s_sets)
                if (cs.owner == account && (state::open(cs.state) || cs.state == state::Accepted))
                    SendSet(player, cs);
            SendOk(player, rq.seq);
            Audit(player, "HELLO", Code::Ok, {{"level", Value::of(uint32(a.level))}});
            return;
        }
        if (!s.role)
        {
            SendErr(player, rq.seq, Code::State, "HELLO first");
            return;
        }
        Handle(player, s, a, rq);
    }

    void Status(std::vector<std::string>& lines)
    {
        lines.push_back(std::string("Emberfall world editor: ") + (s_ready ? "on" : "off") + (s_config.local ? " (local)" : ""));
        if (!s_ready)
            return;
        size_t open = 0;
        for (auto const& [id, cs] : s_sets)
            open += state::open(cs.state);
        lines.push_back(fmt::format("zones {}, editable spawns {}, accounts with access {}, sets live {} ({} open), locks {}, editors online {}",
            s_zones.size(), s_spawns.size(), s_access.size(), s_sets.size(), open, s_locks.size(), s_editors.size()));
        for (auto const& [id, cs] : s_sets)
            lines.push_back(fmt::format("  {} {} by {} ({}): {} op(s), {}", CsName(cs.id), cs.state, cs.ownerName, cs.owner, cs.ops.size(), cs.title));
    }
}
