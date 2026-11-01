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
//
// On the normal realm (§19): the role is the account's GM level (account_access, this realm or -1): 1 contributor, 2
// and up maintainer. A maintainer's set is `live`: its ops change the world for everyone at once (as on the build
// realm). A contributor's open set is a draft: what it adds, and a copy of every release spawn it moves or changes,
// stand in the editor phase (kDraftPhase), seen only by editors with the editor open (VIEW on), and a spawn it removes
// is hidden from those editors only (EmberfallVisibility.h); everyone else sees the release world. Accepted (the site's
// review state), the set applies for everyone; rejected or withdrawn, the drafts go.
//
// A spawn of several creatures (`SET;c;<id>;count;<n>`, Emberfall world-editor.md §20): the spawn and n-1 others on a
// ring round its point, each a spawn of its own under a GUID derived from the spawn's (EfEditorLogic.h extraId), made
// from its data and kept with it (SetCount; a move, the wander radius, the respawn and a removal take them along). The
// release compile writes the same ones into world.sql from the row's `count`, so replay finds the count in effect.

#include "EfEditor.h"
#include "EfEditorLogic.h"

#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "CreatureData.h"
#include "DataMap.h"
#include "DatabaseEnv.h"
#include "EmberfallVisibility.h"
#include "GameGraveyard.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PassiveAI.h"
#include "Player.h"
#include "Realm.h"
#include "SharedDefines.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

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
            bool live = true;   // a maintainer's (applies for everyone at once); false: a contributor's draft (§19)
        };
        std::map<uint32, ChangeSet> s_sets;
        std::map<std::pair<char, uint32>, uint32> s_locks;                // object -> its open set
        std::map<uint32, CreatureData> s_backup;                          // deleted spawns' data (undo, withdraw)

        // The drafts (§19): release spawn -> its draft copy in the editor phase; the release spawns a draft removes
        // (hidden from viewers only); the players whose editor is open (they are in the editor phase).
        std::map<uint32, uint32> s_copies;
        std::map<uint32, uint32> s_copyOf;                                // copy -> release spawn
        std::set<uint32> s_hidden;
        std::set<ObjectGuid> s_viewers;
        uint32 s_nextCopy = kCopyIdBase;

        struct Access
        {
            Level level = Level::None;
            bool frozen = false;
        };
        std::map<uint32, Access> s_access;
        std::map<uint32, Rate> s_rates;
        std::set<ObjectGuid> s_editors;                                   // players who got ROLE this session
        uint32 s_nextTemp = kTempIdBase, s_nextSet = 1, s_refreshTimer = 0, s_tickTimer = 0;

        class Session : public DataMap::Base
        {
        public:
            bool role = false, mode = false, view = false;
            uint32 active = 0, scanId = 0;
            eft::Reassembly submit;
            eft::RateLimit requests{40, 10000};
            float savedSpeed = 1.0f;
            // where editor mode was turned on (a contributor goes back there when it goes off, §19)
            bool hasReturn = false, returnDue = false;
            uint32 returnMap = 0;
            Pos returnPos;
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
        bool IsCopy(uint32 id) { return id >= kCopyIdBase && id <= kCopyIdMax; }
        // the other creatures of a spawn of several (count, EfEditorLogic.h extraId): never edited themselves
        bool IsExtra(uint32 id) { return extraOf(id).has_value(); }

        CreatureData* DataOf(uint32 id) { return id ? const_cast<CreatureData*>(sObjectMgr->GetCreatureData(id)) : nullptr; }

        // A spawn's creatures: 1 and every other one that exists, slot after slot.
        uint32 CountOf(uint32 id)
        {
            uint32 n = 1;
            while (n < kMaxCount && DataOf(extraId(id, n)))
                ++n;
            return n;
        }

        uint32 CountIn(Fields const& f)
        {
            auto c = f.find("count");
            return c != f.end() && c->second.type == Value::Type::Number && c->second.number >= 1 ? uint32(c->second.number) : 1;
        }

        // The spawn and its other creatures.
        std::vector<uint32> GroupIds(uint32 id)
        {
            std::vector<uint32> out{id};
            for (uint32 slot = 1; slot < kMaxCount; ++slot)
                if (uint32 const e = extraId(id, slot); e && DataOf(e))
                    out.push_back(e);
            return out;
        }

        // The ground under a point when its grid is loaded (vmaps on), else the given Z (the unit is put on its ground
        // when it loads: OnCreatureAdd).
        float GroundZ(uint32 mapId, uint32 phase, float x, float y, float z)
        {
            Map* map = LiveMap(mapId);
            if (!map || !map->IsGridLoaded(x, y))
                return z;
            float const g = map->GetHeight(phase | PHASEMASK_NORMAL, x, y, z + 3.0f, true, 10.0f);
            return std::isfinite(g) && g > INVALID_HEIGHT + 1.0f && std::fabs(g - z) <= 2 * kGroundTolerance ? std::round(g * 100) / 100 : z;
        }

        // Whether a set's ops are in the world for everyone: a maintainer's, every set in local mode, an accepted one.
        bool Live(ChangeSet const& cs) { return s_config.local || cs.live || !state::open(cs.state); }

        bool Viewing(Player const* p) { return p && s_viewers.count(p->GetGUID()); }

        // `id`: the spawn's own id (a draft copy's for its data), so the count is its own.
        Fields StateOf(uint32 id, CreatureData const& d)
        {
            Fields o = posObject({d.posX, d.posY, d.posZ, d.orientation});
            o["wander"] = Value::of(std::round(d.wander_distance * 10) / 10);
            o["respawn"] = Value::of(d.spawntimesecs);
            o["entry"] = Value::of(d.id);
            o["count"] = Value::of(CountOf(id));
            return o;
        }

        void RelocateOne(uint32 id, CreatureData& data, Pos const& p)
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

        // A spawn moved: its other creatures keep their places on the ring round it.
        void Relocate(uint32 id, CreatureData& data, Pos const& p)
        {
            RelocateOne(id, data, p);
            for (uint32 slot = 1; slot < kMaxCount; ++slot)
                if (CreatureData* e = DataOf(extraId(id, slot)))
                {
                    Pos q = slotPos(p, slot);
                    q.z = GroundZ(data.mapid, data.phaseMask, q.x, q.y, p.z);
                    RelocateOne(extraId(id, slot), *e, q);
                }
        }

        void SetWander(uint32 id, CreatureData& data, float wander)
        {
            for (uint32 gid : GroupIds(id))
            {
                CreatureData& d = gid == id ? data : *DataOf(gid);
                d.wander_distance = wander;
                d.movementType = wander > 0 ? RANDOM_MOTION_TYPE : IDLE_MOTION_TYPE;
                for (Creature* c : LiveCreatures(d, gid))
                {
                    c->SetWanderDistance(wander);
                    c->SetDefaultMovementType(MovementGeneratorType(d.movementType));
                    if (c->IsAlive())
                        c->GetMotionMaster()->Initialize();
                }
            }
        }

        void SetRespawn(uint32 id, CreatureData& data, uint32 seconds)
        {
            for (uint32 gid : GroupIds(id))
            {
                CreatureData& d = gid == id ? data : *DataOf(gid);
                d.spawntimesecs = seconds;
                for (Creature* c : LiveCreatures(d, gid))
                    c->SetRespawnDelay(seconds);
            }
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

        // A spawn that the editor creates (an add, an undone delete, a draft copy): its data, then the unit when the grid
        // is loaded. The id has no data, so a unit of it on the map is one on its way out (a draft made live, a redo):
        // the new one loads beside it.
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
                if (!c->LoadCreatureFromDB(id, map, true, true))
                {
                    delete c;
                    LOG_WARN("module", "Emberfall.Editor: spawn {} has data but its unit did not load", id);
                }
            }
            return true;
        }

        CreatureData NewData(uint32 entry, uint32 mapId, Pos const& p, float wander, uint32 respawn, uint32 phase = PHASEMASK_NORMAL)
        {
            CreatureData d;
            d.id = entry;
            d.mapid = uint16(mapId);
            d.phaseMask = phase;
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

        void DespawnOne(uint32 id)
        {
            CreatureData const* data = sObjectMgr->GetCreatureData(id);
            if (!data)
                return;
            for (Creature* c : LiveCreatures(*data, id))
            {
                c->CombatStop(true);
                c->AddObjectToRemoveList();
            }
            sObjectMgr->DeleteCreatureData(id);
        }

        // A spawn removed, its other creatures with it (the op's `from` keeps the count for an undo).
        void Despawn(uint32 id, bool backup = true)
        {
            CreatureData const* data = sObjectMgr->GetCreatureData(id);
            if (!data)
                return;
            if (backup)
                Backup(id, *data);
            for (uint32 slot = 1; slot < kMaxCount; ++slot)
                DespawnOne(extraId(id, slot));
            DespawnOne(id);
        }

        // A spawn of `count` creatures (EfEditorLogic.h, count): the others it lacks are made from its data on their
        // slots of the ring, the ones over the count go.
        void SetCount(uint32 id, CreatureData const& data, uint32 count)
        {
            Pos const at{data.posX, data.posY, data.posZ, data.orientation};
            for (uint32 slot = 1; slot < kMaxCount; ++slot)
            {
                uint32 const eid = extraId(id, slot);
                if (!eid)
                    return;
                bool const has = DataOf(eid) != nullptr;
                if (slot < count && !has)
                {
                    CreatureData e;
                    Assign(e, data);
                    Pos const q = slotPos(at, slot);
                    e.posX = q.x, e.posY = q.y, e.orientation = q.o;
                    e.posZ = GroundZ(data.mapid, data.phaseMask, q.x, q.y, at.z);
                    e.poolId = 0, e.spawnGroupId = 0, e.dbData = false;
                    Spawn(eid, e);
                }
                else if (slot >= count && has)
                    DespawnOne(eid);
            }
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
                    CreatureData d;
                    if (s_backup.count(id) && s_backup[id].id == uint32(entry->second.number))
                    {
                        Assign(d, s_backup[id]);   // an undone delete: the spawn as it was, where `to` says
                        d.posX = p->x, d.posY = p->y, d.posZ = p->z, d.orientation = p->o;
                    }
                    else
                    {
                        auto w = op.to.find("wander");
                        auto r = op.to.find("respawn");
                        float const wander = w != op.to.end() ? float(w->second.number) : 0.0f;
                        uint32 const respawn = r != op.to.end() ? uint32(r->second.number) : 120;
                        Assign(d, NewData(uint32(entry->second.number), uint32(map->second.number), *p, wander, respawn));
                    }
                    if (!Spawn(id, d))
                        return false;
                    if (CountIn(op.to) > 1)
                        SetCount(id, d, CountIn(op.to));   // an undone delete of a spawn of several
                    return true;
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
                        else if (k == "count" && v.type == Value::Type::Number)
                            SetCount(id, *data, std::clamp<uint32>(uint32(v.number), 1, kMaxCount));
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

        // --------------------------------------------------------------------------------------- drafts (§19)

        // The open draft (a contributor's set) that holds this object, if any.
        ChangeSet* DraftSetOf(char kind, uint32 id)
        {
            auto lock = s_locks.find({kind, id});
            if (lock == s_locks.end())
                return nullptr;
            auto it = s_sets.find(lock->second);
            return it != s_sets.end() && !Live(it->second) ? &it->second : nullptr;
        }

        std::vector<Op> OpsOn(ChangeSet const& cs, uint32 id)
        {
            std::vector<Op> out;
            for (Op const& op : cs.ops)
                if (op.target == id)
                    out.push_back(op);
            return out;
        }

        // A release spawn's units shown or hidden again for the viewers (a draft removes it, or no longer does).
        void UpdateVisibility(uint32 id)
        {
            for (uint32 gid : GroupIds(id))
                if (CreatureData const* d = sObjectMgr->GetCreatureData(gid))
                    for (Creature* c : LiveCreatures(*d, gid))
                        c->UpdateObjectVisibility();
        }

        // An existing spawn's data and units brought to a state (position, wander, respawn), in place.
        void SetState(uint32 id, CreatureData& data, Fields const& st)
        {
            if (auto p = posOf(st); p && !posNear(StateOf(id, data), st))
                Relocate(id, data, *p);
            if (auto w = st.find("wander"); w != st.end() && w->second.type == Value::Type::Number &&
                std::fabs(data.wander_distance - w->second.number) > 0.05)
                SetWander(id, data, float(w->second.number));
            if (auto r = st.find("respawn"); r != st.end() && r->second.type == Value::Type::Number &&
                data.spawntimesecs != uint32(r->second.number))
                SetRespawn(id, data, uint32(r->second.number));
            if (uint32 const n = CountIn(st); n != CountOf(id))
                SetCount(id, data, n);
        }

        // The editor phase made to show what the open draft holding this object makes of it: nothing, a copy of the
        // release spawn where the draft has it, the spawn the draft adds, or the release spawn hidden from viewers.
        // Called after every change of a draft (an op, undo, redo, withdraw, reject, accept, freeze, the replay).
        // Returns false on a conflict (the draft meets a newer state; the op is not drafted).
        bool RebuildTarget(char kind, uint32 id)
        {
            ChangeSet* cs = DraftSetOf(kind, id);
            std::vector<Op> const ops = cs ? OpsOn(*cs, id) : std::vector<Op>{};
            if (IsTemp(id))
            {
                // a spawn a draft adds stands in the editor phase itself; a live set's add is not a draft's
                CreatureData* data = const_cast<CreatureData*>(sObjectMgr->GetCreatureData(id));
                if (data && !(data->phaseMask & kDraftPhase))
                    return true;
                Draft const d = foldDraft(std::nullopt, ops);
                if (!d.present)
                {
                    if (data)
                        Despawn(id, false);
                    return !d.conflict;
                }
                if (data)
                {
                    SetState(id, *data, d.state);
                    return !d.conflict;
                }
                auto p = posOf(d.state);
                auto entry = d.state.find("entry"), map = d.state.find("map");
                if (!p || entry == d.state.end() || map == d.state.end())
                    return false;
                auto w = d.state.find("wander");
                auto r = d.state.find("respawn");
                CreatureData const nd = NewData(uint32(entry->second.number), uint32(map->second.number), *p,
                    w != d.state.end() ? float(w->second.number) : 0.0f, r != d.state.end() ? uint32(r->second.number) : 120, kDraftPhase);
                if (Spawn(id, nd) && CountIn(d.state) > 1)
                    SetCount(id, nd, CountIn(d.state));
                return !d.conflict;
            }
            bool const wasHidden = s_hidden.erase(id) > 0;
            CreatureData const* orig = sObjectMgr->GetCreatureData(id);
            Draft d;
            if (cs && orig)
                d = foldDraft(StateOf(id, *orig), ops);
            bool const hide = cs && orig && !d.present;
            bool const needCopy = cs && orig && d.present && d.changed;
            auto copy = s_copies.find(id);
            if (needCopy && copy != s_copies.end())
            {
                if (CreatureData* cd = const_cast<CreatureData*>(sObjectMgr->GetCreatureData(copy->second)))
                    SetState(copy->second, *cd, d.state);
            }
            else if (needCopy && s_nextCopy <= kCopyIdMax)
            {
                uint32 const cid = s_nextCopy++;
                CreatureData cd;
                Assign(cd, *orig);
                cd.phaseMask = kDraftPhase;
                cd.poolId = 0, cd.spawnGroupId = 0, cd.dbData = false;
                if (auto p = posOf(d.state))
                    cd.posX = p->x, cd.posY = p->y, cd.posZ = p->z, cd.orientation = p->o;
                if (auto w = d.state.find("wander"); w != d.state.end())
                {
                    cd.wander_distance = float(w->second.number);
                    cd.movementType = cd.wander_distance > 0 ? RANDOM_MOTION_TYPE : IDLE_MOTION_TYPE;
                }
                if (auto r = d.state.find("respawn"); r != d.state.end())
                    cd.spawntimesecs = uint32(r->second.number);
                if (Spawn(cid, cd) && CountIn(d.state) > 1)
                    SetCount(cid, cd, CountIn(d.state));   // the draft's creatures, all of them in the editor phase
                s_copies[id] = cid;
                s_copyOf[cid] = id;
            }
            else if (!needCopy && copy != s_copies.end())
            {
                Despawn(copy->second, false);
                s_copyOf.erase(copy->second);
                s_copies.erase(copy);
            }
            if (hide)
                s_hidden.insert(id);
            if (hide != wasHidden)
                UpdateVisibility(id);
            return !(cs && (!orig || d.conflict));
        }

        // The data an op on this object starts from: its draft copy's while a draft has one.
        uint32 EffectiveId(uint32 id)
        {
            auto copy = s_copies.find(id);
            return copy != s_copies.end() && sObjectMgr->GetCreatureData(copy->second) ? copy->second : id;
        }

        CreatureData* EffectiveData(uint32 id)
        {
            if (auto copy = s_copies.find(id); copy != s_copies.end())
                if (CreatureData const* cd = sObjectMgr->GetCreatureData(copy->second))
                    return const_cast<CreatureData*>(cd);
            return const_cast<CreatureData*>(sObjectMgr->GetCreatureData(id));
        }

        // Every object of a set rebuilt (after its state changed: withdrawn, rejected, accepted, frozen).
        void RebuildSet(ChangeSet const& cs)
        {
            std::set<std::pair<char, uint32>> targets;
            for (Op const& op : cs.ops)
                if (!isOverlay(op.kind))
                    targets.insert({char(op.kind), op.target});
            for (auto const& [k, id] : targets)
                RebuildTarget(k, id);
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

        // A spawn's marker as the viewer sees it. With a draft copy (§19) the marker is the draft (its place, its unit)
        // and carries the original's place for the ghost ring; a spawn a draft removes is flagged so.
        Marker MarkerOf(uint32 viewer, uint32 id, CreatureData const& data)
        {
            Marker m;
            char const k = KindOf(id, data);
            m.kind = k == 'n' ? Kind::Npc : Kind::Creature;
            m.id = id, m.entry = data.id;
            CreatureData const* shown = &data;
            uint32 shownId = id;
            if (auto copy = s_copies.find(id); copy != s_copies.end())
                if (CreatureData const* cd = sObjectMgr->GetCreatureData(copy->second))
                {
                    shown = cd, shownId = copy->second;
                    m.ghost = Pos{data.posX, data.posY, data.posZ, data.orientation};
                }
            m.pos = {shown->posX, shown->posY, shown->posZ, shown->orientation};
            m.wander = shown->wander_distance, m.respawn = shown->spawntimesecs;
            m.flags = FlagsFor(viewer, k, id, data);
            if (m.ghost)
                m.flags += flag::Ghost;
            if (s_hidden.count(id))
                m.flags += flag::Removed;
            for (Creature* c : LiveCreatures(*shown, shownId))
                if (c->IsAlive())
                {
                    m.lowguid = c->GetGUID().GetCounter();
                    break;
                }
            // a spawn of several: the count and the others' living units (the overlay rings them, Select target finds them)
            m.count = CountOf(shownId);
            for (uint32 slot = 1; slot < m.count; ++slot)
                if (CreatureData const* e = sObjectMgr->GetCreatureData(extraId(shownId, slot)))
                    for (Creature* c : LiveCreatures(*e, extraId(shownId, slot)))
                        if (c->IsAlive())
                        {
                            m.others.push_back(c->GetGUID().GetCounter());
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

        // CS: a set's state for its owner; `active` (sixth field 1) makes it the client's active set (one the realm
        // opened by itself for a change made without one).
        void SendSet(Player* player, ChangeSet const& cs, bool active = false)
        {
            size_t n = 0;
            for (Op const& op : cs.ops)
                n += !op.undone;
            std::vector<std::string> f{std::to_string(cs.id), cs.state, std::to_string(n), eft::encodeName(cs.title)};
            if (active)
                f.push_back("1");
            SendBody(player, eft::body(0, "CS", f));
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
                if (isPlace(op.kind))
                {
                    // a prop or a building (§22): <model>[:<yards moved>:<yaw>]
                    auto f = placeStateOf(op.from), t = placeStateOf(op.to);
                    detail = t ? t->model : f ? f->model : std::string();
                    if (op.op == OpKind::Move && f && t)
                        detail += ":" + fixed(std::hypot(t->u - f->u, t->v - f->v), 1) + ":" + fixed(t->yaw, 0);
                    entries.push_back(std::to_string(op.seq) + "," + opName(op.op) + "," + std::string(1, char(op.kind)) + "," +
                                      std::to_string(op.target) + ",0," + detail + "," + (op.undone ? "1" : "0"));
                    continue;
                }
                if (isGround(op.kind))
                {
                    // a stroke: <layer|mode>:<radius|class>:<length yd>
                    if (auto st = strokeOf(op.kind, op.to))
                        detail = (op.kind == Kind::Height ? std::string(sculptName(st->mode)) : st->layer) + ":" +
                                 (op.kind == Kind::Road ? st->cls : fixed(st->radius, 1)) + ":" + fixed(pathLength(st->pts), 0);
                    entries.push_back(std::to_string(op.seq) + ",add," + std::string(1, char(op.kind)) + "," +
                                      std::to_string(op.target) + ",0," + detail + "," + (op.undone ? "1" : "0"));
                    continue;
                }
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

        // Every object of a set broadcast as it stands now.
        void BroadcastSet(ChangeSet const& cs)
        {
            std::set<std::pair<char, uint32>> done;
            for (Op const& op : cs.ops)
            {
                if (isOverlay(op.kind) || !done.insert({char(op.kind), op.target}).second)
                    continue;
                Pos at = posOf(op.to).value_or(posOf(op.from).value_or(Pos{}));
                uint32 mapId = 0;
                if (CreatureData const* d = sObjectMgr->GetCreatureData(op.target))
                    at = {d->posX, d->posY, d->posZ, 0}, mapId = d->mapid;
                else if (auto m = op.to.find("map"); m != op.to.end())
                    mapId = uint32(m->second.number);
                else if (auto m2 = op.from.find("map"); m2 != op.from.end())
                    mapId = uint32(m2->second.number);
                else if (auto z = s_zones.find(cs.zone); z != s_zones.end())
                    mapId = z->second.map;
                Broadcast(char(op.kind), op.target, mapId, at);
            }
        }

        // Reverts a set's active ops live, newest first (withdraw, reject, freeze). A draft (`wasDraft`: a contributor's
        // set while it was open) never changed the world: it is unlocked and its drafts go.
        void RevertLive(ChangeSet& cs, bool wasDraft)
        {
            if (wasDraft)
            {
                Unlock(cs.id);
                RebuildSet(cs);
                BroadcastSet(cs);
                return;
            }
            for (size_t i = cs.ops.size(); i-- > 0;)
            {
                Op const& op = cs.ops[i];
                if (op.undone || isOverlay(op.kind))
                    continue;   // a stroke never changed the server's world (GroundSyncSet takes it off the clients)
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

        // --------------------------------------------------------------------------------------- ground (step 3, §20)

        // Ground strokes (kinds l, r, h) change nothing on the server: they are kept by tile and streamed to every
        // player's client (GND), which paints them over its own ground (a ghost for heights) until a release bakes them.
        // A maintainer's (live) and an accepted set's strokes go to everyone; a contributor's draft only to the editors
        // with the editor open. Every player keeps, per tile, the newest stroke it has been sent (a serial), so a player
        // walking into a tile gets what it lacks, a few dozen messages a second at most.
        std::map<std::string, std::map<std::string, std::string>> s_layers;   // zone -> paintable layer -> tileset
        struct GroundRec
        {
            uint32 cs = 0, seq = 0, owner = 0, map = 0;
            std::pair<int32, int32> tile;
            uint64 serial = 0;
            bool draft = false;
            Stroke stroke;
            std::string token;      // <tileset>/<layer> (l, r)
        };
        using GroundKey = std::pair<uint32, uint32>;                        // (cs, seq)
        using TileKey = std::tuple<uint32, int32, int32>;                    // (map, tile x, tile y)
        std::map<GroundKey, GroundRec> s_ground;
        std::map<TileKey, std::map<uint64, GroundKey>> s_groundTiles;
        uint64 s_groundSerial = 0;
        uint32 s_nextGround = 1;
        std::map<uint32, std::pair<Window, Window>> s_groundRates;           // account -> (minute, day)
        constexpr uint32 kGroundBudget = 60;                                 // GND messages per player and second
        constexpr char const* kGroundKey = "emberfall_editor_ground";

        class GroundView : public DataMap::Base
        {
        public:
            uint32 map = UINT32_MAX;
            bool viewer = false;
            // the client said HELLO (every client does, ~2 s after entering the world): before it, whispers can come
            // before the client takes them, and a stroke lost then would never come again
            bool ready = false;
            std::map<std::pair<int32, int32>, uint64> tiles;   // tile -> newest serial sent
            std::map<std::pair<int32, int32>, uint64> placeTiles;   // the same for the props and buildings (step 2, §22)
        };

        GroundView& GroundViewOf(Player* p) { return *p->CustomData.GetDefault<GroundView>(kGroundKey); }

        bool Frozen(uint32 account)
        {
            auto acc = s_access.find(account);
            return !s_config.local && acc != s_access.end() && acc->second.frozen;
        }

        // Whether a set's strokes are shown at all: accepted (not yet released) or open, and its owner not frozen.
        bool GroundShown(ChangeSet const& cs) { return state::replayed(cs.state) && !Frozen(cs.owner); }

        std::string GroundToken(std::string const& zone, Stroke const& s)
        {
            if (s.kind == Kind::Height)
                return {};
            auto z = s_layers.find(zone);
            std::string tileset = "Accord";
            if (z != s_layers.end())
                if (auto l = z->second.find(s.layer); l != z->second.end() && !l->second.empty())
                    tileset = l->second;
            return tileset + "/" + s.layer;
        }

        std::string GroundFlags(GroundRec const& r, uint32 viewer)
        {
            std::string f;
            if (r.draft)
                f += 'd';
            if (r.owner == viewer)
                f += 'm';
            return f.empty() ? std::string("-") : f;
        }

        void SendGround(Player* p, GroundRec const& r)
        {
            std::string const body = groundBody(r.cs, r.seq, GroundFlags(r, p->GetSession()->GetAccountId()), r.map, r.stroke, r.token);
            if (!body.empty())
                SendBody(p, body);
        }

        // Players on `map` whose client has this tile (it was sent to them).
        template <class F> void ForTileTrackers(uint32 map, std::pair<int32, int32> tile, F&& f)
        {
            for (auto const& [account, session] : sWorldSessionMgr->GetAllSessions())
            {
                Player* p = session ? session->GetPlayer() : nullptr;
                if (!p || !p->IsInWorld() || p->GetMapId() != map)
                    continue;
                GroundView& v = GroundViewOf(p);
                if (v.map != map)
                    continue;
                if (auto it = v.tiles.find(tile); it != v.tiles.end())
                    f(p, v, it->second);
            }
        }

        void GroundErase(GroundKey key)
        {
            auto it = s_ground.find(key);
            if (it == s_ground.end())
                return;
            GroundRec const r = it->second;
            s_groundTiles[{r.map, r.tile.first, r.tile.second}].erase(r.serial);
            s_ground.erase(it);
            std::string const body = eft::body(0, "GNDX", {std::to_string(key.first), std::to_string(key.second)});
            ForTileTrackers(r.map, r.tile, [&](Player* p, GroundView&, uint64&) { SendBody(p, body); });
        }

        // A stroke into the index with a new serial; the players up to date in its tile get it at once (the painter
        // among them), the others with the next tick.
        void GroundInsert(GroundRec r)
        {
            TileKey const tk{r.map, r.tile.first, r.tile.second};
            auto& tile = s_groundTiles[tk];
            uint64 const prev = tile.empty() ? 0 : tile.rbegin()->first;
            r.serial = ++s_groundSerial;
            tile[r.serial] = {r.cs, r.seq};
            GroundRec const& stored = s_ground[{r.cs, r.seq}] = r;
            ForTileTrackers(r.map, r.tile, [&](Player* p, GroundView& v, uint64& last) {
                if (last < prev)
                    return;   // behind in this tile: the tick sends everything in order
                if (!stored.draft || v.viewer)
                    SendGround(p, stored);
                last = stored.serial;
            });
        }

        // The index made to match a set (after any change: a stroke, undo, redo, withdraw, a review state, a freeze):
        // strokes gone are erased (GNDX to the clients that had them), new ones or ones whose draft state changed
        // (accepted: now everyone's) go in with a new serial (at start nobody tracks a tile yet).
        void GroundSyncSet(ChangeSet const& cs)
        {
            bool const shown = GroundShown(cs), draft = !Live(cs);
            std::set<uint32> want;
            for (Op const& op : cs.ops)
            {
                if (!isGround(op.kind) || op.undone || !shown)
                    continue;
                want.insert(op.seq);
                auto it = s_ground.find({cs.id, op.seq});
                if (it != s_ground.end() && it->second.draft == draft)
                    continue;
                auto stroke = strokeOf(op.kind, op.to);
                if (!stroke)
                {
                    LOG_ERROR("module", "Emberfall.Editor: {} op {} is no stroke; skipped", CsName(cs.id), op.seq);
                    continue;
                }
                if (it != s_ground.end())
                {
                    // accepted (or back to a draft): silently out of the index, then in with a new serial
                    s_groundTiles[{it->second.map, it->second.tile.first, it->second.tile.second}].erase(it->second.serial);
                    s_ground.erase(it);
                }
                GroundRec r;
                r.cs = cs.id, r.seq = op.seq, r.owner = cs.owner, r.draft = draft, r.stroke = *stroke;
                auto m = op.to.find("map");
                r.map = m != op.to.end() ? uint32(m->second.number) : 0;
                r.tile = tileOf(stroke->pts.front().x, stroke->pts.front().y);
                r.token = GroundToken(cs.zone, *stroke);
                GroundInsert(std::move(r));
            }
            std::vector<GroundKey> gone;
            for (auto it = s_ground.lower_bound({cs.id, 0}); it != s_ground.end() && it->first.first == cs.id; ++it)
                if (!want.count(it->first.second))
                    gone.push_back(it->first);
            for (GroundKey const& k : gone)
                GroundErase(k);
        }

        // --------------------------------------------------------------------------------------- places (step 2, §22)

        // Props and kit buildings change nothing on the server either: the ops are kept, and what each shown set makes of
        // each object it touches (a PLACE record: the model where it stands now, and the release instance the clients
        // hide) is streamed by tile like the strokes, drafts only to the editors with the editor open. The release's
        // objects (dist/server/editor_world.sql `ef_editor_prop`, the editable zones' placements.yaml rows) are known by
        // their row's content, which the ops' `from` records: a new release finds them again by it (§7.3's rules).
        struct ModelInfo
        {
            char kind = 'p';
            std::string path;                 // game path with '/', no extension (a prop's variants add -v<k>)
            uint32 variants = 0, built = 0;   // a prop's variants and which of them the clients' data has (bit k)
            bool placeable = false;           // on the whitelist (editor.yaml `models`): an editor may place it
        };
        std::map<std::string, ModelInfo> s_models;
        std::map<std::string, Frame> s_frames;        // zone -> its design frame

        struct RelObj
        {
            char kind = 'p';
            uint8 zone = 0;
            uint16 model = 0;
            uint32 map = 0;
            float u = 0, v = 0, yaw = 0, size = 1;
            float x = 0, y = 0;
        };
        std::vector<RelObj> s_rel;                                         // release id = index + 1
        std::vector<std::string> s_relModels, s_relZones;
        std::map<uint32, std::string> s_relNames;                          // a building's name
        std::map<std::pair<std::string, std::string>, uint32> s_relByName;  // (zone, building name) -> id
        constexpr float kRelCell = 16.0f;
        std::map<std::tuple<uint32, int32, int32>, std::vector<uint32>> s_relCells;

        struct PlaceRec
        {
            uint32 owner = 0;
            bool draft = false;
            PlaceShow show;
            std::pair<int32, int32> tile;
            uint64 serial = 0;
        };
        using PlaceKey = std::tuple<uint32, char, uint32>;                  // (cs, kind, id)
        std::map<PlaceKey, PlaceRec> s_place;
        std::map<TileKey, std::map<uint64, PlaceKey>> s_placeTiles;
        uint64 s_placeSerial = 0;
        std::map<uint32, std::pair<Window, Window>> s_placeRates;           // account -> (minute, day)

        bool IsRel(char kind, uint32 id) { return id >= 1 && id <= s_rel.size() && s_rel[id - 1].kind == kind; }

        std::tuple<uint32, int32, int32> RelCell(uint32 map, float x, float y)
        {
            return {map, int32(std::floor(x / kRelCell)), int32(std::floor(y / kRelCell))};
        }

        PlaceState RelState(uint32 id)
        {
            RelObj const& r = s_rel[id - 1];
            PlaceState s;
            s.present = true;
            s.model = s_relModels[r.model];
            s.u = round2(r.u), s.v = round2(r.v), s.yaw = std::round(double(r.yaw) * 10) / 10, s.size = round2(r.size);
            s.map = r.map;
            if (auto n = s_relNames.find(id); n != s_relNames.end())
                s.name = n->second;
            return s;
        }

        // The release object of `kind` nearest (x, y) within `reach`; with `want`, only one whose row has that content.
        uint32 FindRel(uint32 map, char kind, float x, float y, float reach, PlaceState const* want = nullptr)
        {
            uint32 best = 0;
            float bestD = reach;
            auto [m, cx, cy] = RelCell(map, x, y);
            for (int32 dy = -1; dy <= 1; ++dy)
                for (int32 dx = -1; dx <= 1; ++dx)
                {
                    auto it = s_relCells.find({m, cx + dx, cy + dy});
                    if (it == s_relCells.end())
                        continue;
                    for (uint32 id : it->second)
                    {
                        RelObj const& r = s_rel[id - 1];
                        float const d = std::hypot(r.x - x, r.y - y);
                        if (r.kind != kind || d > bestD)
                            continue;
                        if (want)
                        {
                            PlaceState const s = RelState(id);
                            if (s.model != want->model || std::fabs(s.u - want->u) > 0.006 || std::fabs(s.v - want->v) > 0.006)
                                continue;
                        }
                        best = id, bestD = d;
                    }
                }
            return best;
        }

        // The release row with this content in the zone (a prop by model, u and v; a building by its name).
        uint32 FindRelByContent(std::string const& zone, char kind, PlaceState const& s)
        {
            if (kind == 'b')
            {
                auto it = s_relByName.find({zone, s.name});
                return it != s_relByName.end() && s_relModels[s_rel[it->second - 1].model] == s.model ? it->second : 0;
            }
            auto f = s_frames.find(zone);
            if (f == s_frames.end())
                return 0;
            auto [x, y] = worldOf(f->second, s.u, s.v, Kind::Prop);
            return FindRel(s.map, kind, float(x), float(y), 0.1f, &s);
        }

        // Where a model's state stands: its game path (a prop's variant by its u, v; empty: not drawable here).
        std::string PlacePath(char kind, PlaceState const& s)
        {
            auto m = s_models.find(s.model);
            if (m == s_models.end() || m->second.kind != kind || m->second.path.empty())
                return {};
            if (kind == 'b')
                return m->second.path;
            return variantPath(m->second.path, shownVariant(s.u, s.v, m->second.variants, m->second.built));
        }

        std::vector<Op> PlaceOpsOn(ChangeSet const& cs, char kind, uint32 id)
        {
            std::vector<Op> out;
            for (Op const& op : cs.ops)
                if (char(op.kind) == kind && op.target == id)
                    out.push_back(op);
            return out;
        }

        // What a set makes of one object for the clients (nothing: it does not touch it, or leaves it as the release has it).
        std::optional<PlaceShow> ShowOf(ChangeSet const& cs, char kind, uint32 id)
        {
            PlaceFold const f = foldPlace(PlaceOpsOn(cs, kind, id));
            if (!f.touched)
                return std::nullopt;
            bool const rel = IsRel(kind, id);
            if (rel && f.after.present && samePlace(f.after, RelState(id)))
                return std::nullopt;
            if (!rel && !f.after.present)
                return std::nullopt;   // an add removed again
            auto frame = s_frames.find(cs.zone);
            if (frame == s_frames.end())
                return std::nullopt;
            PlaceShow s;
            s.cs = cs.id, s.id = id, s.kind = Kind(kind);
            s.map = f.after.present ? f.after.map : (rel ? s_rel[id - 1].map : 0);
            if (f.after.present)
            {
                auto [x, y] = worldOf(frame->second, f.after.u, f.after.v, Kind(kind));
                s.path = PlacePath(kind, f.after);
                s.x = float(x), s.y = float(y), s.z = float(f.after.z);
                s.o = float(orientationOf(Kind(kind), f.after.yaw));
                s.size = kind == 'b' ? 1.0f : float(f.after.size);
            }
            if (rel)
                s.hide = std::make_pair(s_rel[id - 1].x, s_rel[id - 1].y);
            if (rel && !f.after.present)
                s.map = s_rel[id - 1].map;
            return s;
        }

        void SendPlace(Player* p, PlaceRec const& r)
        {
            PlaceShow s = r.show;
            s.flags.clear();
            if (r.draft)
                s.flags += 'd';
            if (r.owner == p->GetSession()->GetAccountId())
                s.flags += 'm';
            std::string const body = placeBody(s);
            if (!body.empty())
                SendBody(p, body);
        }

        template <class F> void ForPlaceTrackers(uint32 map, std::pair<int32, int32> tile, F&& f)
        {
            for (auto const& [account, session] : sWorldSessionMgr->GetAllSessions())
            {
                Player* p = session ? session->GetPlayer() : nullptr;
                if (!p || !p->IsInWorld() || p->GetMapId() != map)
                    continue;
                GroundView& v = GroundViewOf(p);
                if (v.map != map)
                    continue;
                if (auto it = v.placeTiles.find(tile); it != v.placeTiles.end())
                    f(p, v, it->second);
            }
        }

        void PlaceErase(PlaceKey const& key, bool tell = true)
        {
            auto it = s_place.find(key);
            if (it == s_place.end())
                return;
            PlaceRec const r = it->second;
            s_placeTiles[{r.show.map, r.tile.first, r.tile.second}].erase(r.serial);
            s_place.erase(it);
            if (!tell)
                return;
            std::string const body = eft::body(0, "PLACEX", {std::to_string(std::get<0>(key)), std::string(1, std::get<1>(key)),
                std::to_string(std::get<2>(key))});
            ForPlaceTrackers(r.show.map, r.tile, [&](Player* p, GroundView&, uint64&) { SendBody(p, body); });
        }

        // A record into the index with a new serial; the players up to date in its tile get it at once.
        void PlaceInsert(PlaceKey const& key, PlaceRec r)
        {
            r.tile = placeTile(r.show);
            TileKey const tk{r.show.map, r.tile.first, r.tile.second};
            auto& tile = s_placeTiles[tk];
            uint64 const prev = tile.empty() ? 0 : tile.rbegin()->first;
            r.serial = ++s_placeSerial;
            tile[r.serial] = key;
            PlaceRec const& stored = s_place[key] = r;
            ForPlaceTrackers(r.show.map, r.tile, [&](Player* p, GroundView& v, uint64& last) {
                if (last < prev)
                    return;   // behind in this tile: the tick sends everything in order
                if (!stored.draft || v.viewer)
                    SendPlace(p, stored);
                last = stored.serial;
            });
        }

        // The records made to match a set (after any change: an op, undo, redo, withdraw, a review state, a freeze).
        void PlaceSyncSet(ChangeSet const& cs)
        {
            bool const shown = GroundShown(cs), draft = !Live(cs);
            std::set<std::pair<char, uint32>> want;
            if (shown)
                for (Op const& op : cs.ops)
                    if (isPlace(op.kind) && op.target && !op.undone)
                        want.insert({char(op.kind), op.target});
            std::set<std::pair<char, uint32>> keep;
            for (auto const& [kind, id] : want)
            {
                PlaceKey const key{cs.id, kind, id};
                std::optional<PlaceShow> const s = ShowOf(cs, kind, id);
                if (!s)
                    continue;
                keep.insert({kind, id});
                auto it = s_place.find(key);
                if (it != s_place.end() && it->second.draft == draft && placeBody(it->second.show) == placeBody(*s))
                    continue;
                // a new state: the old record goes silently (the client replaces a record by its key)
                if (it != s_place.end())
                    PlaceErase(key, false);
                PlaceRec r;
                r.owner = cs.owner, r.draft = draft, r.show = *s;
                PlaceInsert(key, std::move(r));
            }
            std::vector<PlaceKey> gone;
            for (auto it = s_place.lower_bound({cs.id, '\0', 0}); it != s_place.end() && std::get<0>(it->first) == cs.id; ++it)
                if (!keep.count({std::get<1>(it->first), std::get<2>(it->first)}))
                    gone.push_back(it->first);
            for (PlaceKey const& k : gone)
                PlaceErase(k);
        }

        // An object as an op on it in `cs` starts from: the set's own state of it, else the newest live set's that
        // shows it (an accepted or a maintainer's set, not yet released), else the release's. Nothing: no such object,
        // or it is gone.
        std::optional<PlaceState> CurrentPlace(char kind, uint32 id, ChangeSet const& cs)
        {
            PlaceFold const own = foldPlace(PlaceOpsOn(cs, kind, id));
            if (own.touched)
                return own.after.present ? std::optional<PlaceState>(own.after) : std::nullopt;
            for (auto it = s_sets.rbegin(); it != s_sets.rend(); ++it)
            {
                if (it->first == cs.id || !GroundShown(it->second) || !Live(it->second))
                    continue;
                PlaceFold const f = foldPlace(PlaceOpsOn(it->second, kind, id));
                if (f.touched)
                    return f.after.present ? std::optional<PlaceState>(f.after) : std::nullopt;
            }
            if (IsRel(kind, id))
                return RelState(id);
            return std::nullopt;
        }

        // The zone of an object: a release object's own, an added one's set's.
        std::string PlaceZone(char kind, uint32 id)
        {
            if (IsRel(kind, id))
                return s_relZones[s_rel[id - 1].zone];
            for (auto const& [sid, cs] : s_sets)
                for (Op const& op : cs.ops)
                    if (char(op.kind) == kind && op.target == id && op.op == OpKind::Add)
                        return cs.zone;
            return {};
        }

        bool PlaceRateAllows(uint32 account, Level level, uint64 now, bool record)
        {
            PlaceLimits const pl;
            uint32 const k = s_config.local || level == Level::Maintainer ? 100000 : level == Level::Reviewer ? 10 : 1;
            auto it = s_placeRates.try_emplace(account, Window{pl.perMinute, 60000}, Window{pl.perDay, 86400000}).first;
            it->second.first.setLimit(pl.perMinute * k), it->second.second.setLimit(pl.perDay * k);
            if (it->second.first.full(now) || it->second.second.full(now))
                return false;
            if (record)
                it->second.first.add(now), it->second.second.add(now);
            return true;
        }

        // At start (ReplayAll), before the locks are taken: the ops on release objects find their rows again by the
        // content their first op's `from` records (a release since may have renumbered them). An add the release has
        // shipped, and an object the release has neither as it was nor as the set leaves it, are skipped (target 0); a
        // move that meets neither is a conflict (false: the set is marked).
        bool ResolvePlaces(ChangeSet& cs)
        {
            bool ok = true;
            std::map<std::pair<char, uint32>, std::vector<size_t>> by;
            for (size_t i = 0; i < cs.ops.size(); ++i)
                if (isPlace(cs.ops[i].kind))
                    by[{char(cs.ops[i].kind), cs.ops[i].target}].push_back(i);
            for (auto const& [key, idx] : by)
            {
                auto const [kind, stored] = key;
                std::vector<Op> ops;
                for (size_t i : idx)
                    ops.push_back(cs.ops[i]);
                PlaceFold const f = foldPlace(ops);
                uint32 target = stored;
                if (ops.front().op == OpKind::Add)
                {
                    // an add the release has (baked): skipped
                    if (f.after.present && FindRelByContent(cs.zone, kind, f.after))
                        target = 0;
                }
                else
                {
                    // a release object: by its row's content (a turn or a new size keeps model, u and v: the release's own
                    // row then compares equal once it is baked, and nothing is shown)
                    auto first = placeStateOf(ops.front().from);
                    target = first ? FindRelByContent(cs.zone, kind, *first) : 0;
                    if (!target && f.touched && f.after.present && !FindRelByContent(cs.zone, kind, f.after))
                    {
                        ok = false;   // neither its from nor its to: a newer state on main
                        LOG_WARN("module", "Emberfall.Editor: {} {} {}: the release has neither its from nor its to; skipped",
                            CsName(cs.id), kind, stored);
                    }
                    // not found otherwise: moved and baked, or removed (baked or on main): nothing to show
                }
                for (size_t i : idx)
                    cs.ops[i].target = target;
            }
            return ok;
        }

        // The models an editor may place (PMOD), after ROLE.
        void SendModels(Player* player)
        {
            std::vector<std::string> list;
            for (auto const& [key, m] : s_models)
            {
                if (!m.placeable)
                    continue;
                std::string const def = "World/Doodads/" + key;
                uint32 const preview = m.kind == 'p' ? shownVariant(0, 0, m.variants, m.built) : 0;
                list.push_back(std::string(1, m.kind) + "," + key + "," + (m.kind == 'p' && m.path == def ? std::string("-") : m.path) +
                               "," + std::to_string(preview));
            }
            for (std::string const& body : pages("PMOD", {}, list, 200))
                SendBody(player, body);
        }

        // Every second, for every player in the world: a new map forgets every stroke (GNDX;*;*) and place
        // (PLACEX;*;*;*), an editor opening the editor gets its tiles again with the drafts; then the strokes and the
        // places of the 3x3 tiles around it it lacks.
        void GroundTick()
        {
            for (auto const& [account, session] : sWorldSessionMgr->GetAllSessions())
            {
                Player* p = session ? session->GetPlayer() : nullptr;
                if (!p || !p->IsInWorld() || p->IsBeingTeleported())
                    continue;
                GroundView& v = GroundViewOf(p);
                if (!v.ready)
                    continue;
                bool const viewer = Viewing(p);
                if (v.map != p->GetMapId())
                {
                    if (v.map != UINT32_MAX || !s_ground.empty())
                        SendBody(p, eft::body(0, "GNDX", {"*", "*"}));
                    if (v.map != UINT32_MAX || !s_place.empty())
                        SendBody(p, eft::body(0, "PLACEX", {"*", "*", "*"}));
                    v.map = p->GetMapId();
                    v.tiles.clear();
                    v.placeTiles.clear();
                }
                if (viewer != v.viewer)
                {
                    v.viewer = viewer;
                    if (viewer)
                        v.tiles.clear(), v.placeTiles.clear();   // send again, with the drafts this time
                }
                auto const [tx, ty] = tileOf(p->GetPositionX(), p->GetPositionY());
                uint32 budget = kGroundBudget;
                for (int32 dy = -1; dy <= 1 && budget; ++dy)
                    for (int32 dx = -1; dx <= 1 && budget; ++dx)
                    {
                        std::pair<int32, int32> const tile{tx + dx, ty + dy};
                        uint64& last = v.tiles[tile];
                        auto t = s_groundTiles.find({v.map, tile.first, tile.second});
                        if (t == s_groundTiles.end())
                            continue;
                        for (auto it = t->second.upper_bound(last); it != t->second.end() && budget; ++it)
                        {
                            last = it->first;
                            auto r = s_ground.find(it->second);
                            if (r == s_ground.end() || (r->second.draft && !viewer))
                                continue;
                            SendGround(p, r->second);
                            --budget;
                        }
                    }
                for (int32 dy = -1; dy <= 1 && budget; ++dy)
                    for (int32 dx = -1; dx <= 1 && budget; ++dx)
                    {
                        std::pair<int32, int32> const tile{tx + dx, ty + dy};
                        uint64& last = v.placeTiles[tile];
                        auto t = s_placeTiles.find({v.map, tile.first, tile.second});
                        if (t == s_placeTiles.end())
                            continue;
                        for (auto it = t->second.upper_bound(last); it != t->second.end() && budget; ++it)
                        {
                            last = it->first;
                            auto r = s_place.find(it->second);
                            if (r == s_place.end() || (r->second.draft && !viewer))
                                continue;
                            SendPlace(p, r->second);
                            --budget;
                        }
                    }
            }
        }

        // The layers each zone may paint (GLAY), for an editor.
        void SendLayers(Player* player)
        {
            for (auto const& [zone, layers] : s_layers)
            {
                std::vector<std::string> list;
                for (auto const& [name, tileset] : layers)
                    list.push_back(name);
                for (std::string const& body : pages("GLAY", {zone}, list, 8))
                    SendBody(player, body);
            }
        }

        bool GroundRateAllows(uint32 account, Level level, uint64 now, bool record)
        {
            GroundLimits const gl;
            uint32 const k = s_config.local || level == Level::Maintainer ? 100000 : level == Level::Reviewer ? 10 : 1;
            auto it = s_groundRates.try_emplace(account, Window{gl.perMinute, 60000}, Window{gl.perDay, 86400000}).first;
            it->second.first.setLimit(gl.perMinute * k), it->second.second.setLimit(gl.perDay * k);
            if (it->second.first.full(now) || it->second.second.full(now))
                return false;
            if (record)
                it->second.first.add(now), it->second.second.add(now);
            return true;
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
            if (IsCopy(id))
                return Code::Unknown;   // a draft copy is edited through its release spawn
            t.data = EffectiveData(id);
            if (!t.data || s_hidden.count(id))
                return Code::Unknown;   // gone, or a draft removes it (undo that first)
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

        // Step 10's neighbours within `reach` (a square) of a point; `self` and its other creatures do not count.
        std::vector<Neighbour> Neighbours(uint32 mapId, Pos const& at, uint32 self, float reach)
        {
            std::vector<Neighbour> near;
            for (auto const& [id, d] : sObjectMgr->GetAllCreatureData())
            {
                uint32 const base = baseOf(id);
                if (IsCopy(base) || d.mapid != mapId || std::fabs(d.posX - at.x) > reach || std::fabs(d.posY - at.y) > reach)
                    continue;
                Neighbour n;
                n.pos = {d.posX, d.posY, d.posZ, 0};
                n.self = base == self;
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
                if (g.Map == mapId && std::fabs(g.x - at.x) < reach && std::fabs(g.y - at.y) < reach)
                {
                    Neighbour n;
                    n.pos = {g.x, g.y, g.z, 0};
                    n.service = true;
                    near.push_back(n);
                }
            return near;
        }

        // Step 10.
        Density CheckNeighbours(uint32 mapId, Pos const& at, uint32 self, Limits const& l)
        {
            return checkDensity(at, Neighbours(mapId, at, self, 31), l);
        }

        // Steps 4, 8 and 10 for a spawn of `count` creatures at `centre` (count, a move of a spawn of several): the
        // others from slot `fromSlot` on stand in the zone, on the ground near the spawn's height and out of deep water,
        // and every creature of the group keeps the density rules. `detail` names the one that failed.
        Code CheckGroup(Player* player, std::string const& zone, uint32 mapId, Pos const& centre, uint32 count, uint32 fromSlot,
                        uint32 self, Limits const& l, std::string& detail)
        {
            std::vector<Pos> const members = groupPositions(centre, count);
            Map* map = player->GetMap();
            for (uint32 slot = std::max<uint32>(1, fromSlot); slot < members.size(); ++slot)
            {
                Pos const& q = members[slot];
                detail = fmt::format("creature {} of {}", slot + 1, count);
                ZoneInfo const* at = ZoneAt(mapId, q);
                if (!at || at->key != zone)
                    return Code::Zone;
                float const ground = map->GetHeight(player->GetPhaseMask(), q.x, q.y, centre.z + 3.0f, true, 25.0f);
                if (Code const g = checkGround(centre.z, ground); g != Code::Ok)
                    return g;
                if (map->IsUnderWater(player->GetPhaseMask(), q.x, q.y, ground, 2.0f))
                    return Code::Water;
            }
            GroupDensity const g = checkGroupDensity(members, Neighbours(mapId, centre, self, 31 + kCountRing), l);
            detail = fmt::format("creature {} of {}", g.member + 1, count);
            if (g.density.code == Code::Ok)
                detail.clear();
            return g.density.code;
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
            // a raised count places creatures: they count against the day's adds
            uint32 const placed = op.op == OpKind::Set && op.to.count("count") ? CountIn(op.to) - std::min(CountIn(op.to), CountIn(op.from)) : 0;
            if (!rate.allows(op.op, now) || !rate.allowsAdds(placed, now))
                return Code::Rate;
            bool const live = Live(cs);
            if (live && !Apply(op))
                return Code::Internal;
            rate.record(op.op, now);
            rate.recordAdds(placed, now);
            op.seq = cs.ops.empty() ? 1 : cs.ops.back().seq + 1;
            op.createdMs = now;
            if (cs.firstOpMs == 0)
                cs.firstOpMs = now;
            // a new op makes the undone ops after the last active one final (no redo past it)
            cs.ops.push_back(op);
            StoreOp(cs, op);
            Lock(char(op.kind), op.target, cs.id);
            // a contributor's: the editor phase shows it (§19); the world stays the release's for everyone else
            if (!live && !RebuildTarget(char(op.kind), op.target))
                LOG_WARN("module", "Emberfall.Editor: {} op {} on {} meets a newer state in its draft", CsName(cs.id), op.seq, op.target);
            Broadcast(char(op.kind), op.target, player->GetMapId(), at);
            id = std::to_string(op.target);
            return Code::Ok;
        }

        // --------------------------------------------------------------------------------------- requests

        void SendRole(Player* player, Access const& a)
        {
            std::vector<Zone> zones;
            for (auto const& [key, z] : s_zones)
                zones.push_back({key, z.map, z.area});
            Limits const l = LimitsOf(a.level);
            Rate& r = RateOf(player->GetSession()->GetAccountId(), a.level);
            uint64 const now = NowMs();
            size_t open = 0;
            for (auto const& [id, cs] : s_sets)
                open += cs.owner == player->GetSession()->GetAccountId() && state::open(cs.state);
            // one ROLE of at most eft::kMaxBody (roleBody): an oversized one is dropped by the client, the editor never shows
            size_t dropped = 0;
            SendBody(player, roleBody(uint32(a.level), zones, l.opsPerDay - std::min<uint32>(l.opsPerDay, uint32(r.day.count(now))),
                                      uint32(l.openSets > open ? l.openSets - open : 0), l.opsPerSet, s_config.local, &dropped));
            if (dropped)
                LOG_ERROR("module", "Emberfall.Editor: ROLE for account {} leaves {} of {} zone(s) off to fit one message",
                          player->GetSession()->GetAccountId(), dropped, zones.size());
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

        // Editor mode (§6.4, §19): flight, run speed x3, nobody attacks you and you attack nobody, no casting, no
        // looting objects. An explicit toggle, off by default: the panel works without it. Turned off, it takes a
        // contributor back to where they turned it on (`back`: at once; otherwise the next tick, e.g. after a map
        // change), so flight is no travel shortcut; a maintainer stays where they are.
        Code SetMode(Player* player, Session& s, bool on, Level level, bool back = true)
        {
            if (s.mode == on)
                return Code::Ok;
            if (on)
            {
                Map* map = player->GetMap();
                if (Code c = checkModeOn(player->IsInCombat(), player->IsAlive(), player->IsInFlight(), map && map->Instanceable());
                    c != Code::Ok)
                    return c;
                s.mode = true;
                s.hasReturn = returnsOnModeOff(level);
                s.returnMap = player->GetMapId();
                s.returnPos = {player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), player->GetOrientation()};
                s.savedSpeed = player->GetSpeedRate(MOVE_RUN);
                player->SetCanFly(true);
                player->SetSpeed(MOVE_RUN, 3.0f, true);
                player->SetSpeed(MOVE_FLIGHT, 3.0f, true);
                player->SetUnitFlag(UNIT_FLAG_NON_ATTACKABLE);
                player->SetImmuneToNPC(true);
                player->CombatStop(true);
                return Code::Ok;
            }
            s.mode = false;
            player->SetCanFly(false);
            player->SetSpeed(MOVE_RUN, s.savedSpeed > 0 ? s.savedSpeed : 1.0f, true);
            player->SetSpeed(MOVE_FLIGHT, 1.0f, true);
            player->RemoveUnitFlag(UNIT_FLAG_NON_ATTACKABLE);
            player->SetImmuneToNPC(false);
            if (s.hasReturn)
            {
                s.hasReturn = false;
                if (back)
                    player->TeleportTo(s.returnMap, s.returnPos.x, s.returnPos.y, s.returnPos.z, s.returnPos.o);
                else
                    s.returnDue = true;
            }
            return Code::Ok;
        }

        // The editor open or closed (VIEW, §19): open, the player is in the editor phase too and sees the drafts, and
        // no longer sees the release spawns a draft removes.
        void SetView(Player* player, Session& s, bool on)
        {
            if (s.view == on)
                return;
            s.view = on;
            if (on)
                s_viewers.insert(player->GetGUID());
            else
                s_viewers.erase(player->GetGUID());
            uint32 const mask = player->GetPhaseMask();
            uint32 const want = on ? (mask | kDraftPhase) : (mask & ~kDraftPhase);
            if (!player->IsGameMaster() && want != mask)
                player->SetPhaseMask(want ? want : uint32(PHASEMASK_NORMAL), true);
            else
                player->UpdateObjectVisibility();
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
                if (IsCopy(id) || IsExtra(id) || d.mapid != sc.map || std::hypot(d.posX - sc.x, d.posY - sc.y) > sc.radius)
                    continue;   // a draft copy is shown through its release spawn's marker, a spawn's others through its own
                entries.push_back(encodeMarker(MarkerOf(viewer, id, d)));
            }
            s.scanId = (s.scanId % 9999) + 1;
            for (std::string const& body : pages("MARK", {std::to_string(s.scanId)}, entries))
                SendBody(player, body);
            SendOk(player, seq, std::to_string(s.scanId));
        }

        // A new draft set in the zone the player stands in, made active. `live`: a maintainer's (§19).
        ChangeSet* NewSet(Player* player, Session& s, Access const& a, std::string const& title, Code& code)
        {
            uint32 const account = player->GetSession()->GetAccountId();
            ZoneInfo const* zone = ZoneAt(player->GetMapId(), {player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), 0});
            if (!zone)
            {
                code = Code::Zone;
                return nullptr;
            }
            size_t open = 0;
            for (auto const& [id, cs] : s_sets)
                open += cs.owner == account && state::open(cs.state);
            if (open >= LimitsOf(a.level).openSets)
            {
                code = Code::Limit;
                return nullptr;
            }
            ChangeSet cs;
            cs.id = s_nextSet++;
            cs.owner = account;
            cs.ownerName = player->GetName();
            cs.title = title;
            cs.zone = zone->key;
            cs.state = std::string(state::Draft);
            cs.live = s_config.local || appliesLive(a.level);
            CharacterDatabase.DirectExecute("INSERT INTO " + Table("ef_changeset") +
                " (`id`, `owner`, `owner_name`, `title`, `description`, `zone`, `state`, `live`, `created_at`) VALUES ({}, {}, '{}', '{}', '', '{}', 'draft', {}, NOW())",
                cs.id, account, Escaped(cs.ownerName), Escaped(title), Escaped(cs.zone), cs.live ? 1 : 0);
            ChangeSet& stored = s_sets[cs.id] = cs;
            s.active = cs.id;
            SendSet(player, stored, true);
            Audit(player, "NEW", Code::Ok, {{"cs", Value::of(cs.id)}, {"live", Value::of(cs.live ? 1 : 0)}});
            code = Code::Ok;
            return &stored;
        }

        // A change made without an active set: the player's newest editable set of the zone they stand in, or a new one
        // titled "<Zone> edits <date>".
        ChangeSet* AutoSet(Player* player, Session& s, Access const& a, Code& code)
        {
            uint32 const account = player->GetSession()->GetAccountId();
            ZoneInfo const* zone = ZoneAt(player->GetMapId(), {player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), 0});
            if (!zone)
            {
                code = Code::Zone;
                return nullptr;
            }
            for (auto it = s_sets.rbegin(); it != s_sets.rend(); ++it)
                if (it->second.owner == account && it->second.zone == zone->key && state::editable(it->second.state))
                {
                    s.active = it->first;
                    SendSet(player, it->second, true);
                    SendOps(player, it->second);
                    code = Code::Ok;
                    return &it->second;
                }
            char date[16];
            std::time_t const t = std::time(nullptr);
            std::tm tm{};
            gmtime_r(&t, &tm);
            std::strftime(date, sizeof date, "%Y-%m-%d", &tm);
            return NewSet(player, s, a, autoTitle(zone->name.empty() ? zone->key : zone->name, date), code);
        }

        // To review: submitted (the bot opens the merge request), or accepted at once in local mode (make editor-pull).
        void Submit(ChangeSet& cs, std::string const& description)
        {
            cs.description = description;
            cs.state = std::string(s_config.local ? state::Accepted : state::Submitted);
            CharacterDatabase.DirectExecute("UPDATE " + Table("ef_changeset") +
                " SET `description` = '{}', `state` = '{}', `submitted_at` = NOW() WHERE `id` = {}",
                Escaped(description), cs.state, cs.id);
            if (s_config.local)
                Unlock(cs.id);   // accepted: replayed until make editor-pull marks it saved
        }

        // A maintainer's live sets with changes go to review by themselves when the editor closes or they log out:
        // the bot opens the merge request (§19), Nathanael merges.
        void SubmitLive(Player* player)
        {
            uint32 const account = player->GetSession()->GetAccountId();
            Session& s = SessionOf(player);
            for (auto& [id, cs] : s_sets)
            {
                if (cs.owner != account || !cs.live || s_config.local || cs.state != state::Draft)
                    continue;
                bool any = false;
                for (Op const& op : cs.ops)
                    any = any || !op.undone;
                if (!any)
                    continue;
                Submit(cs, "Maintainer edits, live on the realm since they were made.");
                if (s.active == cs.id)
                    s.active = 0;
                SendSet(player, cs);
                Audit(player, "SUBMIT", Code::Ok, {{"cs", Value::of(cs.id)}, {"auto", Value::of(1)}});
            }
        }

        // PAINT, ROAD, SCULPT (§20): a stroke of the zone's ground, validated (zone, layer, presence, ranges, the line's
        // shape, the set's and the realm's caps, the rate) and stored; the server's ground stays the release's.
        template <class Refuse>
        void HandleGround(Player* player, Session& s, Access const& a, eft::Request const& rq, Refuse const& refuse)
        {
            uint32 const seq = rq.seq;
            std::optional<Stroke> stroke = rq.cmd == "PAINT" ? parsePaint(rq.fields) : rq.cmd == "ROAD" ? parseRoad(rq.fields) : parseSculpt(rq.fields);
            if (!stroke)
                return refuse(Code::Decode, "");
            GroundLimits const gl;
            if (Code c = checkStroke(*stroke, gl); c != Code::Ok)
                return refuse(c, "");
            Code code = Code::Ok;
            ChangeSet* cs = ActiveSet(player, s, code);
            if (!cs && code == Code::Set)
                cs = AutoSet(player, s, a, code);
            if (!cs)
                return refuse(code, "");
            ZoneInfo const& zone = s_zones[cs->zone];
            if (player->GetMapId() != zone.map)
                return refuse(Code::Presence, "");
            if (stroke->kind != Kind::Height)
            {
                auto layers = s_layers.find(cs->zone);
                if (layers == s_layers.end() || !layers->second.count(stroke->layer))
                    return refuse(Code::Whitelist, "layer");
            }
            for (Point const& p : stroke->pts)
            {
                if (player->GetExactDist2d(p.x, p.y) > kPresence)
                    return refuse(Code::Presence, "");
                ZoneInfo const* at = ZoneAt(zone.map, {p.x, p.y, player->GetPositionZ(), 0});
                if (!at || at->key != cs->zone)
                    return refuse(Code::Zone, "");
            }
            if (stroke->kind == Kind::Height && stroke->height)
            {
                Point const& p0 = stroke->pts.front();
                float const ground = player->GetMap()->GetHeight(player->GetPhaseMask(), p0.x, p0.y, player->GetPositionZ() + 50.0f, true, 200.0f);
                if (!std::isfinite(ground) || ground < -4000.0f || std::fabs(*stroke->height - ground) > gl.flattenReach)
                    return refuse(Code::Ground, "");
            }
            if (groundCount(cs->ops) + 1 > gl.perSet)
                return refuse(Code::Limit, "strokes per set");
            if (s_ground.size() >= gl.realmCap)
                return refuse(Code::Limit, "strokes on the realm");
            uint32 const account = player->GetSession()->GetAccountId();
            uint64 const now = NowMs();
            if (!GroundRateAllows(account, a.level, now, false))
                return refuse(Code::Rate, "");
            if (groundBody(cs->id, 9999, "dm", zone.map, *stroke, GroundToken(cs->zone, *stroke)).empty())
                return refuse(Code::Decode, "too long");
            GroundRateAllows(account, a.level, now, true);
            Op op;
            op.op = OpKind::Add;
            op.kind = stroke->kind;
            op.target = s_nextGround++;
            op.to = strokeFields(*stroke, zone.map);
            op.seq = cs->ops.empty() ? 1 : cs->ops.back().seq + 1;
            op.createdMs = now;
            if (cs->firstOpMs == 0)
                cs->firstOpMs = now;
            cs->ops.push_back(op);
            StoreOp(*cs, op);
            GroundSyncSet(*cs);
            SendSet(player, *cs);
            SendOps(player, *cs);
            SendOk(player, seq, std::to_string(op.seq));
            Audit(player, rq.cmd, Code::Ok, {{"cs", Value::of(cs->id)}, {"seq", Value::of(op.seq)},
                {"points", Value::of(double(stroke->pts.size()))}});
        }

        // PADD, PMOVE, PDEL (§22): a prop or a kit building placed, moved, turned, scaled or removed, validated (the set's
        // zone, presence, the ground, the whitelist, the size, the lock, the set's and the realm's caps, the rate),
        // stored and streamed (PLACE); the server's world stays the release's.
        template <class Refuse>
        void HandlePlace(Player* player, Session& s, Access const& a, eft::Request const& rq, Refuse const& refuse)
        {
            uint32 const seq = rq.seq;
            bool const add = rq.cmd == "PADD", del = rq.cmd == "PDEL";
            std::optional<PlaceRequest> const req = add ? parsePlaceAdd(rq.fields) : del ? parsePlaceDel(rq.fields) : parsePlaceMove(rq.fields);
            if (!req)
                return refuse(Code::Decode, "");
            if (s_models.empty() || s_frames.empty())
                return refuse(Code::Whitelist, "no place data on the realm (editor_world.sql)");
            Code code = Code::Ok;
            ChangeSet* cs = ActiveSet(player, s, code);
            if (!cs && code == Code::Set)
                cs = AutoSet(player, s, a, code);
            if (!cs)
                return refuse(code, "");
            ZoneInfo const& zone = s_zones[cs->zone];
            auto frame = s_frames.find(cs->zone);
            if (frame == s_frames.end())
                return refuse(Code::Zone, "no frame");
            char const kind = char(req->kind);
            PlaceLimits const pl;
            uint32 const account = player->GetSession()->GetAccountId();
            uint64 const now = NowMs();
            // the object (PMOVE, PDEL): one PLACE named, or the release's at the point the client draws it
            uint32 id = 0;
            std::optional<PlaceState> cur;
            if (!add)
            {
                if (req->ref.at)
                    id = player->GetMapId() == zone.map ? FindRel(zone.map, kind, req->ref.x, req->ref.y, kMatchReach) : 0;
                else if (IsRel(kind, req->ref.id) || !PlaceZone(kind, req->ref.id).empty())
                    id = req->ref.id;
                if (!id)
                    return refuse(Code::Unknown, "");
                if (PlaceZone(kind, id) != cs->zone)
                    return refuse(Code::Zone, "another zone's set");
                auto lock = s_locks.find({kind, id});
                if (lock != s_locks.end() && lock->second != cs->id)
                    return refuse(Code::Lock, "");
                cur = CurrentPlace(kind, id, *cs);
                if (!cur)
                    return refuse(Code::Unknown, "removed");
                auto [ox, oy] = worldOf(frame->second, cur->u, cur->v, req->kind);
                if (player->GetMapId() != zone.map || player->GetExactDist2d(float(ox), float(oy)) > kPresence)
                    return refuse(Code::Presence, "the object");
            }
            PlaceState next;
            if (!del)
            {
                Pos p = req->pos;
                ZoneInfo const* at = ZoneAt(player->GetMapId(), p);
                if (!at || at->key != cs->zone)
                    return refuse(Code::Zone, "");
                if (Code c = CheckPlace(player, zone.map, p, false); c != Code::Ok)
                    return refuse(c, "");
                if (Code c = checkPlaceSize(req->kind, req->size, s_config.local ? Level::Maintainer : a.level, pl); c != Code::Ok)
                    return refuse(c, "size");
                std::string const model = add ? req->model : cur->model;
                auto m = s_models.find(model);
                if (m == s_models.end() || m->second.kind != kind || (add && !m->second.placeable) || m->second.path.empty())
                    return refuse(Code::Whitelist, "");
                auto const [u, v] = designOf(frame->second, p.x, p.y, req->kind);
                next.present = true, next.model = model, next.u = u, next.v = v, next.yaw = req->yaw;
                next.size = kind == 'b' ? 1.0 : double(req->size), next.z = p.z, next.map = zone.map;
                if (cur)
                    next.name = cur->name;
                else if (kind == 'b')
                    next.name = fmt::format("{} ({} #{})", model, CsName(cs->id), placeCount(cs->ops, Kind::Building) + 1);
            }
            if (cur && next.present && samePlace(*cur, next))
            {
                SendOk(player, seq, std::to_string(id));
                return;
            }
            // the caps: objects a set touches (§9.1; reviewers x10, maintainers none), objects the realm shows
            bool const newObject = add || foldPlace(PlaceOpsOn(*cs, kind, id)).touched == false;
            uint32 const scale = s_config.local || a.level == Level::Maintainer ? 100000 : a.level == Level::Reviewer ? 10 : 1;
            if (newObject && placeCount(cs->ops, req->kind) + 1 > (kind == 'b' ? pl.buildingsPerSet : pl.propsPerSet) * scale)
                return refuse(Code::Limit, kind == 'b' ? "buildings per set" : "props per set");
            if (newObject && s_place.size() >= pl.realmCap)
                return refuse(Code::Limit, "objects on the realm");
            if (!PlaceRateAllows(account, a.level, now, false))
                return refuse(Code::Rate, "");
            if (add && s_nextTemp > kTempIdMax)
                return refuse(Code::Internal, "temporary ids exhausted");
            PlaceRateAllows(account, a.level, now, true);
            Op op;
            op.op = add ? OpKind::Add : del ? OpKind::Delete : OpKind::Move;
            op.kind = req->kind;
            op.target = add ? s_nextTemp++ : id;
            op.tempId = add ? op.target : 0;
            if (cur)
                op.from = placeFields(req->kind, *cur);
            if (next.present)
                op.to = placeFields(req->kind, next);
            op.seq = cs->ops.empty() ? 1 : cs->ops.back().seq + 1;
            op.createdMs = now;
            if (cs->firstOpMs == 0)
                cs->firstOpMs = now;
            cs->ops.push_back(op);
            StoreOp(*cs, op);
            Lock(kind, op.target, cs->id);
            PlaceSyncSet(*cs);
            SendSet(player, *cs);
            SendOps(player, *cs);
            SendOk(player, seq, std::to_string(op.target));
            Audit(player, rq.cmd, Code::Ok, {{"cs", Value::of(cs->id)}, {"seq", Value::of(op.seq)}, {"kind", Value::str(std::string(1, kind))},
                {"id", Value::of(op.target)}, {"model", Value::str((next.present ? next.model : cur->model).substr(0, 48))}});
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
            if (rq.cmd == "MODE" || rq.cmd == "VIEW")
            {
                if (f.size() != 1 || (f[0] != "on" && f[0] != "off"))
                    return refuse(Code::Decode);
                bool const on = f[0] == "on";
                if (rq.cmd == "VIEW")
                {
                    SetView(player, s, on);
                    if (!on)
                        SubmitLive(player);   // a maintainer's live changes go to review when the editor closes
                }
                else if (Code c = SetMode(player, s, on, a.level); c != Code::Ok)
                    return refuse(c);
                SendOk(player, seq);
                Audit(player, rq.cmd, Code::Ok, {{"on", Value::of(on ? 1 : 0)}});
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
                Code code = Code::Ok;
                ChangeSet* cs = NewSet(player, s, a, title, code);
                if (!cs)
                    return refuse(code, code == Code::Limit ? "open sets" : "");
                SendOk(player, seq, std::to_string(cs->id));
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
                    RevertLive(cs, !Live(cs));
                    cs.state = std::string(state::Withdrawn);
                    StoreState(cs);
                    Unlock(cs.id);
                    GroundSyncSet(cs);
                    PlaceSyncSet(cs);
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
                    Submit(cs, description);
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
                if (isGround(cs.ops[*index].kind))
                {
                    // a stroke, with the rest of its line (group): nothing on the server, the clients drop or get it
                    auto range = undo ? undoRange(cs.ops) : redoRange(cs.ops);
                    if (!undo && GroundLimits{}.perSet < groundCount(cs.ops) + (range->second - range->first + 1))
                        return refuse(Code::Limit, "strokes per set");
                    for (size_t i = range->first; i <= range->second; ++i)
                    {
                        cs.ops[i].undone = undo;
                        CharacterDatabase.DirectExecute("UPDATE " + Table("ef_op") + " SET `undone` = {} WHERE `changeset_id` = {} AND `seq` = {}",
                            undo ? 1 : 0, cs.id, cs.ops[i].seq);
                    }
                    GroundSyncSet(cs);
                    SendSet(player, cs);
                    SendOps(player, cs);
                    SendOk(player, seq, std::to_string(cs.ops[range->second].target));
                    Audit(player, rq.cmd, Code::Ok, {{"cs", Value::of(cs.id)}, {"seq", Value::of(cs.ops[range->first].seq)},
                        {"strokes", Value::of(double(range->second - range->first + 1))}});
                    return;
                }
                if (isPlace(cs.ops[*index].kind))
                {
                    // a prop or a building (§22): nothing on the server, the clients get the object's record anew
                    Op& op = cs.ops[*index];
                    op.undone = undo;
                    CharacterDatabase.DirectExecute("UPDATE " + Table("ef_op") + " SET `undone` = {} WHERE `changeset_id` = {} AND `seq` = {}",
                        undo ? 1 : 0, cs.id, op.seq);
                    PlaceSyncSet(cs);
                    SendSet(player, cs);
                    SendOps(player, cs);
                    SendOk(player, seq, std::to_string(op.target));
                    Audit(player, rq.cmd, Code::Ok, {{"cs", Value::of(cs.id)}, {"seq", Value::of(op.seq)}});
                    return;
                }
                Op& op = cs.ops[*index];
                CreatureData const* d = EffectiveData(op.target);
                Pos at = d ? Pos{d->posX, d->posY, d->posZ, 0} : posOf(op.from).value_or(posOf(op.to).value_or(Pos{}));
                bool const live = Live(cs);
                if (live && !Apply(undo ? inverse(op) : op))
                    return refuse(Code::Internal);
                op.undone = undo;
                if (!live)
                    RebuildTarget(char(op.kind), op.target);
                CharacterDatabase.DirectExecute("UPDATE " + Table("ef_op") + " SET `undone` = {} WHERE `changeset_id` = {} AND `seq` = {}",
                    undo ? 1 : 0, cs.id, op.seq);
                Broadcast(char(op.kind), op.target, player->GetMapId(), at);
                SendSet(player, cs);
                SendOps(player, cs);
                SendOk(player, seq, std::to_string(op.target));
                Audit(player, rq.cmd, Code::Ok, {{"cs", Value::of(cs.id)}, {"seq", Value::of(op.seq)}});
                return;
            }

            // ---- the ground (step 3, §20): PAINT, ROAD, SCULPT
            if (rq.cmd == "PAINT" || rq.cmd == "ROAD" || rq.cmd == "SCULPT")
                return HandleGround(player, s, a, rq, refuse);
            // ---- props and kit buildings (step 2, §22): PADD, PMOVE, PDEL
            if (rq.cmd == "PADD" || rq.cmd == "PMOVE" || rq.cmd == "PDEL")
                return HandlePlace(player, s, a, rq, refuse);

            // ---- the ops: ADD, MOVE, SET, DEL
            if (rq.cmd != "ADD" && rq.cmd != "MOVE" && rq.cmd != "SET" && rq.cmd != "DEL")
                return refuse(Code::Decode, "unknown command");
            Code code = Code::Ok;
            ChangeSet* cs = ActiveSet(player, s, code);
            if (!cs && code == Code::Set)
                cs = AutoSet(player, s, a, code);   // edit and go on playing: the realm opens a set by itself (§19)
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
                else if (uint32 const n = CountOf(EffectiveId(mv->id)); dist2d(old, p) > 0.5f && n > 1)
                {
                    // a spawn of several moves with its others: each of them where it will stand
                    std::string detail;
                    if (Code c = CheckGroup(player, cs->zone, t.data->mapid, p, n, 1, mv->id, l, detail); c != Code::Ok)
                        return refuse(c, detail);
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
                else if (st->field == SetField::Respawn)
                {
                    if (Code c = checkRespawn(st->value, named, a.level, l); c != Code::Ok)
                        return refuse(c);
                    op.from["respawn"] = Value::of(t.data->spawntimesecs);
                    op.to["respawn"] = Value::of(uint32(st->value));
                }
                else
                {
                    // how many creatures stand at the spawn (count, EfEditorLogic.h): each one keeps the density rules
                    if (Code c = checkCount(st->value, named, a.level); c != Code::Ok)
                        return refuse(c, fmt::format("count 1-{}", kMaxCount));
                    if (t.data->movementType == WAYPOINT_MOTION_TYPE || t.data->poolId || t.data->spawnGroupId)
                        return refuse(Code::ReadOnly, "a patrol or a pooled spawn has one creature");
                    uint32 const now = CountOf(EffectiveId(st->id)), want = uint32(st->value);
                    if (want > now)
                    {
                        std::string detail;
                        Pos const centre{t.data->posX, t.data->posY, t.data->posZ, t.data->orientation};
                        if (Code c = CheckGroup(player, cs->zone, t.data->mapid, centre, want, now, st->id, l, detail); c != Code::Ok)
                            return refuse(c, detail);
                    }
                    op.from["count"] = Value::of(now);
                    op.to["count"] = Value::of(want);
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
                op.from = StateOf(EffectiveId(del->id), *t.data);
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

        bool s_freezeTable = false;   // the auth database has ef_editor_access (the site's freeze and licence record)

        // The role is the account's GM level for this realm (account_access, RealmID this realm's or -1; §19): 1 the
        // contributor, 2 and up the maintainer. ef_editor_access only freezes (the site's `editor:freeze`).
        void LoadAccess(uint32 only)
        {
            std::string const which = only ? fmt::format(" AND `id` = {}", only) : std::string();
            QueryResult r = LoginDatabase.Query("SELECT `id`, MAX(`gmlevel`) FROM `account_access` WHERE `RealmID` IN ({}, -1){} GROUP BY `id`",
                int32(realm.Id.Realm), which);
            std::map<uint32, Access> fresh;
            if (r)
                do
                {
                    Field* f = r->Fetch();
                    Level const level = levelOfSecurity(f[1].Get<uint8>());
                    if (level != Level::None)
                        fresh[f[0].Get<uint32>()] = {level, false};
                } while (r->NextRow());
            if (s_freezeTable)
            {
                std::string const whichFrozen = only ? fmt::format(" AND `account_id` = {}", only) : std::string();
                if (QueryResult z = LoginDatabase.Query("SELECT `account_id` FROM `ef_editor_access` WHERE `frozen` <> 0{}", whichFrozen))
                    do
                    {
                        auto it = fresh.find(z->Fetch()[0].Get<uint32>());
                        if (it != fresh.end())
                            it->second.frozen = true;
                    } while (z->NextRow());
            }
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
                    SetMode(p, s, false, Level::Contributor);
                    SetView(p, s, false);
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
                if (state::open(cs.state))
                {
                    GroundSyncSet(cs);   // a frozen owner's strokes go off the clients (and come back when thawed)
                    PlaceSyncSet(cs);    // and the props and buildings
                }
                if (frozen && state::open(cs.state))
                {
                    bool anyLock = false;
                    for (auto const& [k, set] : s_locks)
                        anyLock = anyLock || set == cs.id;
                    if (anyLock)
                    {
                        RevertLive(cs, !Live(cs));
                        Unlock(cs.id);
                        LOG_INFO("module", "Emberfall.Editor: {} of frozen account {} reverted live", CsName(cs.id), cs.owner);
                    }
                }
            }
        }

        // A live set's ops applied as the replay applies them (§7.3): skipped when in effect (shipped), a conflict when
        // the target meets a newer state; an open set takes its locks. `finals`: temporary ids whose adds shipped.
        bool ReplayLiveSet(ChangeSet& cs, std::map<uint32, uint32> const& finals, uint32& applied, uint32& skipped, uint32& conflicts)
        {
            bool conflict = false;
            for (Op op : cs.ops)
            {
                if (op.undone || isOverlay(op.kind))
                    continue;   // strokes: GroundSyncSet (nothing on the server)
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
                    how = replayState(op, StateOf(op.target, *d));
                if (how == Replay::Apply && Apply(op))
                    ++applied;
                else if (how == Replay::Conflict)
                {
                    ++conflicts;
                    conflict = true;
                    LOG_WARN("module", "Emberfall.Editor: {} op {} ({} {}) meets a newer state; skipped", CsName(cs.id), op.seq,
                        opName(op.op), op.target);
                }
                else
                    ++skipped;
                if (state::open(cs.state) && how != Replay::Conflict)
                    s_locks[{char(op.kind), op.target}] = cs.id;
            }
            return !conflict;
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
                bool const wasDraft = !Live(cs);
                cs.state = st;
                if (wasLive && (st == state::Rejected || st == state::Withdrawn))
                {
                    RevertLive(cs, wasDraft);
                    Unlock(cs.id);
                }
                else if (wasDraft && !state::open(st))
                {
                    // a contributor's draft accepted (or released): its drafts go and it applies for everyone (§19)
                    Unlock(cs.id);
                    RebuildSet(cs);
                    if (st == state::Accepted)
                    {
                        uint32 applied = 0, skipped = 0, conflicts = 0;
                        ReplayLiveSet(cs, {}, applied, skipped, conflicts);
                        LOG_INFO("module", "Emberfall.Editor: {} accepted: live for everyone ({} op(s), {} in effect, {} conflict(s))",
                            CsName(cs.id), applied, skipped, conflicts);
                    }
                    BroadcastSet(cs);
                }
                if (st == state::Accepted || st == state::Released)
                    Unlock(cs.id);
                GroundSyncSet(cs);   // rejected, withdrawn, released: off the clients; accepted: everyone's now
                PlaceSyncSet(cs);
                if (Player* p = OnlineOwner(cs))
                    SendSet(p, cs);
                LOG_INFO("module", "Emberfall.Editor: {} is {} (site)", CsName(cs.id), st);
            } while (r->NextRow());
        }

        // The props and buildings (§22; editor_world.sql from step 2 on): the models (the whitelist and every model the
        // editable zones stand), the zones' frames and the release's objects. Without the tables nobody places anything.
        void LoadPlaces()
        {
            s_models.clear(), s_frames.clear(), s_rel.clear(), s_relModels.clear(), s_relZones.clear(), s_relNames.clear();
            s_relByName.clear(), s_relCells.clear();
            if (!TableExists("ef_editor_model", true) || !TableExists("ef_editor_frame", true) || !TableExists("ef_editor_prop", true))
            {
                LOG_WARN("module", "Emberfall.Editor: world tables ef_editor_model/frame/prop are missing (dist/server/editor_world.sql): "
                    "no props or buildings");
                return;
            }
            if (QueryResult r = WorldDatabase.Query("SELECT `model`, `kind`, `path`, `variants`, `built`, `placeable` FROM `ef_editor_model`"))
                do
                {
                    Field* f = r->Fetch();
                    std::string const key = f[0].Get<std::string>(), kind = f[1].Get<std::string>(), path = f[2].Get<std::string>();
                    if (!modelKey(key) || kind.size() != 1 || !isPlace(kind[0]) || path.size() > 96)
                        continue;
                    ModelInfo m;
                    m.kind = kind[0], m.path = path, m.variants = f[3].Get<uint32>(), m.built = f[4].Get<uint32>();
                    m.placeable = f[5].Get<uint8>() != 0;
                    s_models[key] = m;
                } while (r->NextRow());
            if (QueryResult r = WorldDatabase.Query("SELECT `zone`, `origin_u`, `origin_v`, `tile` FROM `ef_editor_frame`"))
                do
                {
                    Field* f = r->Fetch();
                    s_frames[f[0].Get<std::string>()] = Frame{f[1].Get<double>(), f[2].Get<double>(), f[3].Get<double>()};
                } while (r->NextRow());
            std::map<std::string, uint16> models;
            std::map<std::string, uint8> zones;
            if (QueryResult r = WorldDatabase.Query("SELECT `id`, `zone`, `kind`, `model`, `u`, `v`, `yaw`, `size`, `name` FROM `ef_editor_prop` ORDER BY `id`"))
                do
                {
                    Field* f = r->Fetch();
                    uint32 const id = f[0].Get<uint32>();
                    std::string const zone = f[1].Get<std::string>(), kind = f[2].Get<std::string>(), model = f[3].Get<std::string>();
                    auto frame = s_frames.find(zone);
                    auto zi = s_zones.find(zone);
                    if (id != s_rel.size() + 1 || kind.size() != 1 || !isPlace(kind[0]) || frame == s_frames.end() || zi == s_zones.end())
                    {
                        LOG_ERROR("module", "Emberfall.Editor: ef_editor_prop row {} is out of order or names no zone: the props stop there", id);
                        break;
                    }
                    if (models.size() >= 65535 || zones.size() >= 255)
                        break;
                    auto [mi, newModel] = models.try_emplace(model, uint16(s_relModels.size()));
                    if (newModel)
                        s_relModels.push_back(model);
                    auto [zix, newZone] = zones.try_emplace(zone, uint8(s_relZones.size()));
                    if (newZone)
                        s_relZones.push_back(zone);
                    RelObj o;
                    o.kind = kind[0], o.zone = zix->second, o.model = mi->second, o.map = zi->second.map;
                    o.u = f[4].Get<float>(), o.v = f[5].Get<float>(), o.yaw = f[6].Get<float>(), o.size = f[7].Get<float>();
                    auto [x, y] = worldOf(frame->second, o.u, o.v, Kind(o.kind));
                    o.x = float(x), o.y = float(y);
                    s_rel.push_back(o);
                    s_relCells[RelCell(o.map, o.x, o.y)].push_back(id);
                    if (o.kind == 'b')
                    {
                        std::string const name = f[8].Get<std::string>();
                        s_relNames[id] = name;
                        s_relByName[{zone, name}] = id;
                    }
                } while (r->NextRow());
            size_t placeable = 0;
            for (auto const& [k, m] : s_models)
                placeable += m.placeable;
            LOG_INFO("server.loading", ">> Emberfall.Editor: {} release prop(s) and building(s) in {} zone(s), {} model(s) ({} placeable)",
                s_rel.size(), s_frames.size(), s_models.size(), placeable);
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
            // the paintable ground layers (§20; editor_world.sql from 2026-10-31 on): without the table nobody paints
            s_layers.clear();
            if (TableExists("ef_editor_layer", true))
            {
                if (QueryResult r = WorldDatabase.Query("SELECT `zone`, `layer`, `tileset` FROM `ef_editor_layer`"))
                    do
                    {
                        Field* f = r->Fetch();
                        std::string const layer = f[1].Get<std::string>();
                        if (layerName(layer))
                            s_layers[f[0].Get<std::string>()][layer] = f[2].Get<std::string>();
                    } while (r->NextRow());
            }
            else
                LOG_WARN("module", "Emberfall.Editor: world table ef_editor_layer is missing (dist/server/editor_world.sql): no ground painting");
            LoadPlaces();
        }

        // §7.1 and §7.3: every accepted set not yet released (by id), then every open set of an account that is not
        // frozen, in the order of its first op; each op skipped when already in effect, a conflict marks its set.
        void ReplayAll()
        {
            s_sets.clear(), s_locks.clear(), s_copies.clear(), s_copyOf.clear(), s_hidden.clear();
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
            if (QueryResult r = CharacterDatabase.Query("SELECT `id`, `owner`, IFNULL(`owner_name`, ''), `title`, IFNULL(`description`, ''), `zone`, `state`, `live` FROM " +
                    Table("ef_changeset") + " ORDER BY `id`"))
                do
                {
                    Field* f = r->Fetch();
                    ChangeSet cs;
                    cs.id = f[0].Get<uint32>(), cs.owner = f[1].Get<uint32>(), cs.ownerName = f[2].Get<std::string>();
                    cs.title = f[3].Get<std::string>(), cs.description = f[4].Get<std::string>(), cs.zone = f[5].Get<std::string>();
                    cs.state = f[6].Get<std::string>();
                    cs.live = f[7].Get<uint8>() != 0;
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

            // Live sets (accepted ones, a maintainer's open ones) change the world; then a contributor's open sets take
            // their locks and their drafts go into the editor phase (§19), the same in-effect rules for both.
            uint32 applied = 0, skipped = 0, conflicts = 0, drafted = 0;
            std::vector<ChangeSet*> drafts;
            // the props and buildings (§22) find their release rows again (and take their locks: ResolvePlaces)
            std::set<uint32> placeConflicts;
            for (auto& [id, cs] : s_sets)
                if (!ResolvePlaces(cs))
                    placeConflicts.insert(id);
            for (ChangeSet* cs : order)
                for (Op const& op : cs->ops)
                    if (state::open(cs->state) && isPlace(op.kind) && op.target && !op.undone)
                        s_locks[{char(op.kind), op.target}] = cs->id;
            for (ChangeSet* cs : order)
            {
                bool ok = !placeConflicts.count(cs->id);
                if (Live(*cs))
                    ok = ReplayLiveSet(*cs, finals, applied, skipped, conflicts);
                else
                {
                    for (Op const& op : cs->ops)
                        if (!op.undone && !isOverlay(op.kind))
                            s_locks[{char(op.kind), op.target}] = cs->id;
                    drafts.push_back(cs);
                }
                if (!ok && state::open(cs->state) && cs->state != state::Conflict)
                {
                    cs->state = std::string(state::Conflict);
                    StoreState(*cs);
                }
            }
            for (ChangeSet* cs : drafts)
            {
                std::set<std::pair<char, uint32>> targets;
                for (Op const& op : cs->ops)
                    if (!op.undone && !isOverlay(op.kind))
                        targets.insert({char(op.kind), op.target});
                bool ok = true;
                for (auto const& [k, id] : targets)
                {
                    ok = RebuildTarget(k, id) && ok;
                    ++drafted;
                }
                if (!ok)
                {
                    ++conflicts;
                    if (cs->state != state::Conflict)
                    {
                        cs->state = std::string(state::Conflict);
                        StoreState(*cs);
                    }
                }
            }
            CharacterDatabase.DirectExecute("DELETE FROM " + Table("ef_lock"));
            for (auto const& [key, set] : s_locks)
                CharacterDatabase.DirectExecute("INSERT INTO " + Table("ef_lock") + " (`kind`, `target_id`, `changeset_id`) VALUES ('{}', {}, {})",
                    std::string(1, key.first), key.second, set);

            // the day's rate windows survive a restart (ops' server timestamps)
            s_rates.clear();
            s_groundRates.clear();
            s_placeRates.clear();
            uint64 const now = NowMs();
            for (auto const& [id, cs] : s_sets)
                for (Op const& op : cs.ops)
                    if (op.createdMs + 86400000 > now && isGround(op.kind))
                    {
                        GroundLimits const gl;
                        auto it = s_groundRates.try_emplace(cs.owner, Window{gl.perMinute, 60000}, Window{gl.perDay, 86400000}).first;
                        it->second.second.add(op.createdMs);
                    }
                    else if (op.createdMs + 86400000 > now && isPlace(op.kind))
                    {
                        PlaceLimits const pl;
                        auto it = s_placeRates.try_emplace(cs.owner, Window{pl.perMinute, 60000}, Window{pl.perDay, 86400000}).first;
                        it->second.second.add(op.createdMs);
                    }
                    else if (op.createdMs + 86400000 > now)
                    {
                        Rate& r = s_rates[cs.owner];
                        r.day.add(op.createdMs);
                        if (op.op == OpKind::Add)
                            r.adds.add(op.createdMs);
                        if (op.op == OpKind::Delete)
                            r.deletes.add(op.createdMs);
                        if (op.op == OpKind::Set && op.to.count("count"))
                            r.recordAdds(CountIn(op.to) - std::min(CountIn(op.to), CountIn(op.from)), op.createdMs);
                    }
            // the ground strokes (§20): into the index, streamed to the players as they come near
            s_ground.clear(), s_groundTiles.clear();
            uint32 maxGround = 0;
            for (auto const& [id, cs] : s_sets)
            {
                for (Op const& op : cs.ops)
                    if (isGround(op.kind))
                        maxGround = std::max(maxGround, op.target);
                GroundSyncSet(cs);
            }
            s_nextGround = maxGround + 1;
            // the props and buildings (§22): their records into the index, streamed like the strokes
            s_place.clear(), s_placeTiles.clear();
            for (auto const& [id, cs] : s_sets)
                PlaceSyncSet(cs);
            LOG_INFO("server.loading", ">> Emberfall.Editor: {} zone(s), {} editable spawn(s), {} set(s) replayed: {} op(s) applied, {} already in effect, {} conflict(s); {} object(s) of {} draft(s) in the editor phase; {} ground stroke(s) shown; {} prop(s) and building(s) shown",
                s_zones.size(), s_spawns.size(), order.size(), applied, skipped, conflicts, drafted, drafts.size(), s_ground.size(), s_place.size());
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


    // The core's hook (EmberfallVisibility.h): a release spawn a contributor's draft removes is hidden from the editors
    // whose editor is open; everyone else still sees it until the set is accepted (§19).
    static bool HideFromViewer(Creature const* creature, Player const* viewer)
    {
        return !s_hidden.empty() && s_hidden.count(baseOf(creature->GetSpawnId())) && Viewing(viewer);
    }

    void OnStartup()
    {
        s_ready = false;
        Emberfall::Visibility::SetCreatureFilter(nullptr);
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
        if (!CharacterDatabase.Query("SHOW COLUMNS FROM `{}`.`ef_changeset` LIKE 'live'", s_config.db))
        {
            LOG_ERROR("module", "Emberfall.Editor: {}.ef_changeset has no `live` column (apply data/sql/db-editor/editor.sql again): the editor stays off",
                s_config.db);
            return;
        }
        for (char const* t : {"ef_editor_zone", "ef_editor_spawn", "ef_editor_entry"})
            if (!TableExists(t, true))
            {
                LOG_ERROR("module", "Emberfall.Editor: world table {} is missing (dist/server/editor_world.sql): the editor stays off", t);
                return;
            }
        s_freezeTable = LoginDatabase.Query("SHOW TABLES LIKE 'ef_editor_access'") != nullptr;
        if (!s_config.local && !s_freezeTable)
            LOG_WARN("module", "Emberfall.Editor: auth table ef_editor_access is missing: nobody can be frozen (data/sql/db-auth)");
        LoadRelease();
        if (!s_config.local)
            LoadAccess(0);
        ReplayAll();
        Emberfall::Visibility::SetCreatureFilter(&HideFromViewer);
        s_ready = true;
        LOG_INFO("server.loading", ">> Emberfall.Editor: on ({} mode, database {}; role: the GM level of realm {}, 1 contributor, 2+ maintainer)",
            s_config.local ? "local" : "realm", s_config.db, realm.Id.Realm);
    }

    void OnUpdate(uint32 diff)
    {
        if (!s_ready)
            return;
        // every second: the viewers keep the editor phase (a phase aura, a memory, resets the mask), a return due
        // after a map change happens
        s_tickTimer += diff;
        if (s_tickTimer >= 1000)
        {
            s_tickTimer = 0;
            for (ObjectGuid const& g : std::vector<ObjectGuid>(s_editors.begin(), s_editors.end()))
            {
                Player* p = ObjectAccessor::FindPlayer(g);
                if (!p || !p->IsInWorld() || p->IsBeingTeleported())
                    continue;
                Session& s = SessionOf(p);
                if (s.view && !p->IsGameMaster() && !(p->GetPhaseMask() & kDraftPhase))
                    p->SetPhaseMask(p->GetPhaseMask() | kDraftPhase, true);
                if (s.returnDue)
                {
                    s.returnDue = false;
                    p->TeleportTo(s.returnMap, s.returnPos.x, s.returnPos.y, s.returnPos.z, s.returnPos.o);
                }
            }
            GroundTick();
        }
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

    // Before the character is saved: editor mode ends where it began (a contributor's flight is no travel shortcut),
    // the editor phase is left, and a maintainer's live changes go to review.
    void OnBeforeLogout(Player* player)
    {
        if (!s_ready)
            return;
        Session& s = SessionOf(player);
        if (s.mode)
        {
            bool const back = s.hasReturn && s.returnMap == player->GetMapId();
            Pos const to = s.returnPos;
            s.hasReturn = false;
            SetMode(player, s, false, Level::Maintainer);
            if (back)
                player->Relocate(to.x, to.y, to.z, to.o);
        }
        SetView(player, s, false);
        if (s.role)
            SubmitLive(player);
    }

    void OnLogout(Player* player)
    {
        if (!s_ready)
            return;
        s_viewers.erase(player->GetGUID());
        s_editors.erase(player->GetGUID());
    }

    // Another map (a portal, a hearthstone, a dungeon): editor mode ends, and a contributor goes back to where it began.
    void OnMapChanged(Player* player)
    {
        if (!s_ready)
            return;
        Session& s = SessionOf(player);
        if (s.mode)
            SetMode(player, s, false, AccessOf(player->GetSession()->GetAccountId()).level, false);
    }

    bool InEditorMode(Player const* player)
    {
        if (!s_ready || !player)
            return false;
        auto const* s = const_cast<Player*>(player)->CustomData.Get<Session>(kSessionKey);
        return s && s->mode;
    }

    // A draft copy, or a spawn a draft adds (§19): seen only in the editor phase, and it does nothing there: nobody
    // attacks it, it attacks nobody, it has no menu, no quest, no loot, no AI.
    // The other creatures of a spawn of several (count) share the spawn's height (world.sql has nothing better: the
    // compile has no ground for every zone); the first time a unit loads, it and its data go onto the ground of its own
    // spot. On every realm, the editor on or not.
    static void OntoGround(Creature* creature, uint32 id)
    {
        CreatureData* data = const_cast<CreatureData*>(sObjectMgr->GetCreatureData(id));
        Map* map = creature->GetMap();
        if (!data || !map || creature->IsFlying() || creature->CanFly())
            return;
        float const x = data->posX, y = data->posY, z = data->posZ;
        float const g = map->GetHeight(creature->GetPhaseMask(), x, y, z + 3.0f, true, 10.0f);
        if (!std::isfinite(g) || g <= INVALID_HEIGHT + 1.0f || std::fabs(g - z) < 0.05f || std::fabs(g - z) > 2 * kGroundTolerance)
            return;
        if (map->IsInWater(creature->GetPhaseMask(), x, y, g, creature->GetCollisionHeight()))
            return;
        data->posZ = g;
        creature->SetHomePosition(x, y, g, data->orientation);
        if (creature->IsAlive())
            creature->Relocate(x, y, g, creature->GetOrientation());
    }

    void OnCreatureAdd(Creature* creature)
    {
        uint32 const id = creature->GetSpawnId();
        if (IsExtra(id))
            OntoGround(creature, id);
        if (!s_ready)
            return;
        uint32 const base = baseOf(id);
        if (!IsCopy(base) && !(IsTemp(base) && (creature->GetPhaseMask() & kDraftPhase)))
            return;
        creature->SetFaction(FACTION_FRIENDLY);
        creature->SetUnitFlag(UNIT_FLAG_NON_ATTACKABLE);
        creature->SetImmuneToAll(true);
        creature->ReplaceAllNpcFlags(UNIT_NPC_FLAG_NONE);
        creature->AIM_Initialize(new NullCreatureAI(creature));
        creature->SetReactState(REACT_PASSIVE);
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
        if (parsed.ok() && parsed.request.cmd == "HELLO")
        {
            // any client, editor or not: from now on it gets the ground strokes (§20), all of them again
            GroundView& v = GroundViewOf(player);
            v.ready = true;
            v.map = UINT32_MAX;
            v.tiles.clear();
            v.placeTiles.clear();
            if (s_ground.empty() && s_place.empty())
                v.map = player->GetMapId();   // nothing to forget, nothing to send yet
        }
        if (a.level == Level::None || a.frozen)
        {
            // HELLO from a player without the role: silence (the UI stays hidden), and not audited: on the normal realm
            // every client says HELLO once. Anything else is a forged or patched client.
            if (parsed.ok() && parsed.request.cmd == "HELLO" && !a.frozen)
                return;
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
            SendLayers(player);
            SendModels(player);
            for (auto const& [id, cs] : s_sets)
                if (cs.owner == account && (state::open(cs.state) || cs.state == state::Accepted))
                    SendSet(player, cs, id == s.active);
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
        size_t open = 0, drafts = 0;
        for (auto const& [id, cs] : s_sets)
        {
            open += state::open(cs.state);
            drafts += !Live(cs);
        }
        lines.push_back(fmt::format("zones {}, editable spawns {}, accounts with access {}, sets live {} ({} open, {} drafts), locks {}, editors online {} ({} with the editor open)",
            s_zones.size(), s_spawns.size(), s_access.size(), s_sets.size(), open, drafts, s_locks.size(), s_editors.size(), s_viewers.size()));
        lines.push_back(fmt::format("editor phase: {} draft copies, {} spawns hidden from viewers", s_copies.size(), s_hidden.size()));
        size_t draftStrokes = 0, layers = 0;
        for (auto const& [key, r] : s_ground)
            draftStrokes += r.draft;
        for (auto const& [zone, l] : s_layers)
            layers += l.size();
        lines.push_back(fmt::format("ground: {} stroke(s) shown ({} drafts) in {} tile(s); {} paintable layer(s) in {} zone(s)",
            s_ground.size(), draftStrokes, s_groundTiles.size(), layers, s_layers.size()));
        size_t draftPlaces = 0, placeable = 0;
        for (auto const& [key, r] : s_place)
            draftPlaces += r.draft;
        for (auto const& [key, m] : s_models)
            placeable += m.placeable;
        lines.push_back(fmt::format("places: {} prop(s) and building(s) shown ({} drafts); {} release object(s), {} model(s), {} placeable",
            s_place.size(), draftPlaces, s_rel.size(), s_models.size(), placeable));
        for (auto const& [id, cs] : s_sets)
            lines.push_back(fmt::format("  {} {}{} by {} ({}): {} op(s), {}", CsName(cs.id), cs.state, Live(cs) ? "" : " (draft)", cs.ownerName, cs.owner,
                cs.ops.size(), cs.title));
    }
}
