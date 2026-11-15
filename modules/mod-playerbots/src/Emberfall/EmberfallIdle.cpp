/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version. Emberfall's part of the module (Copyright Emberfall / Tales of Orrin).
 */

/*
 * Emberfall's part of mod-playerbots: the idle trace of EmberfallIdle.h. Released under GNU GPL v2 or later.
 */

#include "EmberfallIdle.h"

#include "Config.h"
#include "EmberfallBots.h"
#include "Log.h"
#include "MoveSpline.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Timer.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace EmberfallIdle
{
    namespace
    {
        bool s_enabled = false;

        constexpr uint32 CAP_MS = 120000;       // a window ends after two minutes at most
        constexpr float AWAY_YARDS = 25.0f;     // ... or when the bot is this far from where the fight ended
        constexpr float WALK_ACT_YARDS = 5.0f;  // a walk counts as an act once it has carried the bot this far
        constexpr uint32 LONG_DELAY_MS = 3500;  // a wait longer than the react delay out of a fight (1-3 s)

        enum Kind
        {
            STAND,
            SIT,
            WALK,
            CAST,
            LOOT,
            KIND_MAX
        };

        struct Window
        {
            uint32 startMs = 0, lastMs = 0;
            uint32 map = 0;
            float x0 = 0.0f, y0 = 0.0f, z0 = 0.0f;
            float lx = 0.0f, ly = 0.0f;  // the position at the last tick
            float wx = 0.0f, wy = 0.0f;  // where the current walk began
            bool walkAct = false;
            int32 rpg0 = -1;
            float hp = 0.0f, mp = 0.0f;
            uint32 quests = 0;
            uint32 kindMs[KIND_MAX] = {};
            Kind cur = STAND;
            uint32 curStartMs = 0;
            std::map<std::string, uint32> segCause;  // the open stand: its time by what held it
            std::string segActs;                      // the actions the brain ran in it, in order
            std::string segLastAct;
            uint32 longestMs = 0;
            std::string longestCause = "-", longestActs = "-";
            uint32 stands3 = 0;                       // stands of 3 s or more
            std::map<std::string, uint32> causes;     // all standing, by what held it
            std::string lastAction = "-";
            uint32 lastActionMs = 0;
            uint32 prevDelay = 0, delaySet = 0;
            std::string delayBy = "-";
            char const* firstAct = nullptr;
            uint32 firstActMs = 0, standFirstMs = 0;
            std::string acts, lastAct;
            uint32 nActs = 0;
        };

        std::mutex s_lock;
        std::unordered_map<uint64, Window> s_windows;

        char const* KindName(Kind k)
        {
            static char const* const names[KIND_MAX] = {"stand", "sit", "walk", "cast", "loot"};
            return names[k];
        }

        char const* RpgName(int32 s)
        {
            static char const* const names[] = {"IDLE", "GRIND", "CAMP", "WANDER", "NPC", "QUEST",
                                                "FLIGHT", "REST", "PVP", "GATHER"};
            return s >= 0 && s < int32(sizeof(names) / sizeof(names[0])) ? names[s] : "?";
        }

        int32 RpgOf(Player* bot)
        {
            PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
            return ai ? int32(ai->rpgInfo.GetStatus()) : -1;
        }

        // the quest log and the quests handed in: a change is a talk to a quest giver
        uint32 QuestMark(Player* bot)
        {
            uint32 n = uint32(bot->getRewardedQuests().size()) * 64;
            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
                if (uint32 q = bot->GetQuestSlotQuestId(slot))
                    n += q % 61 + 1;
            return n;
        }

        float ManaPct(Player* bot)
        {
            return bot->GetMaxPower(POWER_MANA) > 0 ? bot->GetPowerPct(POWER_MANA) : 100.0f;
        }

        std::string Key(std::string s)
        {
            std::replace(s.begin(), s.end(), ' ', '_');
            return s;
        }

        void Act(Window& w, char const* what, uint32 now)
        {
            if (!w.firstAct)
            {
                w.firstAct = what;
                w.firstActMs = getMSTimeDiff(w.startMs, now);
                w.standFirstMs = w.kindMs[STAND] + (w.cur == STAND ? getMSTimeDiff(w.curStartMs, now) : 0);
            }
            if (w.nActs < 12 && w.lastAct != what)
            {
                if (!w.acts.empty())
                    w.acts += ',';
                w.acts += what;
                w.lastAct = what;
                ++w.nActs;
            }
        }

        void CloseSegment(Window& w, uint32 now)
        {
            if (w.cur != STAND)
                return;
            uint32 const ms = getMSTimeDiff(w.curStartMs, now);
            if (ms >= 3000)
                ++w.stands3;
            if (ms > w.longestMs)
            {
                w.longestMs = ms;
                std::string top = "-";
                uint32 topMs = 0;
                for (auto const& [k, v] : w.segCause)
                    if (v > topMs)
                    {
                        top = k;
                        topMs = v;
                    }
                w.longestCause = top;
                w.longestActs = w.segActs.empty() ? "-" : w.segActs;
            }
            w.segCause.clear();
            w.segActs.clear();
            w.segLastAct.clear();
        }

        void Emit(Player* bot, Window& w, char const* end, uint32 now)
        {
            CloseSegment(w, now);
            std::vector<std::pair<uint32, std::string>> sorted;
            for (auto const& [k, v] : w.causes)
                sorted.emplace_back(v, k);
            std::sort(sorted.rbegin(), sorted.rend());
            std::string causes;
            for (size_t i = 0; i < sorted.size() && i < 6; ++i)
            {
                if (!causes.empty())
                    causes += ';';
                causes += Acore::StringFormat("{}:{:.1f}", sorted[i].second, sorted[i].first / 1000.0f);
            }
            EmberfallBots::Spec const* spec = EmberfallBots::SpecOf(bot);
            LOG_INFO("playerbots",
                     "Emberfall idle: bot={} level={} spec={} zone={} end={} dur={:.1f} hp={:.0f} mp={:.0f} rpg0={} rpg1={} "
                     "stand={:.1f} sit={:.1f} walk={:.1f} cast={:.1f} loot={:.1f} stands3={} longest={:.1f} "
                     "longest_cause={} longest_acts={} first={}:{:.1f} stand_first={:.1f} acts={} causes={}",
                     bot->GetName(), bot->GetLevel(), spec ? spec->key : "-", bot->GetZoneId(), end,
                     getMSTimeDiff(w.startMs, now) / 1000.0f, w.hp, w.mp, RpgName(w.rpg0), RpgName(RpgOf(bot)),
                     w.kindMs[STAND] / 1000.0f, w.kindMs[SIT] / 1000.0f, w.kindMs[WALK] / 1000.0f,
                     w.kindMs[CAST] / 1000.0f, w.kindMs[LOOT] / 1000.0f, w.stands3, w.longestMs / 1000.0f,
                     w.longestCause, w.longestActs, w.firstAct ? w.firstAct : "none",
                     (w.firstAct ? w.firstActMs : getMSTimeDiff(w.startMs, now)) / 1000.0f,
                     (w.firstAct ? w.standFirstMs : w.kindMs[STAND]) / 1000.0f, w.acts.empty() ? "-" : w.acts,
                     causes.empty() ? "-" : causes);
        }
    }

    bool Enabled() { return s_enabled; }

    void Load()
    {
        s_enabled = sConfigMgr->GetOption<bool>("Playerbots.Emberfall.IdleTrace", false);
        if (s_enabled)
            LOG_INFO("server.loading", ">> Emberfall bots: the idle trace is on (Playerbots.Emberfall.IdleTrace)");
    }

    void FightEnded(Player* bot)
    {
        if (!s_enabled || !bot || !bot->IsAlive())
            return;
        std::lock_guard<std::mutex> guard(s_lock);
        Window& w = s_windows[bot->GetGUID().GetRawValue()];
        w = Window();
        uint32 const now = getMSTime();
        w.startMs = w.lastMs = w.curStartMs = now;
        w.map = bot->GetMapId();
        w.x0 = w.lx = bot->GetPositionX();
        w.y0 = w.ly = bot->GetPositionY();
        w.z0 = bot->GetPositionZ();
        w.rpg0 = RpgOf(bot);
        w.hp = bot->GetHealthPct();
        w.mp = ManaPct(bot);
        w.quests = QuestMark(bot);
    }

    void NoteAction(Player* bot, std::string const& name)
    {
        if (!s_enabled || !bot)
            return;
        std::lock_guard<std::mutex> guard(s_lock);
        auto it = s_windows.find(bot->GetGUID().GetRawValue());
        if (it == s_windows.end())
            return;
        Window& w = it->second;
        w.lastAction = Key(name);
        w.lastActionMs = getMSTime();
        if (w.cur == STAND && w.lastAction != w.segLastAct && w.segActs.size() < 200)
        {
            if (!w.segActs.empty())
                w.segActs += ',';
            w.segActs += w.lastAction;
            w.segLastAct = w.lastAction;
        }
    }

    void Tick(Player* bot, uint32 aiDelay, bool active)
    {
        if (!s_enabled)
            return;
        std::lock_guard<std::mutex> guard(s_lock);
        auto it = s_windows.find(bot->GetGUID().GetRawValue());
        if (it == s_windows.end())
            return;
        Window& w = it->second;
        uint32 const now = getMSTime();
        uint32 const dt = getMSTimeDiff(w.lastMs, now);
        w.lastMs = now;

        char const* end = nullptr;
        if (!bot->IsAlive())
            end = "died";
        else if (bot->IsInCombat())
        {
            Act(w, "fight", now);
            end = "fight";
        }
        else if (bot->IsInFlight())
        {
            Act(w, "flight", now);
            end = "flight";
        }
        else if (bot->GetMapId() != w.map || bot->IsBeingTeleported())
            end = "teleport";
        else if (bot->GetExactDist(w.x0, w.y0, w.z0) > AWAY_YARDS)
        {
            Act(w, "walk", now);
            end = "away";
        }
        else if (getMSTimeDiff(w.startMs, now) > CAP_MS)
            end = "cap";
        if (end)
        {
            Emit(bot, w, end, now);
            s_windows.erase(it);
            return;
        }

        float const x = bot->GetPositionX(), y = bot->GetPositionY();
        bool const moved = std::abs(x - w.lx) + std::abs(y - w.ly) > 0.05f;
        w.lx = x;
        w.ly = y;
        bool const lootRecent = w.lastAction.find("loot") != std::string::npos &&
                                getMSTimeDiff(w.lastActionMs, now) < 2000;
        Kind k = STAND;
        if (bot->IsNonMeleeSpellCast(false))
            k = CAST;
        else if (bot->IsSitState())
            k = SIT;
        else if (moved || !bot->movespline->Finalized())
            k = WALK;
        else if (lootRecent)
            k = LOOT;

        if (k != w.cur)
        {
            CloseSegment(w, now);
            w.cur = k;
            w.curStartMs = now;
            if (k == SIT)
                Act(w, "sit", now);
            else if (k == CAST)
                Act(w, "cast", now);
            else if (k == LOOT)
                Act(w, "loot", now);
            else if (k == WALK)
            {
                w.wx = x;
                w.wy = y;
                w.walkAct = false;
            }
        }
        if (k == WALK && !w.walkAct && bot->GetExactDist2d(w.wx, w.wy) >= WALK_ACT_YARDS)
        {
            w.walkAct = true;
            Act(w, "walk", now);
        }
        uint32 const q = QuestMark(bot);
        if (q != w.quests)
        {
            w.quests = q;
            Act(w, "talk", now);
        }

        // what holds the bot when it stands: a long delay an action set, the react delay after its last action, or
        // the module's inactivity (PassiveDelay)
        if (aiDelay > w.prevDelay)
        {
            w.delaySet = aiDelay;
            w.delayBy = w.lastAction;
        }
        w.prevDelay = aiDelay;
        w.kindMs[k] += dt;
        if (k == STAND)
        {
            std::string key;
            if (!active)
                key = "inactive";
            else if (aiDelay > 0 && w.delaySet > LONG_DELAY_MS)
                key = "delay:" + w.delayBy;
            else
                key = "wait:" + w.lastAction;
            key += '@';
            key += RpgName(RpgOf(bot));
            w.segCause[key] += dt;
            w.causes[key] += dt;
        }
    }
}
