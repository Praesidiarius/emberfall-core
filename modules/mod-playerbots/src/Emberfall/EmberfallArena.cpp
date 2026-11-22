/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version. Emberfall's part of the module (Copyright Emberfall / Tales of Orrin).
 */

/*
 * The telegraph arena (Emberfall docs/design/playerbots-pilot.md §20): a test stack's measurement of the bots' step out
 * of telegraphs, no client needed. Playerbots.Emberfall.TelegraphArena = "SPAWN BOTS PULLS [ab] [NAME...]" (empty, the default: off;
 * never on a realm): once the bots are in the world, BOTS random bots (of those NAMEs, if given: the same team for
 * the switch off and on; with `ab` the pulls alternate the switch off and on, one team in one world) within 5
 * levels of the creature of `creature`
 * row SPAWN are put in one group (up to five; more fight ungrouped), taken to it (into its instance for a dungeon
 * boss) and set on it, PULLS times; after each pull the dead are raised, everyone healed and the creature reset. One
 * "Emberfall arena:" line a pull (its length, the creature's end, the deaths, the avoidable casts and the hits per bot,
 * the steps out, the world's update time) and one at the end (per spell, the cost of the hook and the looks).
 * Released under GNU GPL v2 or later, as the module.
 */

#include "EmberfallArena.h"

#include "Config.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "DBCStores.h"
#include "EmberfallBots.h"
#include "EmberfallTelegraphs.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "Timer.h"
#include "UpdateTime.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

namespace EmberfallArena
{
    namespace
    {
        enum class Phase : uint8 { Off, Wait, Gather, Arrive, Pull, Fight, Reset, Done };

        constexpr uint32 kTickMs = 500;
        constexpr uint32 kSettleMs = 120000;     // after the world's start: logins and the first randomize
        constexpr uint32 kArriveMs = 60000;
        constexpr uint32 kFightMs = 300000;
        constexpr uint32 kResetMs = 12000;
        constexpr uint32 kMaxGroup = 5;

        struct Arena
        {
            uint32 spawn = 0, bots = 0, pulls = 0;
            std::vector<std::string> names;  // a fixed team (the same bots off and on), else the first that fit
            bool alternate = false;          // "ab": the switch off on odd pulls, on on even ones (one team, one world)
            Phase phase = Phase::Off;
            uint32 since = 0, last = 0;
            uint32 map = 0, level = 0, pull = 0;
            float x = 0, y = 0, z = 0;
            std::vector<ObjectGuid> team;
            ObjectGuid creature;
            std::vector<bool> died;
            std::vector<bool> sent;
            uint64 tickSum = 0;
            uint32 tickN = 0, tickMax = 0;
            // the run's sums
            uint32 kills = 0, deaths = 0, hits = 0, stepped = 0;
            float seconds = 0;
        };
        Arena s_arena;

        bool Reached(uint32 now, uint32 at) { return int32(now - at) >= 0; }

        void Go(Phase p)
        {
            s_arena.phase = p;
            s_arena.since = getMSTime();
        }

        std::vector<Player*> Team()
        {
            std::vector<Player*> out;
            for (ObjectGuid const& g : s_arena.team)
                if (Player* p = ObjectAccessor::FindConnectedPlayer(g))
                    out.push_back(p);
            return out;
        }

        void Raise(Player* p)
        {
            if (!p->IsAlive())
            {
                p->ResurrectPlayer(1.0f);
                p->SpawnCorpseBones();
            }
            p->CombatStop(true);
            p->SetFullHealth();
            if (p->GetMaxPower(POWER_MANA) > 0)
                p->SetPower(POWER_MANA, p->GetMaxPower(POWER_MANA));
        }

        Creature* FindCreature(Player* anyone)
        {
            if (!anyone || !anyone->IsInWorld() || anyone->IsBeingTeleported() || anyone->GetMapId() != s_arena.map)
                return nullptr;
            auto range = anyone->GetMap()->GetCreatureBySpawnIdStore().equal_range(s_arena.spawn);
            return range.first == range.second ? nullptr : range.first->second;
        }

        // the creature, seen from whoever of the team is on its map (a dead bot released in a dungeon is outside)
        Creature* FindCreature(std::vector<Player*> const& team)
        {
            for (Player* p : team)
                if (Creature* c = FindCreature(p))
                    return c;
            return nullptr;
        }

        void StartFight(std::vector<Player*> const& team, Creature* c, bool engage)
        {
            if (s_arena.alternate)
                EmberfallTelegraphs::SetSwitch(s_arena.pull % 2 == 1);
            EmberfallTelegraphs::ResetCounts();
            s_arena.died.assign(s_arena.team.size(), false);
            s_arena.tickSum = s_arena.tickN = s_arena.tickMax = 0;
            if (engage)
            {
                for (Player* p : team)
                {
                    Raise(p);
                    c->EngageWithTarget(p);
                }
                if (c->IsAIEnabled && !team.empty())
                    c->AI()->AttackStart(team[0]);
            }
            s_arena.creature = c->GetGUID();
            Go(Phase::Fight);
        }

        // where a bot stands at a pull's start: round the creature's spawn, 3 yd out
        void TakeThere(Player* p, uint32 i)
        {
            float const a = float(i) * 2.0f * float(M_PI) / float(std::max<size_t>(1, s_arena.team.size()));
            p->TeleportTo(s_arena.map, s_arena.x + std::cos(a) * 3.0f, s_arena.y + std::sin(a) * 3.0f, s_arena.z + 0.5f, a + float(M_PI));
        }

        bool Pick()
        {
            CreatureData const* data = sObjectMgr->GetCreatureData(s_arena.spawn);
            CreatureTemplate const* tmpl = data ? sObjectMgr->GetCreatureTemplate(data->id) : nullptr;
            if (!tmpl)
            {
                LOG_ERROR("playerbots", "Emberfall arena: no creature row {}: off", s_arena.spawn);
                Go(Phase::Done);
                return false;
            }
            s_arena.map = data->mapid;
            s_arena.x = data->posX;
            s_arena.y = data->posY;
            s_arena.z = data->posZ;
            s_arena.level = tmpl->minlevel;
            std::vector<Player*> pool;
            for (auto const& [guid, p] : ObjectAccessor::GetPlayers())
            {
                if (!p || !p->IsInWorld() || !p->IsAlive() || p->IsBeingTeleported() || (p->GetGroup() && s_arena.names.empty()) ||
                    p->IsInCombat() || p->IsInFlight() || !GET_PLAYERBOT_AI(p) || !EmberfallBots::SpecOf(p))
                    continue;
                if (std::abs(int32(p->GetLevel()) - int32(s_arena.level)) > 5)
                    continue;
                if (!s_arena.names.empty() &&
                    std::find(s_arena.names.begin(), s_arena.names.end(), p->GetName()) == s_arena.names.end())
                    continue;
                pool.push_back(p);
            }
            if (pool.size() < s_arena.bots)
                return false;
            std::sort(pool.begin(), pool.end(), [](Player* a, Player* b) { return a->GetGUID() < b->GetGUID(); });
            pool.resize(s_arena.bots);
            s_arena.team.clear();
            for (Player* p : pool)
                s_arena.team.push_back(p->GetGUID());
            // a named team may still be grouped from an earlier run (groups are kept): out of it first
            for (Player* p : pool)
                if (Group* old = p->GetGroup())
                    old->RemoveMember(p->GetGUID());
            if (pool.size() > 1 && pool.size() <= kMaxGroup)
            {
                Group* g = new Group();
                if (!g->Create(pool[0]))
                    delete g;
                else
                {
                    sGroupMgr->AddGroup(g);
                    for (size_t i = 1; i < pool.size(); ++i)
                        g->AddMember(pool[i]);
                }
            }
            std::ostringstream names;
            for (Player* p : pool)
            {
                EmberfallBots::Spec const* spec = EmberfallBots::SpecOf(p);
                names << ' ' << p->GetName() << '(' << uint32(p->GetLevel()) << ',' << (spec ? spec->key : "-") << ')';
            }
            LOG_INFO("playerbots", "Emberfall arena: creature {} (entry {}, level {}, map {}), {} pulls, telegraphs {}, team:{}",
                     s_arena.spawn, data->id, s_arena.level, s_arena.map, s_arena.pulls, EmberfallTelegraphs::On() ? "on" : "off",
                     names.str());
            return true;
        }

        void EndPull(Creature* c, char const* how)
        {
            uint32 const now = getMSTime();
            float const seconds = float(getMSTimeDiff(s_arena.since, now)) / 1000.0f;
            std::vector<Player*> const team = Team();
            uint32 dead = 0, hits = 0, stepped = 0;
            std::ostringstream perBot;
            for (size_t i = 0; i < s_arena.team.size(); ++i)
            {
                Player* p = ObjectAccessor::FindConnectedPlayer(s_arena.team[i]);
                bool const died = (i < s_arena.died.size() && s_arena.died[i]) || (p && !p->IsAlive());
                uint32 const h = p ? EmberfallTelegraphs::HitsOn(p) : 0, st = p ? EmberfallTelegraphs::SteppedOut(p) : 0;
                dead += died ? 1 : 0;
                hits += h;
                stepped += st;
                perBot << ' ' << (p ? p->GetName() : "?") << ':' << h << '/' << st << (died ? "+" : "");
            }
            std::ostringstream spells;
            uint32 casts = 0;
            for (auto const& [id, sc] : EmberfallTelegraphs::Spells())
            {
                spells << ' ' << id << sc.shape << ':' << sc.casts << '/' << sc.overBots << '/' << sc.hits;
                casts += sc.casts;
            }
            float const mean = s_arena.tickN ? float(s_arena.tickSum) / float(s_arena.tickN) : 0.0f;
            bool const killed = c && !c->IsAlive();
            LOG_INFO("playerbots",
                     "Emberfall arena: pull {} ({}): {:.1f} s, creature {} ({:.0f} %), deaths {}/{}, avoidable casts {}, hits {} ({:.2f} a bot), "
                     "stepped out {}, per bot (hits/steps, + died):{}, spells (id shape: casts/over bots/hits):{}, world update "
                     "mean {:.1f} ms max {} ms, telegraphs {}",
                     s_arena.pull + 1, how, seconds, killed ? "killed" : "alive", c ? c->GetHealthPct() : 0.0f, dead,
                     s_arena.team.size(), casts, hits, float(hits) / float(std::max<size_t>(1, s_arena.team.size())), stepped,
                     perBot.str(), spells.str(), mean, s_arena.tickMax, EmberfallTelegraphs::On() ? "on" : "off");
            s_arena.kills += killed ? 1 : 0;
            s_arena.deaths += dead;
            s_arena.hits += hits;
            s_arena.stepped += stepped;
            s_arena.seconds += seconds;
            Go(Phase::Reset);
        }

        void Tick()
        {
            uint32 const now = getMSTime();
            switch (s_arena.phase)
            {
                case Phase::Off:
                case Phase::Done:
                    return;
                case Phase::Wait:
                    if (getMSTimeDiff(s_arena.since, now) >= kSettleMs && Pick())
                        Go(Phase::Gather);
                    return;
                case Phase::Gather:
                {
                    // the leader first (a dungeon's instance is made for the group as it enters), the others after it
                    std::vector<Player*> const team = Team();
                    if (team.empty())
                    {
                        Go(Phase::Done);
                        return;
                    }
                    Raise(team[0]);
                    TakeThere(team[0], 0);
                    s_arena.sent.assign(s_arena.team.size(), false);
                    s_arena.sent[0] = true;
                    Go(Phase::Arrive);
                    return;
                }
                case Phase::Arrive:
                case Phase::Reset:
                {
                    std::vector<Player*> const team = Team();
                    bool there = !team.empty();
                    for (Player* p : team)
                        there = there && p->IsInWorld() && !p->IsBeingTeleported() && p->GetMapId() == s_arena.map &&
                                p->GetInstanceId() == team[0]->GetInstanceId();
                    Creature* c = FindCreature(team);
                    if (s_arena.phase == Phase::Reset)
                    {
                        // the creature back to full, the team raised and back at the spot, then a pause
                        if (getMSTimeDiff(s_arena.since, now) < 1000)
                        {
                            if (c && c->IsAlive())
                            {
                                c->CombatStop(true);
                                c->SetFullHealth();
                                if (c->IsAIEnabled)
                                    c->AI()->EnterEvadeMode();
                            }
                            for (size_t i = 0; i < team.size(); ++i)
                            {
                                Raise(team[i]);
                                TakeThere(team[i], uint32(i));
                            }
                            s_arena.since -= 1000;  // once
                            return;
                        }
                        if (getMSTimeDiff(s_arena.since, now) < kResetMs)
                            return;
                        if (++s_arena.pull >= s_arena.pulls)
                        {
                            float const n = float(std::max<uint32>(1, s_arena.pulls));
                            float const b = float(std::max<size_t>(1, s_arena.team.size()));
                            EmberfallTelegraphs::Cost const t = EmberfallTelegraphs::Costs();
                            LOG_INFO("playerbots",
                                     "Emberfall arena: done, {} pulls, telegraphs {}: killed {}, {:.2f} deaths a pull, {:.2f} hits a bot a "
                                     "pull, {:.2f} steps out a bot a pull, {:.1f} s a pull; since the start: hook {} calls {:.1f} us each, "
                                     "looks {} {:.3f} us each",
                                     s_arena.pulls, EmberfallTelegraphs::On() ? "on" : "off", s_arena.kills, s_arena.deaths / n,
                                     s_arena.hits / n / b, s_arena.stepped / n / b, s_arena.seconds / n, t.hookCalls,
                                     t.hookCalls ? double(t.hookNs) / 1000.0 / double(t.hookCalls) : 0.0, t.checkCalls,
                                     t.checkCalls ? double(t.checkNs) / 1000.0 / double(t.checkCalls) : 0.0);
                            Go(Phase::Done);
                            return;
                        }
                        Go(Phase::Pull);
                        return;
                    }
                    if (!team.empty() && team[0]->IsInWorld() && !team[0]->IsBeingTeleported() && team[0]->GetMapId() == s_arena.map)
                        for (size_t i = 1; i < team.size() && i < s_arena.sent.size(); ++i)
                            if (!s_arena.sent[i])
                            {
                                Raise(team[i]);
                                TakeThere(team[i], uint32(i));
                                s_arena.sent[i] = true;
                            }
                    if (there && c)
                        Go(Phase::Pull);
                    else if (getMSTimeDiff(s_arena.since, now) >= kArriveMs)
                    {
                        LOG_ERROR("playerbots", "Emberfall arena: the team did not reach creature {} in {} s: off", s_arena.spawn,
                                  kArriveMs / 1000);
                        Go(Phase::Done);
                    }
                    return;
                }
                case Phase::Pull:
                {
                    std::vector<Player*> const team = Team();
                    Creature* c = FindCreature(team);
                    if (!c)
                    {
                        // the team is not at the creature (released outside a dungeon, or still arriving): fetch it again
                        if (getMSTimeDiff(s_arena.since, now) >= 15000)
                            Go(Phase::Gather);
                        return;
                    }
                    if (!c->IsAlive())
                    {
                        c->Respawn(true);
                        return;
                    }
                    if (c->IsInEvadeMode())
                        return;
                    // it found the team first (they arrive within its reach): the fight is on already
                    StartFight(team, c, !c->IsInCombat());
                    return;
                }
                case Phase::Fight:
                {
                    std::vector<Player*> const team = Team();
                    Creature* c = FindCreature(team);
                    uint32 const t = sWorldUpdateTime.GetLastUpdateTime();
                    s_arena.tickSum += t;
                    ++s_arena.tickN;
                    s_arena.tickMax = std::max(s_arena.tickMax, t);
                    bool allDead = true;
                    for (size_t i = 0; i < s_arena.team.size(); ++i)
                        if (Player* p = ObjectAccessor::FindConnectedPlayer(s_arena.team[i]))
                        {
                            if (!p->IsAlive())
                                s_arena.died[i] = true;
                            else
                                allDead = false;
                        }
                    if (allDead)
                        EndPull(c, "wiped");
                    else if (c && !c->IsAlive())
                        EndPull(c, "killed");
                    else if (!c)
                        EndPull(c, "lost");
                    else if (getMSTimeDiff(s_arena.since, now) >= kFightMs)
                        EndPull(c, "timeout");
                    else if (getMSTimeDiff(s_arena.since, now) >= 10000 && !c->IsInCombat())
                        EndPull(c, "reset");
                    return;
                }
            }
        }

        class EmberfallArenaWorldScript : public WorldScript
        {
        public:
            EmberfallArenaWorldScript() : WorldScript("EmberfallArenaWorldScript", {WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_UPDATE}) {}

            void OnAfterConfigLoad(bool reload) override
            {
                if (reload)
                    return;  // set once, at the start
                std::string const spec = sConfigMgr->GetOption<std::string>("Playerbots.Emberfall.TelegraphArena", "");
                std::istringstream in(spec);
                Arena a;
                if (spec.empty() || !(in >> a.spawn >> a.bots >> a.pulls) || !a.spawn || !a.bots || !a.pulls)
                    return;
                for (std::string name; in >> name;)
                {
                    if (name == "ab")
                        a.alternate = true;
                    else
                        a.names.push_back(name);
                }
                s_arena = a;
                Go(Phase::Wait);
                LOG_INFO("playerbots", "Emberfall arena: set for creature {}, {} bots, {} pulls", a.spawn, a.bots, a.pulls);
            }

            void OnUpdate(uint32 /*diff*/) override
            {
                if (s_arena.phase == Phase::Off || s_arena.phase == Phase::Done)
                    return;
                uint32 const now = getMSTime();
                if (!Reached(now, s_arena.last + kTickMs))
                    return;
                s_arena.last = now;
                Tick();
            }
        };
    }

    void AddScripts() { new EmberfallArenaWorldScript(); }
}
