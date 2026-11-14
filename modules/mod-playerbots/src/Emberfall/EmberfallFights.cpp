/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version. Emberfall's part of the module (Copyright Emberfall / Tales of Orrin).
 */

/*
 * Emberfall's part of mod-playerbots: the fight records of EmberfallFights.h. Released under GNU GPL v2 or later.
 */

#include "EmberfallFights.h"

#include "CellImpl.h"
#include "CombatManager.h"
#include "Creature.h"
#include "EmberfallBots.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ScriptMgr.h"
#include "Timer.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

namespace EmberfallFights
{
    namespace
    {
        struct Foe
        {
            uint8 level = 0;
            uint8 rank = 0;
            uint32 entry = 0;
            uint32 joinMs = 0;  // after the fight's start
        };

        struct Fight
        {
            uint32 startMs = 0;
            float hp0 = 0.0f, mp0 = 0.0f, minHp = 100.0f;
            char const* state = "stand";  // rest (seated), walk, stand
            int32 rpg = -1;               // the rpg brain's status (NewRpgStatus)
            char const* first = "?";      // pulled: the bot's damage came first; attacked: the creature's
            uint32 zone = 0, area = 0;
            float x = 0.0f, y = 0.0f;
            uint32 firstEntry = 0;
            uint32 near15 = 0, near30 = 0;  // idle hostile creatures that near the first foe at the start
            uint32 nMax = 0;
            std::map<ObjectGuid, Foe> foes;
            uint64 dealt = 0, taken = 0;
            uint32 firstDealMs = 0, lastDealMs = 0;
            uint32 defensives = 0, kills = 0;
            bool escaped = false;
            // the lethal blow
            bool dying = false;
            uint32 nDeath = 0;
            std::string deathFoes;
            std::string killer = "-";
        };

        struct Losses
        {
            float health[2] = {0.0f, 0.0f};  // percent of the maximum, the last fight lived first
            float mana[2] = {0.0f, 0.0f};
        };

        std::mutex s_lock;
        std::unordered_map<uint64, Fight> s_fights;
        std::unordered_map<uint64, Losses> s_losses;

        struct DeathSpot
        {
            uint32 map = 0;
            float x = 0.0f, y = 0.0f;
            uint32 ms = 0;  // when (getMSTime)
        };
        std::unordered_map<uint64, DeathSpot> s_deathSpots;

        bool IsEfBot(Player* player)
        {
            return player && EmberfallBots::Enabled() && GET_PLAYERBOT_AI(player) != nullptr;
        }

        Player* EfBot(Unit* unit)
        {
            if (!unit || !unit->IsPlayer())
                return nullptr;
            Player* player = unit->ToPlayer();
            return IsEfBot(player) ? player : nullptr;
        }

        float ManaPct(Player* bot)
        {
            return bot->GetMaxPower(POWER_MANA) > 0 ? bot->GetPowerPct(POWER_MANA) : 100.0f;
        }

        // idle hostile creatures standing near the fight's first foe (a camp or a pack round it)
        void CountNear(Player* bot, Unit* enemy, Fight& f)
        {
            std::list<Unit*> units;
            Acore::AnyUnfriendlyUnitInObjectRangeCheck check(enemy, bot, 30.0f);
            Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(enemy, units, check);
            Cell::VisitObjects(enemy, searcher, 30.0f);
            for (Unit* u : units)
            {
                if (u == enemy || !u->IsCreature() || u->IsInCombat() || u->IsCritter() || !bot->IsHostileTo(u))
                    continue;
                ++f.near30;
                if (enemy->GetDistance(u) <= 15.0f)
                    ++f.near15;
            }
        }

        Fight& Start(Player* bot, Unit* enemy)
        {
            auto [it, fresh] = s_fights.try_emplace(bot->GetGUID().GetRawValue());
            Fight& f = it->second;
            if (!fresh)
                return f;
            f.startMs = getMSTime();
            f.hp0 = bot->GetHealthPct();
            f.minHp = f.hp0;
            f.mp0 = ManaPct(bot);
            if (bot->getStandState() == UNIT_STAND_STATE_SIT || bot->IsSitState())
                f.state = "rest";
            else if (bot->isMoving())
                f.state = "walk";
            if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
                f.rpg = int32(ai->rpgInfo.GetStatus());
            f.zone = bot->GetZoneId();
            f.area = bot->GetAreaId();
            f.x = bot->GetPositionX();
            f.y = bot->GetPositionY();
            if (enemy && enemy->IsCreature())
            {
                f.firstEntry = enemy->GetEntry();
                CountNear(bot, enemy, f);
            }
            return f;
        }

        Fight* Find(Player* bot)
        {
            auto it = s_fights.find(bot->GetGUID().GetRawValue());
            return it == s_fights.end() ? nullptr : &it->second;
        }

        void UpdateFoes(Player* bot, Fight& f)
        {
            uint32 const now = getMSTime();
            uint32 n = 0;
            for (auto const& [guid, ref] : bot->GetCombatManager().GetPvECombatRefs())
            {
                Unit* other = ref->GetOther(bot);
                if (!other)
                    continue;
                ++n;
                if (f.foes.count(other->GetGUID()))
                    continue;
                Foe foe;
                foe.level = other->GetLevel();
                foe.entry = other->GetEntry();
                if (Creature* c = other->ToCreature())
                    foe.rank = c->GetCreatureTemplate()->rank;
                foe.joinMs = getMSTimeDiff(f.startMs, now);
                f.foes.emplace(other->GetGUID(), foe);
            }
            f.nMax = std::max(f.nMax, n);
        }

        std::string FoeList(Fight const& f)
        {
            std::string out;
            for (auto const& [guid, foe] : f.foes)
            {
                if (!out.empty())
                    out += ',';
                out += Acore::StringFormat("{}:{}:{}:{:.1f}", foe.level, foe.rank, foe.entry, foe.joinMs / 1000.0f);
            }
            return out.empty() ? "-" : out;
        }

        uint32 PullCount(Fight const& f)
        {
            uint32 n = 0;
            for (auto const& [guid, foe] : f.foes)
                if (foe.joinMs <= 2000)
                    ++n;
            return n;
        }

        // the bot knows a defensive of its spec's routine (a step with self_below_pct)
        bool KnowsDefensive(Player* bot)
        {
            EmberfallBots::Spec const* spec = EmberfallBots::SpecOf(bot);
            if (!spec)
                return false;
            for (EmberfallBots::RoutineStep const& step : spec->steps)
                if (step.selfBelowPct && bot->HasSpell(step.spell))
                    return true;
            return false;
        }

        // one character a routine step: 1 the bot knows its ability, 0 not (yet)
        std::string KnownSteps(Player* bot, EmberfallBots::Spec const* spec)
        {
            std::string out;
            if (spec)
                for (EmberfallBots::RoutineStep const& step : spec->steps)
                    out += bot->HasSpell(step.spell) ? '1' : '0';
            return out.empty() ? "-" : out;
        }

        void Emit(Player* bot, Fight const& f, bool died)
        {
            EmberfallBots::Spec const* spec = EmberfallBots::SpecOf(bot);
            uint32 const durMs = getMSTimeDiff(f.startMs, getMSTime());
            uint32 const dealMs = f.firstDealMs ? getMSTimeDiff(f.firstDealMs, f.lastDealMs) : 0;
            std::string const line = Acore::StringFormat(
                "bot={} level={} spec={} hp0={:.0f} mp0={:.0f} state={} rpg={} first={} zone={} area={} x={:.0f} y={:.0f} "
                "near15={} near30={} first_entry={} n_pull={} n_max={} n_all={} n_death={} foes={} dur={:.1f} deal_s={:.1f} "
                "dealt={} taken={} min_hp={:.0f} def={}/{} kills={} escaped={} killer={} death_foes={} steps={}",
                bot->GetName(), bot->GetLevel(), spec ? spec->key : "-", f.hp0, f.mp0, f.state, f.rpg, f.first, f.zone,
                f.area, f.x, f.y, f.near15, f.near30, f.firstEntry, PullCount(f), f.nMax, f.foes.size(), f.nDeath,
                FoeList(f), durMs / 1000.0f, dealMs / 1000.0f, f.dealt, f.taken, f.minHp, f.defensives,
                KnowsDefensive(bot) ? 1 : 0, f.kills, f.escaped ? 1 : 0, f.killer, f.deathFoes.empty() ? "-" : f.deathFoes,
                KnownSteps(bot, spec));
            if (died)
                LOG_INFO("playerbots", "Emberfall death: {}", line);
            else
                LOG_DEBUG("playerbots", "Emberfall fight: {}", line);
        }

        class EmberfallFightsPlayerScript : public PlayerScript
        {
        public:
            EmberfallFightsPlayerScript()
                : PlayerScript("EmberfallFightsPlayerScript",
                               {PLAYERHOOK_ON_PLAYER_ENTER_COMBAT, PLAYERHOOK_ON_PLAYER_LEAVE_COMBAT,
                                PLAYERHOOK_ON_PLAYER_JUST_DIED})
            {
            }

            void OnPlayerEnterCombat(Player* player, Unit* enemy) override
            {
                if (!IsEfBot(player))
                    return;
                std::lock_guard<std::mutex> guard(s_lock);
                UpdateFoes(player, Start(player, enemy));
            }

            void OnPlayerLeaveCombat(Player* player) override
            {
                if (!IsEfBot(player))
                    return;
                std::lock_guard<std::mutex> guard(s_lock);
                Fight* f = Find(player);
                if (!f || f->dying)
                    return;  // a death: its line comes from OnPlayerJustDied
                Emit(player, *f, false);
                // what the fight cost: the rest before the next pull looks at the last two (EmberfallBots::NeedsRest)
                Losses& l = s_losses[player->GetGUID().GetRawValue()];
                l.health[1] = l.health[0];
                l.mana[1] = l.mana[0];
                l.health[0] = std::max(0.0f, f->hp0 - player->GetHealthPct());
                l.mana[0] = std::max(0.0f, f->mp0 - ManaPct(player));
                s_fights.erase(player->GetGUID().GetRawValue());
            }

            void OnPlayerJustDied(Player* player) override
            {
                if (!IsEfBot(player))
                    return;
                std::lock_guard<std::mutex> guard(s_lock);
                Fight* f = Find(player);
                Fight none;
                if (!f)
                {
                    none.startMs = getMSTime();
                    f = &none;
                }
                Emit(player, *f, true);
                s_fights.erase(player->GetGUID().GetRawValue());
                s_deathSpots[player->GetGUID().GetRawValue()] = {player->GetMapId(), player->GetPositionX(),
                                                                 player->GetPositionY(), getMSTime()};
            }
        };

        class EmberfallFightsUnitScript : public UnitScript
        {
        public:
            EmberfallFightsUnitScript() : UnitScript("EmberfallFightsUnitScript", true, {UNITHOOK_ON_DAMAGE}) {}

            void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
            {
                if (!victim || attacker == victim)
                    return;
                if (Player* bot = EfBot(victim))
                {
                    std::lock_guard<std::mutex> guard(s_lock);
                    Fight& f = Start(bot, attacker);
                    if (f.first[0] == '?')
                        f.first = "attacked";
                    uint32 const hp = bot->GetHealth();
                    f.taken += std::min(damage, hp);
                    uint32 const left = damage >= hp ? 0 : hp - damage;
                    f.minHp = std::min(f.minHp, 100.0f * left / std::max<uint32>(1, bot->GetMaxHealth()));
                    UpdateFoes(bot, f);
                    if (damage >= hp && !f.dying)
                    {
                        f.dying = true;
                        for (auto const& [guid, ref] : bot->GetCombatManager().GetPvECombatRefs())
                            if (Unit* other = ref->GetOther(bot))
                            {
                                ++f.nDeath;
                                if (!f.deathFoes.empty())
                                    f.deathFoes += ',';
                                f.deathFoes += Acore::StringFormat("{}:{:.0f}", other->GetLevel(), other->GetHealthPct());
                            }
                        f.killer = attacker ? Acore::StringFormat("{}:{}", attacker->GetEntry(), attacker->GetLevel()) : "env";
                    }
                }
                if (Player* bot = EfBot(attacker))
                {
                    std::lock_guard<std::mutex> guard(s_lock);
                    Fight& f = Start(bot, victim);
                    if (f.first[0] == '?')
                        f.first = "pulled";
                    f.dealt += std::min(damage, victim->GetHealth());
                    // the killing blow (the kill hook comes after the bot has left the fight)
                    if (damage >= victim->GetHealth() && victim->IsAlive())
                        ++f.kills;
                    uint32 const now = getMSTime();
                    if (!f.firstDealMs)
                        f.firstDealMs = now;
                    f.lastDealMs = now;
                }
            }
        };
    }

    void NoteDefensive(Player* bot)
    {
        if (!IsEfBot(bot))
            return;
        std::lock_guard<std::mutex> guard(s_lock);
        if (Fight* f = Find(bot))
            ++f->defensives;
    }

    void NoteEscape(Player* bot)
    {
        if (!IsEfBot(bot))
            return;
        std::lock_guard<std::mutex> guard(s_lock);
        if (Fight* f = Find(bot))
            f->escaped = true;
    }

    bool Escaping(Player* bot)
    {
        std::lock_guard<std::mutex> guard(s_lock);
        Fight* f = Find(bot);
        return f && f->escaped;
    }

    bool NearRecentDeath(Player* bot, Unit* unit, float yards, uint32 minutes)
    {
        std::lock_guard<std::mutex> guard(s_lock);
        auto it = s_deathSpots.find(bot->GetGUID().GetRawValue());
        if (it == s_deathSpots.end())
            return false;
        DeathSpot const& d = it->second;
        if (getMSTimeDiff(d.ms, getMSTime()) > minutes * MINUTE * IN_MILLISECONDS || d.map != unit->GetMapId())
            return false;
        return unit->GetExactDist2d(d.x, d.y) <= yards;
    }

    float LastLossPct(Player* bot, bool mana)
    {
        std::lock_guard<std::mutex> guard(s_lock);
        auto it = s_losses.find(bot->GetGUID().GetRawValue());
        if (it == s_losses.end())
            return 0.0f;
        float const* l = mana ? it->second.mana : it->second.health;
        return std::max(l[0], l[1]);
    }

    bool Losing(Player* bot, float* secondsToKill, float* secondsToDie)
    {
        std::lock_guard<std::mutex> guard(s_lock);
        Fight* f = Find(bot);
        if (!f)
            return false;
        float const elapsed = getMSTimeDiff(f->startMs, getMSTime()) / 1000.0f;
        if (elapsed < 3.0f || !f->taken)
            return false;
        uint64 foeHealth = 0;
        for (auto const& [guid, ref] : bot->GetCombatManager().GetPvECombatRefs())
            if (Unit* other = ref->GetOther(bot))
                foeHealth += other->GetHealth();
        if (!foeHealth)
            return false;
        float const dps = std::max(1.0f, f->dealt / elapsed);
        float const dtps = std::max(1.0f, f->taken / elapsed);
        float const toKill = foeHealth / dps;
        float const toDie = bot->GetHealth() / dtps;
        if (secondsToKill)
            *secondsToKill = toKill;
        if (secondsToDie)
            *secondsToDie = toDie;
        return toDie < toKill;
    }

    void AddScripts()
    {
        new EmberfallFightsPlayerScript();
        new EmberfallFightsUnitScript();
    }
}
