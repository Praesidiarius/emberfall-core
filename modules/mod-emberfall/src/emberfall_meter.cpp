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

// The damage meter (Emberfall docs/design/damage-meter.md): the server counts what every group (or solo player) does,
// from the combat log at its source (the core's Emberfall::CombatLog sink, PATCHES.md #12), so every member sees the
// same numbers whatever the range. src/meter/EfMeterLogic.h holds the tallies, the fights and the EFM bodies; this file
// resolves units to meters and participants, keeps each client's state on its player, answers the client and pushes
// what changed once a second.
//
// Meters: one per group (its GUID) or per player outside a group; a player joining or leaving a group switches meters
// (the client gets ON with the other meter's epoch and starts over). A meter nobody touched for 30 minutes goes.
// The sink runs on map threads; one mutex guards every meter (its work is a few map lookups per event).
//
// Config: Emberfall.Meter.Enable (1; AC_EMBERFALL_METER_ENABLE). Off: no sink, HELLO answers OFF.
// Commands: `.efmeter status` (the meter, your client's bandwidth, the current or last fight and overall per
// participant), `.efmeter reset` (solo, or the group's leader).

#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "Config.h"
#include "Creature.h"
#include "DataMap.h"
#include "EmberfallCombatLog.h"
#include "GameTime.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "UnitScript.h"
#include "WorldPacket.h"
#include "WorldScript.h"
#include "WorldSession.h"

#include "companions/EfCompanions.h"
#include "meter/EfMeterLogic.h"

#include <memory>
#include <mutex>
#include <unordered_map>

using namespace Acore::ChatCommands;
namespace M = Emberfall::Meter;
namespace CL = Emberfall::CombatLog;

namespace
{
    constexpr char const* kDataKey = "emberfall_meter";
    constexpr uint32 kCombatPollMs = 250;
    constexpr uint64 kDropAfterMs = 30 * 60 * 1000;
    constexpr uint64 kGcEveryMs = 60 * 1000;

    bool s_enable = true;

    std::mutex s_lock;
    std::unordered_map<uint64, std::unique_ptr<M::Meter>> s_meters;
    uint32 s_nextEpoch = 1;
    uint64 s_lastGcMs = 0;

    uint64 NowMs() { return uint64(GameTime::GetGameTimeMS().count()); }

    // The per-player state rides on the player (Object::CustomData): its map's thread, under s_lock.
    struct PlayerMeter : public DataMap::Base
    {
        M::Client client;
        uint64 meterKey = 0;
        uint8 pid = 0;
        uint32 pollMs = 0;
    };

    uint64 MeterKey(Player* p)
    {
        if (Group* g = p->GetGroup())
            return g->GetGUID().GetRawValue();
        return p->GetGUID().GetRawValue();
    }

    M::Meter& MeterFor(uint64 key)
    {
        auto& slot = s_meters[key];
        if (!slot)
        {
            slot = std::make_unique<M::Meter>();
            slot->epoch = s_nextEpoch++;
        }
        return *slot;
    }

    M::Meter* FindMeter(uint64 key)
    {
        auto it = s_meters.find(key);
        return it == s_meters.end() ? nullptr : it->second.get();
    }

    // Who did it: the meter, the participant's row, the pet that did it (0: itself).
    struct Who
    {
        M::Meter* m = nullptr;
        uint8 pid = 0;
        uint8 src = 0;
    };

    Who ResolvePlayer(Player* p)
    {
        M::Meter& m = MeterFor(MeterKey(p));
        return { &m, m.PidFor(p->GetGUID().GetRawValue(), M::Kind::Player, p->getClass(), 0, p->GetName()), 0 };
    }

    // A companion's class for its bar's colour: by its archetype's role (its creature's unit class is a stat row, and
    // our archetypes all use the warrior's): tank warrior, healer priest, melee rogue, ranged mage.
    uint8 CompanionClass(Creature* c)
    {
        Emberfall::Companions::CompanionAIBase* ai = Emberfall::Companions::AIOf(c);
        Emberfall::Companions::Archetype const* a = ai ? Emberfall::Companions::FindArchetype(ai->ArchetypeId()) : nullptr;
        switch (a ? a->role : Emberfall::Companions::Role::None)
        {
            case Emberfall::Companions::Role::Tank: return CLASS_WARRIOR;
            case Emberfall::Companions::Role::Healer: return CLASS_PRIEST;
            case Emberfall::Companions::Role::Melee: return CLASS_ROGUE;
            case Emberfall::Companions::Role::Ranged: return CLASS_MAGE;
            default: return c->getClass();
        }
    }

    // An Emberfall companion: a row of its owner's meter, kept by its place in the roster (a resummon is the same row).
    Who ResolveCompanion(Creature* c, Player* owner)
    {
        uint32 index = 0;
        if (Emberfall::Companions::Roster* r = Emberfall::Companions::GetRoster(owner, false))
            for (uint32 i = 0; i < r->members.size(); ++i)
                if (r->members[i].guid == c->GetGUID())
                    index = i + 1;
        M::Meter& m = MeterFor(MeterKey(owner));
        uint64 const key = (uint64(owner->GetGUID().GetCounter()) << 8) | index;
        return { &m, m.PidFor(key, M::Kind::Companion, CompanionClass(c), 0, c->GetName()), 0 };
    }

    Player* CompanionOwner(Creature* c)
    {
        if (!c || !c->GetOwnerGUID().IsPlayer() || !Emberfall::Companions::AIOf(c))
            return nullptr;
        return Emberfall::Companions::OwnerOf(c);
    }

    Who Resolve(Unit* u)
    {
        if (!u)
            return {};
        if (Player* p = u->ToPlayer())
            return ResolvePlayer(p);
        Creature* c = u->ToCreature();
        if (!c)
            return {};
        if (Player* owner = CompanionOwner(c))
            return ResolveCompanion(c, owner);
        // Pets, guardians, totems, charmed creatures: up the owner chain to a player or a companion.
        Unit* owner = c->GetCharmerOrOwner();
        for (int depth = 0; owner && depth < 3; ++depth)
        {
            Who w;
            if (Player* p = owner->ToPlayer())
                w = ResolvePlayer(p);
            else if (Player* cOwner = CompanionOwner(owner->ToCreature()))
                w = ResolveCompanion(owner->ToCreature(), cOwner);
            if (w.m && w.pid)
            {
                w.src = w.m->PidFor((uint64(w.pid) << 32) | c->GetEntry(), M::Kind::Pet, 0, w.pid, c->GetName());
                return w;
            }
            owner = owner->GetCharmerOrOwner();
        }
        return {};
    }

    // A boss names a fight: the core's world and dungeon bosses, and (our dungeons flag neither) an elite with at least
    // five times the health of its level in a dungeon.
    bool IsBoss(Unit const* u)
    {
        Creature const* c = u ? u->ToCreature() : nullptr;
        if (!c)
            return false;
        if (c->isWorldBoss() || c->IsDungeonBoss())
            return true;
        CreatureTemplate const* t = c->GetCreatureTemplate();
        Map const* map = c->GetMap();
        return t && map && map->IsDungeon() && t->rank >= CREATURE_ELITE_ELITE && t->ModHealth >= 5.0f;
    }

    uint8 HealthPctAfterDamage(Unit const* target, uint32 healthBefore, uint32 damage)
    {
        return M::Pct(healthBefore > damage ? healthBefore - damage : 0, target->GetMaxHealth());
    }

    void OnDamage(CL::Event const& e, uint64 now, bool avoided, bool environmental)
    {
        Who const atk = environmental ? Who{} : Resolve(e.source);
        Who const vic = Resolve(e.target);
        bool const friendly = atk.m && vic.m && atk.m == vic.m;
        bool const self = e.source == e.target;
        if (avoided && friendly)
            return; // a heal or buff on a group member that did not land: not combat
        M::Hit h;
        h.spell = environmental ? M::kEnvSpellBase + e.envType : e.spellId;
        h.amount = e.amount;
        h.overkill = e.overkill;
        h.absorbed = e.absorbed;
        h.crit = e.crit;
        h.avoided = avoided;
        if (atk.m && atk.pid && !friendly && !self)
        {
            M::Enemy const target{ e.target->GetName(), IsBoss(e.target) };
            atk.m->DamageDone(now, atk.pid, atk.src, h, target);
        }
        if (vic.m && vic.pid && !vic.src)
        {
            std::string const& sourceName = e.source && !environmental ? e.source->GetName() : std::string();
            M::Enemy const enemy{ sourceName, IsBoss(e.source) };
            bool const hostile = !environmental && e.source && !friendly && !self;
            vic.m->DamageTaken(now, vic.pid, h, sourceName, hostile ? &enemy : nullptr,
                               HealthPctAfterDamage(e.target, e.targetHealthBefore, avoided ? 0 : e.amount), environmental);
        }
    }

    void OnHeal(CL::Event const& e, uint64 now)
    {
        Who const healer = Resolve(e.source);
        Who const target = Resolve(e.target);
        M::Heal h;
        h.spell = e.spellId;
        h.amount = e.amount;
        h.overheal = std::min(e.overkill, e.amount);
        h.absorbed = e.absorbed;
        h.crit = e.crit;
        if (healer.m && healer.pid)
            healer.m->HealDone(now, healer.pid, healer.src, h);
        if (target.m && target.pid && !target.src)
        {
            uint32 const after = e.healthAfter ? e.targetHealthBefore : e.targetHealthBefore + h.Effective();
            target.m->HealReceived(now, target.pid, h, e.source ? e.source->GetName() : std::string(), M::Pct(after, e.target->GetMaxHealth()));
        }
    }

    void OnAbsorb(CL::Event const& e, uint64 now)
    {
        Who const caster = Resolve(e.caster);
        Who const target = Resolve(e.target);
        Who const attacker = Resolve(e.source);
        // the hit's attacker, when hostile to the shielded one: the absorb may come first in a new fight
        bool const hostile = e.source && e.source != e.target && !(attacker.m && attacker.m == target.m);
        std::string const& name = e.source ? e.source->GetName() : std::string();
        M::Enemy const enemy{ name, IsBoss(e.source) };
        if (caster.m && caster.pid)
            caster.m->AbsorbDone(now, caster.pid, caster.src, e.spellId, e.amount, hostile ? &enemy : nullptr);
        if (target.m && target.pid && !target.src)
            target.m->AbsorbReceived(now, target.pid, e.spellId, e.amount, hostile ? &enemy : nullptr);
    }

    void OnInterruptOrDispel(CL::Event const& e, uint64 now)
    {
        Who const who = Resolve(e.source);
        if (!who.m || !who.pid)
            return;
        Who const target = Resolve(e.target);
        bool const hostile = e.target != e.source && target.m != who.m;
        M::Enemy const enemy{ e.target->GetName(), IsBoss(e.target) };
        if (e.kind == CL::Kind::Interrupt)
            who.m->Interrupt(now, who.pid, who.src, e.spellId, hostile ? &enemy : nullptr);
        else
            who.m->Dispel(now, who.pid, who.src, e.spellId, e.amount, hostile ? &enemy : nullptr);
    }

    void Sink(CL::Event const& e)
    {
        if (!s_enable || !e.target)
            return;
        uint64 const now = NowMs();
        std::lock_guard<std::mutex> guard(s_lock);
        switch (e.kind)
        {
            case CL::Kind::SpellDamage:
            case CL::Kind::PeriodicDamage:
            case CL::Kind::DamageShield:
                OnDamage(e, now, false, false);
                break;
            case CL::Kind::Melee:
                OnDamage(e, now, e.missInfo != 0, false);
                break;
            case CL::Kind::SpellMiss:
                OnDamage(e, now, true, false);
                break;
            case CL::Kind::Environmental:
                OnDamage(e, now, false, true);
                break;
            case CL::Kind::Heal:
            case CL::Kind::PeriodicHeal:
                OnHeal(e, now);
                break;
            case CL::Kind::Absorb:
                OnAbsorb(e, now);
                break;
            case CL::Kind::Interrupt:
            case CL::Kind::Dispel:
                OnInterruptOrDispel(e, now);
                break;
        }
    }

    // ---- the client --------------------------------------------------------------------------------------------------
    // Sends one body as an addon whisper from the player to themselves; the bytes it took on the wire.
    size_t Send(Player* player, std::string const& body)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, std::string(M::kPrefix) + body);
        player->SendDirectMessage(&data);
        return data.size() + 4; // and the server header (size, opcode)
    }

    void SendAll(Player* player, PlayerMeter& pm, std::vector<std::string> const& bodies, bool inFight)
    {
        size_t bytes = 0;
        for (std::string const& body : bodies)
            bytes += Send(player, body);
        pm.client.bytes += bytes;
        pm.client.messages += bodies.size();
        if (inFight)
            pm.client.fightBytes += bytes;
    }

    // The meter the player belongs to now (switching when the group changed: the old one forgets its combat flag).
    M::Meter& Attach(Player* player, PlayerMeter& pm)
    {
        uint64 const key = MeterKey(player);
        if (pm.meterKey && pm.meterKey != key)
            if (M::Meter* old = FindMeter(pm.meterKey))
                old->Forget(pm.pid);
        Who const w = ResolvePlayer(player);
        pm.meterKey = key;
        pm.pid = w.pid;
        return *w.m;
    }

    void Collect(uint64 now)
    {
        if (now - std::min(now, s_lastGcMs) < kGcEveryMs)
            return;
        s_lastGcMs = now;
        for (auto it = s_meters.begin(); it != s_meters.end();)
            it = now - std::min(now, it->second->lastTouchMs) > kDropAfterMs ? s_meters.erase(it) : std::next(it);
    }

    void Update(Player* player, uint32 diff)
    {
        PlayerMeter* pm = player->CustomData.GetDefault<PlayerMeter>(kDataKey);
        pm->pollMs += diff;
        pm->client.pushMs += diff;
        bool const poll = pm->pollMs >= kCombatPollMs;
        bool const push = pm->client.hello && pm->client.pushMs >= M::kPushMs;
        if (!poll && !push)
            return;
        uint64 const now = NowMs();
        std::vector<std::string> bodies;
        bool inFight = false;
        {
            std::lock_guard<std::mutex> guard(s_lock);
            M::Meter& m = Attach(player, *pm);
            if (poll)
            {
                pm->pollMs = 0;
                m.SetCombat(pm->pid, player->IsInCombat(), now);
                for (Creature* c : Emberfall::Companions::LiveCompanions(player))
                {
                    Who const w = ResolveCompanion(c, player);
                    m.SetCombat(w.pid, c->IsInCombat(), now);
                }
                Collect(now);
            }
            if (push)
            {
                inFight = m.Live() != nullptr;
                if (inFight)
                    pm->client.fightMs += pm->client.pushMs;
                pm->client.pushMs = 0;
                bodies = M::Push(m, pm->client, now);
            }
        }
        if (!bodies.empty())
            SendAll(player, *pm, bodies, inFight);
    }

    bool MayReset(Player* player)
    {
        Group* g = player->GetGroup();
        return !g || g->IsLeader(player->GetGUID());
    }

    // Resets the player's meter (the caller checked MayReset).
    void ResetMeter(Player* player)
    {
        std::lock_guard<std::mutex> guard(s_lock);
        M::Meter& m = MeterFor(MeterKey(player));
        m.Reset(s_nextEpoch++);
        LOG_INFO("module", "Emberfall meter: {} reset the meter of {} (epoch {})", player->GetName(), player->GetGroup() ? "the group" : "themselves", m.epoch);
    }

    void HandleRequest(Player* player, std::string_view body)
    {
        auto const req = M::ParseRequest(body);
        if (!req)
            return;
        PlayerMeter* pm = player->CustomData.GetDefault<PlayerMeter>(kDataKey);
        uint64 const now = NowMs();
        std::vector<std::string> out;
        if (!pm->client.Allow(now))
        {
            if (!pm->client.throttled)
            {
                pm->client.throttled = true;
                out.push_back(M::ErrBody(req->seq, "THROTTLE"));
            }
        }
        else if (!s_enable)
            out.push_back(M::OffBody());
        else if (req->cmd == "RESET")
        {
            if (MayReset(player))
            {
                ResetMeter(player);
                out.push_back(M::OkBody(req->seq));
            }
            else
                out.push_back(M::ErrBody(req->seq, "LEADER"));
        }
        else
        {
            std::lock_guard<std::mutex> guard(s_lock);
            M::Meter& m = Attach(player, *pm);
            if (req->cmd == "HELLO")
            {
                pm->client.hello = true;
                pm->client.Forget();
                out = M::Push(m, pm->client, now);
            }
            else if (req->cmd == "VIEW" && !req->fields.empty())
            {
                M::Segment const* s = req->fields[0] == "c" ? nullptr : M::ResolveSegment(m, req->fields[0]);
                pm->client.view = s ? int64(s->id) : -1;
                out = M::Push(m, pm->client, now);
            }
            else if (req->cmd == "SPELLS" && req->fields.size() >= 3)
            {
                M::Segment const* s = M::ResolveSegment(m, req->fields[0]);
                auto const pid = M::ParseDec(req->fields[1]);
                auto const metric = M::ParseMetric(req->fields[2]);
                if (s && pid && *pid <= 255 && metric)
                    out = M::SpellBodies(req->seq, *s, uint8(*pid), *metric);
                else
                    out.push_back(M::EndBody(req->seq));
            }
            else if (req->cmd == "DEATHS" && req->fields.size() >= 2)
            {
                M::Segment const* s = M::ResolveSegment(m, req->fields[0]);
                auto const pid = M::ParseDec(req->fields[1]);
                if (s && pid && *pid <= 255)
                    out = M::DeathBodies(req->seq, *s, uint8(*pid));
                else
                    out.push_back(M::EndBody(req->seq));
            }
            else
                out.push_back(M::ErrBody(req->seq, "CMD"));
        }
        if (!out.empty())
            SendAll(player, *pm, out, false);
    }

    // ---- scripts ----------------------------------------------------------------------------------------------------
    class emberfall_meter_world : public WorldScript
    {
    public:
        emberfall_meter_world()
            : WorldScript("emberfall_meter_world", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED }) { }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            s_enable = sConfigMgr->GetOption<bool>("Emberfall.Meter.Enable", true, false);
            CL::SetSink(s_enable ? &Sink : nullptr);
        }

        void OnBeforeWorldInitialized() override
        {
            if (s_enable)
                LOG_INFO("server.loading", ">> Emberfall meter: on (EFM, pushes every {} ms, fights end {} ms after combat)", M::kPushMs, M::kGraceMs);
            else
                LOG_INFO("server.loading", ">> Emberfall meter: off (Emberfall.Meter.Enable = 0)");
        }
    };

    class emberfall_meter_player : public PlayerScript
    {
    public:
        emberfall_meter_player()
            : PlayerScript("emberfall_meter_player", { PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT,
                PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT }) { }

        void OnPlayerUpdate(Player* player, uint32 diff) override
        {
            if (s_enable && player && player->IsInWorld())
                Update(player, diff);
        }

        // A fresh start: the client sends HELLO when it wants the meter.
        void OnPlayerLogin(Player* player) override { player->CustomData.Erase(kDataKey); }

        void OnPlayerLogout(Player* player) override
        {
            if (PlayerMeter* pm = player->CustomData.Get<PlayerMeter>(kDataKey))
            {
                std::lock_guard<std::mutex> guard(s_lock);
                if (M::Meter* m = FindMeter(pm->meterKey))
                    m->Forget(pm->pid);
            }
            player->CustomData.Erase(kDataKey);
        }

        // The EFM protocol: every `EFM\t` addon whisper is consumed, only the player's own (to itself) answered.
        bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg, Player* receiver) override
        {
            if (type != CHAT_MSG_WHISPER || language != uint32(LANG_ADDON) || msg.compare(0, M::kPrefix.size(), M::kPrefix) != 0)
                return true;
            if (receiver == player)
                HandleRequest(player, std::string_view(msg).substr(M::kPrefix.size()));
            return false;
        }
    };

    class emberfall_meter_unit : public UnitScript
    {
    public:
        emberfall_meter_unit() : UnitScript("emberfall_meter_unit", true, { UNITHOOK_ON_UNIT_DEATH }) { }

        void OnUnitDeath(Unit* unit, Unit* killer) override
        {
            if (!s_enable || !unit || (!unit->IsPlayer() && !CompanionOwner(unit->ToCreature())))
                return;
            uint64 const now = NowMs();
            std::lock_guard<std::mutex> guard(s_lock);
            Who const w = Resolve(unit);
            if (w.m && w.pid && !w.src)
                w.m->Died(now, w.pid, killer && killer != unit ? killer->GetName() : std::string());
        }
    };

    class emberfall_meter_commands : public CommandScript
    {
    public:
        emberfall_meter_commands() : CommandScript("emberfall_meter_commands") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable meterTable =
            {
                { "status", HandleStatus, SEC_PLAYER, Console::No },
                { "reset",  HandleReset,  SEC_PLAYER, Console::No },
            };
            static ChatCommandTable commandTable =
            {
                { "efmeter", meterTable },
            };
            return commandTable;
        }

        static void Rows(ChatHandler* handler, M::Meter const& m, M::Segment const& s, uint64 now)
        {
            uint64 const ms = s.id == 0 ? m.OverallMs(now) : s.DurationMs(now);
            handler->PSendSysMessage("EFMETER segment {} {} {:.1f}s{}{} \"{}\"", s.id, s.id == 0 ? "overall" : "fight", ms / 1000.0,
                                     s.live ? " live" : "", s.boss ? " boss" : "", s.name);
            for (auto const& [pid, r] : s.rows)
            {
                M::Participant const* p = m.Who(pid);
                M::Totals const& t = r.t;
                handler->PSendSysMessage("EFMETER row {} {} damage {} dps {:.1f} healing {} overheal {} absorbs {} taken {} received {} "
                                         "interrupts {} dispels {} deaths {} active_s {:.1f}",
                                         pid, p ? p->name : "?", t.damage, t.damage * 1000.0 / double(std::max<uint64>(ms, 1)), t.healing,
                                         t.overheal, t.absorbs, t.taken, t.received, t.interrupts, t.dispels, t.deaths, t.activeMs / 1000.0);
            }
        }

        static bool HandleStatus(ChatHandler* handler)
        {
            Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
            if (!player)
                return false;
            if (!s_enable)
            {
                handler->SendSysMessage("EFMETER off (Emberfall.Meter.Enable = 0)");
                return true;
            }
            uint64 const now = NowMs();
            PlayerMeter const* pm = player->CustomData.Get<PlayerMeter>(kDataKey);
            std::lock_guard<std::mutex> guard(s_lock);
            M::Meter const& m = MeterFor(MeterKey(player));
            handler->PSendSysMessage("EFMETER on meter {} epoch {} participants {} rows {} fights {} meters {}", player->GetGroup() ? "group" : "solo",
                                     m.epoch, m.roster.size(), m.Rows(), m.fights.size(), s_meters.size());
            if (pm)
                handler->PSendSysMessage("EFMETER client hello {} bytes {} messages {} fight_bytes {} fight_s {:.1f} fight_bps {:.0f}",
                                         pm->client.hello ? "yes" : "no", pm->client.bytes, pm->client.messages, pm->client.fightBytes,
                                         pm->client.fightMs / 1000.0,
                                         pm->client.fightMs ? pm->client.fightBytes * 1000.0 / double(pm->client.fightMs) : 0.0);
            if (M::Segment const* cur = m.Current())
                Rows(handler, m, *cur, now);
            Rows(handler, m, m.overall, now);
            handler->SendSysMessage("EFMETER end");
            return true;
        }

        static bool HandleReset(ChatHandler* handler)
        {
            Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
            if (!player)
                return false;
            if (!s_enable)
            {
                handler->SendSysMessage("EFMETER off (Emberfall.Meter.Enable = 0)");
                return true;
            }
            if (!MayReset(player))
            {
                handler->SendSysMessage("EFMETER only the party leader can reset the group's meter.");
                return true;
            }
            ResetMeter(player);
            handler->SendSysMessage("EFMETER reset");
            return true;
        }
    };
}

void AddEmberfallMeterScripts()
{
    new emberfall_meter_world();
    new emberfall_meter_player();
    new emberfall_meter_unit();
    new emberfall_meter_commands();
}
