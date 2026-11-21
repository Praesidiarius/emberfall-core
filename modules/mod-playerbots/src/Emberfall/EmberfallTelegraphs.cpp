/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version. Emberfall's part of the module (Copyright Emberfall / Tales of Orrin).
 */

/*
 * Emberfall's bots step out of telegraphs: see EmberfallTelegraphs.h. Released under GNU GPL v2 or later, as the module.
 */

#include "EmberfallTelegraphs.h"

#include "CellImpl.h"
#include "Config.h"
#include "Creature.h"
#include "DynamicObject.h"
#include "EmberfallArena.h"
#include "EmberfallBots.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "LastMovementValue.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "Timer.h"

#include <atomic>
#include <chrono>
#include <list>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#if __has_include("EfTelegraphs.h")
#include "EfTelegraphs.h"
#define EF_BOTS_HAVE_TELEGRAPHS 1
#endif

void EmberfallTelegraphStrategy::InitReactionTriggers(std::vector<TriggerNode*>& triggers)
{
    // a reaction above every other (>= 100: a bot far from players, whose reactions run only every second, still sees it)
    triggers.push_back(new TriggerNode("ef telegraph", {NextAction("ef telegraph", ACTION_EMERGENCY + 15)}));
}

#ifdef EF_BOTS_HAVE_TELEGRAPHS

namespace EmberfallTelegraphs
{
    namespace
    {
        using namespace Emberfall::Telegraphs;

        // The figures ([proposal], one place): the reaction, the margin and the clearance are EfTelegraphLogic.h's
        // Reaction; the rest are the world's checks.
        Reaction const kReaction{};
        constexpr float kMaxRadius = 30.0f;      // a wider area is room-wide: kicked or soaked, not stepped out of
        constexpr float kLedgeYards = 4.0f;      // a spot this much higher or lower than the bot: a ledge, not taken
        constexpr float kShortened = 0.75f;      // the navmesh walk stopped this short of the spot: a wall or an edge
        constexpr float kPackYards = 40.0f;      // idle creatures this near are asked for their reach
        constexpr float kPoolYards = 25.0f;      // ground effects this near are kept out of
        constexpr uint32 kOpenTries = 6;         // spots the world is asked about at most
        constexpr uint32 kHoldAfterMs = 200;     // held past the hit
        constexpr uint32 kSummaryMs = 10 * MINUTE * IN_MILLISECONDS;

        std::atomic<bool> s_on{true};
        std::atomic<uint32> s_running{0};  // the bots' fast path: no avoidable cast running anywhere
        std::mutex s_lock;

        // An avoidable cast running: its area without anyone's size (each bot adds its own).
        struct Running
        {
            ObjectGuid caster;
            uint32 spell = 0;
            uint32 map = 0;
            uint32 instance = 0;
            Danger area;
            uint32 landMs = 0;
        };
        std::vector<Running> s_casts;

        // What a bot made of one: when it reacts, whether it gave up (no way out).
        struct Seen
        {
            ObjectGuid caster;
            uint32 spell = 0;
            uint32 landMs = 0;
            uint32 reactAtMs = 0;
            bool late = false;
            bool givenUp = false;
        };
        std::unordered_map<ObjectGuid, std::vector<Seen>> s_seen;

        struct BotCounts
        {
            uint32 hits = 0, stepped = 0;
        };
        std::unordered_map<ObjectGuid, BotCounts> s_bots;
        std::map<uint32, SpellCount> s_spells;
        Counts s_total;
        Counts s_window;
        uint32 s_windowStart = 0;

        bool Reached(uint32 now, uint32 at) { return int32(now - at) >= 0; }

        uint64 Us(std::chrono::steady_clock::time_point since)
        {
            return uint64(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - since).count());
        }

        bool IsEfBot(Player* player) { return player && EmberfallBots::Enabled() && GET_PLAYERBOT_AI(player) != nullptr; }

        Vec2 At(WorldObject const* o) { return Vec2{o->GetPositionX(), o->GetPositionY()}; }

        // The area for someone of this size (the core's leeway: their size counts).
        Danger Grow(Danger d, float reach)
        {
            d.radius += reach;
            if (d.shape == DangerShape::Line)
                d.width += reach;
            return d;
        }

        void Count(uint32 Counts::*field, uint32 n = 1)
        {
            s_total.*field += n;
            s_window.*field += n;
        }

        // Casts over (landed or broken off) and what bots made of them. Under s_lock.
        void Prune(uint32 now)
        {
            std::erase_if(s_casts, [now](Running const& r) { return Reached(now, r.landMs + 500); });
            for (auto it = s_seen.begin(); it != s_seen.end();)
            {
                std::erase_if(it->second, [now](Seen const& s) { return Reached(now, s.landMs + 500); });
                it = it->second.empty() ? s_seen.erase(it) : std::next(it);
            }
            s_running.store(uint32(s_casts.size()), std::memory_order_relaxed);
        }

        Seen* FindSeen(std::vector<Seen>& seen, Running const& r)
        {
            for (Seen& s : seen)
                if (s.caster == r.caster && s.spell == r.spell && s.landMs == r.landMs)
                    return &s;
            return nullptr;
        }

        Seen NewSeen(Running const& r, uint32 now)
        {
            Seen s;
            s.caster = r.caster;
            s.spell = r.spell;
            s.landMs = r.landMs;
            float const lateRoll = float(rand_norm());
            s.late = lateRoll < kReaction.lateChance;
            s.reactAtMs = now + ReactionMs(kReaction, float(rand_norm()), lateRoll);
            return s;
        }

        // The step out a bot is due for: the area, when it lands, the other areas, whether to break its own cast.
        struct Step
        {
            Danger area;
            ObjectGuid caster;
            uint32 spell = 0;
            uint32 landMs = 0;
            bool late = false;
            bool breakCast = false;
            std::vector<Danger> others;
        };

        bool Due(Player* bot, Step& step)
        {
            auto const t0 = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> guard(s_lock);
            uint32 const now = getMSTime();
            Prune(now);
            Running const* best = nullptr;
            Seen* bestSeen = nullptr;
            float const reach = bot->GetCombatReach();
            Vec2 const self = At(bot);
            std::vector<Seen>* seen = nullptr;
            for (Running const& r : s_casts)
            {
                if (r.map != bot->GetMapId() || r.instance != bot->GetInstanceId() || Reached(now, r.landMs))
                    continue;
                Danger const d = Grow(r.area, reach);
                bool const inside = InDanger(d, self, kReaction.margin);
                if (!seen)
                {
                    auto it = s_seen.find(bot->GetGUID());
                    seen = it == s_seen.end() ? nullptr : &it->second;
                }
                Seen* s = seen ? FindSeen(*seen, r) : nullptr;
                if (!s)
                {
                    // walked in after the cast began: the reaction starts now
                    if (!inside)
                        continue;
                    Unit* caster = ObjectAccessor::GetUnit(*bot, r.caster);
                    if (!caster || !caster->IsHostileTo(bot))
                        continue;
                    seen = &s_seen[bot->GetGUID()];
                    seen->push_back(NewSeen(r, now));
                    Count(&Counts::inside);
                    continue;
                }
                if (!inside || s->givenUp || !Reached(now, s->reactAtMs))
                    continue;
                if (!best || int32(r.landMs - best->landMs) < 0)
                {
                    best = &r;
                    bestSeen = s;
                }
            }
            if (best)
            {
                // still casting it (a broken-off cast lands nothing)
                Unit* caster = ObjectAccessor::GetUnit(*bot, best->caster);
                Spell* cast = caster ? caster->GetCurrentSpell(CURRENT_GENERIC_SPELL) : nullptr;
                if (!cast || cast->GetSpellInfo()->Id != best->spell || cast->getState() != SPELL_STATE_PREPARING)
                {
                    bestSeen->givenUp = true;
                    best = nullptr;
                }
            }
            if (best)
            {
                step.area = Grow(best->area, reach);
                step.caster = best->caster;
                step.spell = best->spell;
                step.landMs = best->landMs;
                step.late = bestSeen->late;
                for (Running const& r : s_casts)
                    if (&r != best && r.map == bot->GetMapId() && r.instance == bot->GetInstanceId() && !Reached(now, r.landMs))
                        step.others.push_back(Grow(r.area, reach));
                // a caster's own cast: finished when there is time to walk out after it, else broken off
                step.breakCast = false;
                if (Spell* own = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL); own && own->getState() == SPELL_STATE_PREPARING)
                {
                    Vec2 const out = EscapePoint(step.area, self, kReaction.clear, self);
                    uint32 const walk = WalkMs(kReaction, Dist(self, out));
                    OwnCast const rule = OwnCastRule(kReaction, uint32(std::max(own->GetCastTimeRemaining(), 0)), walk, best->landMs - now);
                    if (rule == OwnCast::Finish)
                        best = nullptr;
                    else
                        step.breakCast = true;
                }
                else if (bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
                    step.breakCast = true;
            }
            s_total.checkUs += Us(t0);
            ++s_total.checkCalls;
            return best != nullptr;
        }

        void GiveUp(Player* bot, Step const& step)
        {
            std::lock_guard<std::mutex> guard(s_lock);
            auto it = s_seen.find(bot->GetGUID());
            if (it == s_seen.end())
                return;
            for (Seen& s : it->second)
                if (s.caster == step.caster && s.spell == step.spell && s.landMs == step.landMs)
                    s.givenUp = true;
            Count(&Counts::noWay);
        }

        void NoteStep(Player* bot, bool inTime)
        {
            std::lock_guard<std::mutex> guard(s_lock);
            Count(&Counts::stepped);
            if (inTime)
                Count(&Counts::inTime);
            ++s_bots[bot->GetGUID()].stepped;
        }

        // ---- the hooks ------------------------------------------------------------------------------------------------
        class EmberfallTelegraphSpellScript : public AllSpellScript
        {
        public:
            EmberfallTelegraphSpellScript() : AllSpellScript("EmberfallTelegraphSpellScript", {ALLSPELLHOOK_ON_PREPARE}) {}

            // The cast's start: the area, and the bots it will cover. Every other spell leaves at the first checks. With
            // the switch off the cast is still counted (the off/on comparison), and no bot reacts.
            void OnSpellPrepare(Spell* spell, Unit* caster, SpellInfo const* /*info*/) override
            {
                if (!caster || !caster->IsCreature())
                    return;
                CastFacts const facts = FactsOf(spell);
                if (!Avoidable(facts))
                    return;
                auto const t0 = std::chrono::steady_clock::now();
                Danger area;
                if (!DangerOf(caster, spell, 0.0f, kMaxRadius, area))
                    return;
                bool const watched = Watches(s_on.load(std::memory_order_relaxed), facts);
                uint32 const now = getMSTime();
                Running r;
                r.caster = caster->GetGUID();
                r.spell = spell->GetSpellInfo()->Id;
                r.map = caster->GetMapId();
                r.instance = caster->GetInstanceId();
                r.area = area;
                r.landMs = now + uint32(spell->GetCastTime());
                // the players the area may reach: one search round the caster, for this cast only
                float const range = Dist(At(caster), area.origin) + area.radius + area.width + 10.0f;
                std::list<Player*> players;
                Acore::AnyPlayerInObjectRangeCheck check(caster, range, true);
                Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(caster, players, check);
                Cell::VisitObjects(caster, searcher, range);
                std::lock_guard<std::mutex> guard(s_lock);
                Prune(now);
                if (watched)
                {
                    s_casts.push_back(r);
                    s_running.store(uint32(s_casts.size()), std::memory_order_relaxed);
                }
                uint32 inside = 0;
                for (Player* p : players)
                {
                    if (!IsEfBot(p) || !caster->IsHostileTo(p))
                        continue;
                    if (!InDanger(Grow(area, p->GetCombatReach()), At(p), kReaction.margin))
                        continue;
                    ++inside;
                    if (!watched)
                        continue;
                    Seen const s = NewSeen(r, now);
                    s_seen[p->GetGUID()].push_back(s);
                    LOG_DEBUG("playerbots", "Emberfall telegraph: {} (level {}) is in {}'s {} ({} ms cast), reacts in {} ms{}",
                              p->GetName(), p->GetLevel(), caster->GetName(), spell->GetSpellInfo()->SpellName[0],
                              spell->GetCastTime(), s.reactAtMs - now, s.late ? " (late)" : "");
                }
                SpellCount& sc = s_spells[r.spell];
                sc.shape = area.shape == DangerShape::Cone ? 'c' : area.shape == DangerShape::Line ? 'l'
                           : Dist(area.origin, At(caster)) < 0.5f ? 'r' : 'g';
                ++sc.casts;
                if (inside)
                {
                    ++sc.overBots;
                    Count(&Counts::casts);
                    Count(&Counts::inside, inside);
                }
                s_total.hookUs += Us(t0);
                ++s_total.hookCalls;
            }
        };

        class EmberfallTelegraphUnitScript : public UnitScript
        {
        public:
            EmberfallTelegraphUnitScript() : UnitScript("EmberfallTelegraphUnitScript", true, {UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN}) {}

            // An avoidable hit that landed on a bot (counted with the switch off as well: the off/on comparison).
            void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo) override
            {
                if (!target || !target->IsPlayer() || !spellInfo || damage <= 0 || !attacker || !attacker->IsCreature())
                    return;
                if (!TelegraphedWithDamage(spellInfo->Id))
                    return;
                Player* bot = target->ToPlayer();
                if (!IsEfBot(bot))
                    return;
                std::lock_guard<std::mutex> guard(s_lock);
                Count(&Counts::hits);
                ++s_bots[bot->GetGUID()].hits;
                ++s_spells[spellInfo->Id].hits;
                LOG_DEBUG("playerbots", "Emberfall telegraph hit: {} (level {}) by {}'s {} for {}", bot->GetName(), bot->GetLevel(),
                          attacker->GetName(), spellInfo->SpellName[0], damage);
            }
        };

        class EmberfallTelegraphWorldScript : public WorldScript
        {
        public:
            EmberfallTelegraphWorldScript()
                : WorldScript("EmberfallTelegraphWorldScript", {WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_UPDATE})
            {
            }

            void OnAfterConfigLoad(bool /*reload*/) override
            {
                s_on.store(sConfigMgr->GetOption<bool>("Playerbots.Emberfall.AvoidTelegraphs", true), std::memory_order_relaxed);
            }

            // The realm's line every 10 minutes while bots met telegraphs (info: a realm keeps it).
            void OnUpdate(uint32 /*diff*/) override
            {
                uint32 const now = getMSTime();
                if (!s_windowStart)
                    s_windowStart = now;
                if (getMSTimeDiff(s_windowStart, now) < kSummaryMs)
                    return;
                std::lock_guard<std::mutex> guard(s_lock);
                if (s_window.inside || s_window.hits)
                    LOG_INFO("playerbots",
                             "Emberfall telegraphs: last {} min: {} casts over bots, {} bots inside, {} stepped out ({} in time), "
                             "{} without a way out, {} hits on bots (switch {})",
                             kSummaryMs / MINUTE / IN_MILLISECONDS, s_window.casts, s_window.inside, s_window.stepped,
                             s_window.inTime, s_window.noWay, s_window.hits, s_on.load() ? "on" : "off");
                s_window = Counts();
                s_windowStart = now;
            }
        };

        // A hostile ground effect near (a pool, a burning patch): kept out of.
        struct HostileGround
        {
            Player* bot;
            float range;
            bool operator()(WorldObject* o) const
            {
                DynamicObject* dyn = o->ToDynObject();
                if (!dyn || !bot->IsWithinDist(dyn, range))
                    return false;
                Unit* caster = dyn->GetCaster();
                return caster && caster->IsHostileTo(bot);
            }
        };
    }

    bool On() { return s_on.load(std::memory_order_relaxed) && EmberfallBots::Enabled(); }

    void AddScripts()
    {
        EmberfallArena::AddScripts();  // the test stacks' measurement (off unless Playerbots.Emberfall.TelegraphArena)
        new EmberfallTelegraphSpellScript();
        new EmberfallTelegraphUnitScript();
        new EmberfallTelegraphWorldScript();
    }

    Counts Total()
    {
        std::lock_guard<std::mutex> guard(s_lock);
        return s_total;
    }

    uint32 HitsOn(Player* bot)
    {
        std::lock_guard<std::mutex> guard(s_lock);
        auto it = s_bots.find(bot->GetGUID());
        return it == s_bots.end() ? 0 : it->second.hits;
    }

    uint32 SteppedOut(Player* bot)
    {
        std::lock_guard<std::mutex> guard(s_lock);
        auto it = s_bots.find(bot->GetGUID());
        return it == s_bots.end() ? 0 : it->second.stepped;
    }

    std::map<uint32, SpellCount> Spells()
    {
        std::lock_guard<std::mutex> guard(s_lock);
        return s_spells;
    }

    void ResetCounts()
    {
        std::lock_guard<std::mutex> guard(s_lock);
        s_bots.clear();
        s_spells.clear();
        s_total = Counts();
    }
}

bool EmberfallTelegraphTrigger::IsActive()
{
    // the fast path of every bot's look: no avoidable cast running anywhere
    if (!EmberfallTelegraphs::s_running.load(std::memory_order_relaxed) || !EmberfallTelegraphs::On())
        return false;
    if (!bot->IsAlive() || bot->IsInFlight() || bot->GetVehicle())
        return false;
    EmberfallTelegraphs::Step step;
    return EmberfallTelegraphs::Due(bot, step);
}

bool EmberfallTelegraphAction::isPossible() { return IsMovingAllowed(); }

bool EmberfallTelegraphAction::Execute(Event /*event*/)
{
    using namespace Emberfall::Telegraphs;
    using namespace EmberfallTelegraphs;
    Step step;
    if (!Due(bot, step))
        return false;
    if (step.breakCast)
        bot->InterruptNonMeleeSpells(false);

    Vec2 const self = At(bot);
    float const bz = bot->GetPositionZ();
    // towards the bot's foe (a step out of a circle's very middle keeps the fight in reach)
    Unit* target = AI_VALUE(Unit*, "current target");
    Vec2 const prefer = target ? At(target) : self;

    // ground effects to keep out of, and the idle creatures whose reach the spot must stay out of
    std::vector<Pool> pools;
    {
        std::list<WorldObject*> objects;
        HostileGround check{bot, kPoolYards};
        Acore::WorldObjectListSearcher<HostileGround> searcher(bot, objects, check, GRID_MAP_TYPE_MASK_DYNAMICOBJECT);
        Cell::VisitObjects(bot, searcher, kPoolYards);
        for (WorldObject* o : objects)
            if (DynamicObject* dyn = o->ToDynObject())
                pools.push_back(Pool{At(dyn), dyn->GetRadius() + bot->GetCombatReach()});
    }
    struct Pack
    {
        Vec2 at;
        float reach;
    };
    std::vector<Pack> packs;
    {
        std::list<Unit*> units;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(bot, bot, kPackYards);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(bot, units, check);
        Cell::VisitObjects(bot, searcher, kPackYards);
        for (Unit* u : units)
            if (Creature* c = u->ToCreature(); c && c->IsAlive() && !c->IsInCombat() && !c->IsCritter() && c->IsHostileTo(bot))
                packs.push_back(Pack{At(c), c->GetAggroRange(bot)});
    }

    // the world's checks on a spot (the stock flee's): a straight walk on the navmesh that gets there (no wall, no
    // edge), at about the bot's height (no ledge), outside every idle creature's reach
    float spotZ = bz;
    auto open = [&](Vec2 c) {
        float x = c.x, y = c.y, z = bz;
        if (!bot->GetMap()->CheckCollisionAndGetValidCoords(bot, self.x, self.y, bz, x, y, z))
            return false;
        if (std::hypot(x - c.x, y - c.y) > kShortened || std::fabs(z - bz) > kLedgeYards)
            return false;
        for (Pack const& p : packs)
            if (Dist(p.at, Vec2{x, y}) <= p.reach)
                return false;
        spotZ = z;
        return true;
    };
    std::optional<Vec2> spot = PickEscape(step.area, self, kReaction.margin, kReaction.clear, prefer, step.others, pools, open, kOpenTries);
    Unit* caster = ObjectAccessor::GetUnit(*bot, step.caster);
    char const* what = caster && caster->GetCurrentSpell(CURRENT_GENERIC_SPELL)
                           ? caster->GetCurrentSpell(CURRENT_GENERIC_SPELL)->GetSpellInfo()->SpellName[0]
                           : "?";
    if (!spot)
    {
        GiveUp(bot, step);
        LOG_DEBUG("playerbots", "Emberfall telegraph: {} (level {}) finds no way out of {}'s {}", bot->GetName(), bot->GetLevel(),
                  caster ? caster->GetName() : "?", what);
        return false;
    }

    // a forced move: the bot's other moves (closing in, following) wait while it holds
    LastMovement& last = AI_VALUE(LastMovement&, "last movement");
    if (last.priority == MovementPriority::MOVEMENT_FORCED)
        last.lastdelayTime = 0.0f;
    if (!MoveTo(bot->GetMapId(), spot->x, spot->y, spotZ, false, false, true, false, MovementPriority::MOVEMENT_FORCED))
    {
        GiveUp(bot, step);
        return false;
    }
    uint32 const now = getMSTime();
    uint32 const walk = WalkMs(kReaction, Dist(self, *spot));
    uint32 const left = int32(step.landMs - now) > 0 ? step.landMs - now : 0;
    bool const inTime = uint64(walk) + kReaction.safetyMs <= left;
    // held until the hit has landed (melee: else it closes in again at once); a ranged bot fights on once there
    uint32 const hold = EmberfallBots::FightsRanged(bot) ? walk + kHoldAfterMs : std::max(walk, left) + kHoldAfterMs;
    last.Set(bot->GetMapId(), spot->x, spot->y, spotZ, bot->GetOrientation(), float(hold), MovementPriority::MOVEMENT_FORCED);
    SetDuration(hold);
    NoteStep(bot, inTime);
    LOG_DEBUG("playerbots", "Emberfall telegraph: {} (level {}) steps {:.1f} yd out of {}'s {}, {} ms before it lands{}{}",
              bot->GetName(), bot->GetLevel(), Dist(self, *spot), caster ? caster->GetName() : "?", what, left,
              inTime ? "" : " (too late)", step.breakCast ? ", its own cast broken off" : "");
    return true;
}

#else

namespace EmberfallTelegraphs
{
    bool On() { return false; }
    void AddScripts() { EmberfallArena::AddScripts(); }
    Counts Total() { return Counts(); }
    uint32 HitsOn(Player*) { return 0; }
    uint32 SteppedOut(Player*) { return 0; }
    std::map<uint32, SpellCount> Spells() { return {}; }
    void ResetCounts() {}
}

bool EmberfallTelegraphTrigger::IsActive() { return false; }
bool EmberfallTelegraphAction::isPossible() { return false; }
bool EmberfallTelegraphAction::Execute(Event /*event*/) { return false; }

#endif
