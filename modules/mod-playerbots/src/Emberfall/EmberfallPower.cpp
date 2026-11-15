/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version. Emberfall's part of the module (Copyright Emberfall / Tales of Orrin).
 */

/*
 * Emberfall's part of mod-playerbots: a bot's spec power, EmberfallPower.h. Released under GNU GPL v2 or later.
 */

#include "EmberfallPower.h"

#include "DBCStores.h"
#include "EmberfallBots.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ScriptMgr.h"
#include "Timer.h"

#include <mutex>
#include <vector>

#if __has_include("EfClassKit.h") && __has_include("EfTalents.h")
#include "EfClassKit.h"
#include "EfTalents.h"
#define EF_BOTS_HAVE_SPEC_POWER 1
#endif

namespace EmberfallPower
{
    namespace
    {
        struct Pending
        {
            ObjectGuid guid;
            uint32 dueMs = 0;
            char const* when = "";
        };
        std::mutex s_lock;
        std::vector<Pending> s_pending;

        bool IsEfBot(Player* player)
        {
            return player && EmberfallBots::Enabled() && GET_PLAYERBOT_AI(player) != nullptr &&
                   EmberfallBots::SpecOf(player) != nullptr;
        }

        void Queue(Player* player, char const* when)
        {
            if (!IsEfBot(player))
                return;
            std::lock_guard<std::mutex> guard(s_lock);
            s_pending.push_back({player->GetGUID(), getMSTime() + 3000, when});
        }

        class EmberfallPowerPlayerScript : public PlayerScript
        {
        public:
            EmberfallPowerPlayerScript()
                : PlayerScript("EmberfallPowerPlayerScript", {PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LEVEL_CHANGED})
            {
            }

            void OnPlayerLogin(Player* player) override { Queue(player, "login"); }
            void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override { Queue(player, "level"); }
        };
    }

    void Apply(Player* bot)
    {
#ifdef EF_BOTS_HAVE_SPEC_POWER
        Emberfall::ClassKit::ApplySpecPower(bot);
#else
        (void)bot;
#endif
    }

    int32 Expected(Player* bot)
    {
#ifdef EF_BOTS_HAVE_SPEC_POWER
        Emberfall::ClassKit::SpecPower const* p =
            Emberfall::ClassKit::SpecPowerOf(Emberfall::Talents::ActiveSpec(bot));
        if (p && p->classId == bot->getClass())
            return int32(p->power);
        ChrClassesEntry const* cls = sChrClassesStore.LookupEntry(bot->getClass());
        return cls && cls->powerType < MAX_POWERS ? int32(cls->powerType) : int32(POWER_MANA);
#else
        (void)bot;
        return -1;
#endif
    }

    bool Check(Player* bot, char const* when)
    {
        if (!IsEfBot(bot))
            return true;
        int32 const want = Expected(bot);
        if (want < 0)
            return true;
        Powers const have = bot->getPowerType();
        EmberfallBots::Spec const* spec = EmberfallBots::SpecOf(bot);
        bool const ok = int32(have) == want;
        if (ok)
            LOG_DEBUG("playerbots", "Emberfall power: {} level {} spec {} after {}: power {} ok ({}/{})", bot->GetName(),
                      bot->GetLevel(), spec->key, when, int32(have), bot->GetPower(have), bot->GetMaxPower(have));
        else
            LOG_ERROR("playerbots", "Emberfall power: {} level {} spec {} after {}: power {}, its spec's is {}",
                      bot->GetName(), bot->GetLevel(), spec->key, when, int32(have), want);
        return ok;
    }

    void AddScripts() { new EmberfallPowerPlayerScript(); }

    void Update(uint32 /*diff*/)
    {
        std::vector<Pending> due;
        {
            std::lock_guard<std::mutex> guard(s_lock);
            if (s_pending.empty())
                return;
            uint32 const now = getMSTime();
            for (auto it = s_pending.begin(); it != s_pending.end();)
            {
                if (int32(now - it->dueMs) >= 0)
                {
                    due.push_back(*it);
                    it = s_pending.erase(it);
                }
                else
                    ++it;
            }
        }
        for (Pending const& p : due)
            if (Player* bot = ObjectAccessor::FindPlayer(p.guid))
                if (bot->IsInWorld())
                    Check(bot, p.when);
    }
}
