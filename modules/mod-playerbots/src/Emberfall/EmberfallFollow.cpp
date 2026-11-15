/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version. Emberfall's part of the module (Copyright Emberfall / Tales of Orrin).
 */

/*
 * The bots follow a player (EmberfallFollow.h; the rule itself: EmberfallFollowLogic.h). Released under GNU GPL v2
 * or later, as the module.
 */

#include "EmberfallFollow.h"

#include "EmberfallBots.h"
#include "EmberfallFollowLogic.h"

#include "AccountMgr.h"
#include "Config.h"
#include "DBCStores.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "StringFormat.h"
#include "Timer.h"
#include "Tokenize.h"
#include "TravelMgr.h"
#include "World.h"

#include <algorithm>
#include <map>
#include <random>
#include <set>
#include <tuple>
#include <vector>

namespace EmberfallFollow
{
    namespace L = Logic;

    namespace
    {
        // the settings
        std::vector<std::string> s_accountNames;  // Playerbots.Emberfall.FollowAccounts, in order
        std::vector<uint32> s_accountIds;          // the same order; 0: no such account (yet)
        uint32 s_intervalMinutes = 120;
        uint32 s_sharePct = 90;
        uint32 s_levels = 3;
        float s_sightYards = 100.0f;
        uint32 s_perSecond = 2;
        uint32 s_maxPerZone = 0;
        uint32 s_waitLoginMinutes = 10;            // at most this long for the bots to be logged in before a pass
        uint32 s_retryMinutes = 5;                 // a busy bot (a fight, dead, a journey) is tried again this long
        std::vector<L::Zone> s_zones;

        // the state (world thread only)
        uint32 s_pollMs = 0;
        uint32 s_followedGuid = 0;                 // the followed character now (0: none online)
        std::string s_followedName;
        bool s_pending = false;                    // a pass is due
        uint32 s_pendingSince = 0;                 // getMSTime() when it became due
        uint32 s_lastPassStart = 0;                // getMSTime() of the last pass's start (0: none yet)
        bool s_hadPass = false;
        std::string s_lastSummary = "no pass yet";

        struct Pass
        {
            bool active = false;
            L::Plan plan;
            size_t next = 0;        // the next order
            uint32 carried = 0;     // orders and retries taken up (the pace)
            uint32 done = 0;        // orders taken up (carried out or skipped)
            uint32 elapsedMs = 0;
            uint32 startMs = 0;
            uint8 level = 0;
            std::string name;
            std::map<uint32, uint32> zoneCounts;                  // zone -> online bots there
            std::set<std::tuple<uint32, uint8, uint8>> noPlace;   // (zone, level, race): no place found
            uint32 relevelled = 0, placed = 0, exempt = 0, gone = 0, fallback = 0, noZone = 0, retried = 0, otherLevel = 0;
            std::vector<std::pair<uint32, L::Order>> retries;    // (pass ms when due, order): busy at its turn
            uint32 slowestMs = 0;
        };
        Pass s_pass;

        // a location's zone, by its map and position to the yard (the destination caches never change)
        std::map<std::tuple<uint32, int32, int32>, uint32> s_zoneOf;

        uint32 ZoneOf(WorldLocation const& loc)
        {
            auto key = std::make_tuple(loc.GetMapId(), int32(loc.GetPositionX()), int32(loc.GetPositionY()));
            auto it = s_zoneOf.find(key);
            if (it != s_zoneOf.end())
                return it->second;
            uint32 const zone = sMapMgr->GetZoneId(PHASEMASK_NORMAL, loc);
            s_zoneOf.emplace(key, zone);
            return zone;
        }

        std::string ZoneName(uint32 zone)
        {
            if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(zone))
                return area->area_name[sWorld->GetDefaultDbcLocale()];
            return "?";
        }

        uint8 MaxLevel()
        {
            return uint8(std::min<uint32>(sPlayerbotAIConfig.RandomBotMaxLevel, sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL)));
        }

        uint8 SideOf(Player* bot) { return bot->GetTeamId() == TEAM_ALLIANCE ? L::SIDE_ACCORD : L::SIDE_KINDRED; }

        void ResolveAccounts()
        {
            s_accountIds.resize(s_accountNames.size(), 0);
            for (size_t i = 0; i < s_accountNames.size(); ++i)
                if (!s_accountIds[i])
                    s_accountIds[i] = AccountMgr::GetId(s_accountNames[i]);
        }

        bool AnyUnresolved()
        {
            return std::find(s_accountIds.begin(), s_accountIds.end(), 0u) != s_accountIds.end();
        }

        // a real player (not a bot of any kind) near the position, on the same map (GMs only when visible)
        bool PlayerWithin(uint32 mapId, float x, float y, float z, float yards)
        {
            float const sq = yards * yards;
            for (Player* player : sRandomPlayerbotMgr.GetPlayers())
            {
                if (!player || !player->IsInWorld() || player->GetMapId() != mapId)
                    continue;
                if (player->IsGameMaster() && !player->isGMVisible())
                    continue;
                if (player->GetExactDistSq(x, y, z) < sq)
                    return true;
                if (WorldObject* view = player->GetViewpoint(); view && view != player && view->GetExactDistSq(x, y, z) < sq)
                    return true;
            }
            return false;
        }

        L::Exemptions ExemptionsOf(Player* bot)
        {
            L::Exemptions e;
            e.grouped = bot->GetGroup() != nullptr;
            e.combat = bot->IsInCombat();
            e.dead = !bot->IsAlive();
            Map* map = bot->GetMap();
            e.dungeon = !map || map->Instanceable();
            e.vehicle = bot->GetVehicle() != nullptr;
            e.travelling = bot->IsInFlight() || bot->IsBeingTeleported() || bot->GetTransport() != nullptr ||
                           bot->HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT);
            e.seen = PlayerWithin(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), s_sightYards);
            return e;
        }

        std::vector<Player*> OnlineBots()
        {
            std::vector<Player*> out;
            for (auto const& [guid, bot] : sRandomPlayerbotMgr.GetAllBots())
                if (bot && bot->IsInWorld() && sRandomPlayerbotMgr.IsRandomBot(bot))
                    out.push_back(bot);
            return out;
        }

        // the followed character now: the first listed account with a character in the world
        Player* FindFollowed()
        {
            std::vector<L::Online> online;
            std::vector<Player*> players;
            for (Player* player : sRandomPlayerbotMgr.GetPlayers())
            {
                if (!player || !player->IsInWorld() || !player->GetSession() || !IsRealPlayer(player))
                    continue;
                online.push_back({player->GetSession()->GetAccountId(), player->GetGUID().GetCounter(), player->GetLevel()});
                players.push_back(player);
            }
            int const i = L::PickFollowed(s_accountIds, online);
            return i < 0 ? nullptr : players[i];
        }

        // the quest givers and inns of the bot's level in `zone`, as the first-login placement picks them (a quest
        // giver with Playerbots.ProbTeleToQuestGivers, else an inn or flight master hub), none within sight of a player
        std::vector<WorldLocation> PlacesIn(Player* bot, uint32 zone)
        {
            std::vector<WorldLocation> givers, hubs;
            for (WorldLocation const& loc : sTravelMgr.GetValidQuestGiverLocations(bot))
                if (ZoneOf(loc) == zone)
                    givers.push_back(loc);
            for (WorldLocation const& loc : sTravelMgr.GetTeleportLocations(bot))
                if (ZoneOf(loc) == zone)
                    hubs.push_back(loc);
            bool const giverFirst = urand(0, 99) < uint32(sPlayerbotAIConfig.ProbTeleToQuestGivers * 100);
            std::vector<WorldLocation> out = giverFirst ? givers : hubs;
            std::vector<WorldLocation> const& other = giverFirst ? hubs : givers;
            if (out.empty())
                out = other;
            out.erase(std::remove_if(out.begin(), out.end(), [](WorldLocation const& loc)
                                     {
                                         return PlayerWithin(loc.GetMapId(), loc.GetPositionX(), loc.GetPositionY(),
                                                             loc.GetPositionZ(), s_sightYards);
                                     }),
                      out.end());
            return out;
        }

        // the module's own teleport checks (RandomPlayerbotMgr::RandomTeleport: the bot maps, no water, the ground, a low
        // bot near its start), without its 150 yd look round the bot: the pass has checked the sight already
        bool TeleportTo(Player* bot, std::vector<WorldLocation> locs)
        {
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            if (!botAI)
                return false;
            std::shuffle(locs.begin(), locs.end(), RandomEngine::Instance());
            for (WorldLocation const& loc : locs)
            {
                auto const& maps = sPlayerbotAIConfig.RandomBotMaps;
                if (std::find(maps.begin(), maps.end(), loc.GetMapId()) == maps.end())
                    continue;
                Map* map = sMapMgr->FindMap(loc.GetMapId(), 0);
                if (!map)
                    continue;
                float const x = loc.GetPositionX(), y = loc.GetPositionY();
                float z = loc.GetPositionZ();
                if (map->IsInWater(bot->GetPhaseMask(), x, y, z, bot->GetCollisionHeight()))
                    continue;
                float const ground = map->GetHeight(bot->GetPhaseMask(), x, y, z + 0.5f);
                if (ground <= INVALID_HEIGHT)
                    continue;
                z = 0.05f + ground;
                if (!botAI->StarterLevelDistanceCheck(bot, loc, true))
                    continue;
                bot->GetMotionMaster()->Clear();
                botAI->Reset(true);
                bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
                bot->TeleportTo(loc.GetMapId(), x, y, z, 0);
                bot->SendMovementFlagUpdate();
                return true;
            }
            return false;
        }

        // a new place in a fitting zone for the bot at its (new) level; the zone it went to, 0: none found
        uint32 Place(Player* bot)
        {
            uint8 const level = bot->GetLevel(), side = SideOf(bot), race = bot->getRace();
            std::set<uint32> excluded;
            for (auto const& [zone, lv, rc] : s_pass.noPlace)
                if (lv == level && rc == race)
                    excluded.insert(zone);
            while (uint32 const zone = L::PickZone(s_zones, s_pass.zoneCounts, level, side, s_maxPerZone, excluded))
            {
                uint32 const from = bot->GetZoneId();
                if (TeleportTo(bot, PlacesIn(bot, zone)))
                {
                    if (s_pass.zoneCounts[from])
                        --s_pass.zoneCounts[from];
                    ++s_pass.zoneCounts[zone];
                    return zone;
                }
                excluded.insert(zone);
                s_pass.noPlace.insert({zone, level, race});
            }
            return 0;
        }

        // a level by the module's own path for it (RandomPlayerbotMgr::RandomizeFirst's: PlayerbotFactory::Randomize,
        // in Emberfall mode RandomizeEmberfall: the level, our trainers' spells, the spec's talents, gear by our
        // weights, food, the quest log reset; a lower level also takes the trainers' spells above it and the gear)
        void Relevel(Player* bot, uint8 level)
        {
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            if (!botAI)
                return;
            sRandomPlayerbotMgr.SetValue(bot, "level", level);
            PlayerbotFactory factory(bot, level);
            factory.Randomize(false);
            botAI->Reset(true);
        }

        // whether a fitting zone may still have a place for a bot of `race` and `side` at `level` (none found there yet)
        bool MayPlace(uint8 level, uint8 side, uint8 race)
        {
            std::set<uint32> excluded;
            for (auto const& [zone, lv, rc] : s_pass.noPlace)
                if (lv == level && rc == race)
                    excluded.insert(zone);
            return L::PickZone(s_zones, s_pass.zoneCounts, level, side, s_maxPerZone, excluded) != 0;
        }

        void Carry(L::Order order)
        {
            Player* bot = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(order.guid));
            if (!bot || !bot->IsInWorld() || !sRandomPlayerbotMgr.IsRandomBot(bot))
            {
                ++s_pass.gone;
                return;
            }
            if (L::Exemptions const e = ExemptionsOf(bot); e.Any())
            {
                // busy now: again in a little while, as long as the pass is young enough
                if (s_pass.elapsedMs + L::RETRY_AFTER_MS < s_retryMinutes * MINUTE * IN_MILLISECONDS)
                {
                    s_pass.retries.emplace_back(s_pass.elapsedMs + L::RETRY_AFTER_MS, order);
                    ++s_pass.retried;
                    return;
                }
                ++s_pass.exempt;
                LOG_DEBUG("playerbots", "Emberfall follow: {} left for a later pass (group {} fight {} dead {} seen {} instance {} "
                          "vehicle {} travel {})", bot->GetName(), e.grouped, e.combat, e.dead, e.seen, e.dungeon, e.vehicle,
                          e.travelling);
                return;
            }
            uint8 const oldLevel = bot->GetLevel();
            uint32 const oldZone = bot->GetZoneId();
            if (order.kind == L::Move::Relevel)
            {
                // a window level whose zones have a place for it (the planned one first)
                for (uint8 level : L::LevelsToTry(s_pass.plan.window, order.level))
                    if (MayPlace(level, SideOf(bot), bot->getRace()))
                    {
                        if (level != order.level)
                            ++s_pass.otherLevel;
                        order.level = level;
                        break;
                    }
                Relevel(bot, order.level);
                ++s_pass.relevelled;
            }
            uint32 const zone = Place(bot);
            if (!zone)
            {
                if (order.kind == L::Move::Relevel)
                {
                    // no fitting zone had a place for it: the module's own placement for the level
                    sRandomPlayerbotMgr.RandomTeleportForLevel(bot);
                    ++s_pass.fallback;
                }
                else
                    ++s_pass.noZone;
                LOG_INFO("playerbots", "Emberfall follow: {} level {} -> {}: no place in a zone of its band ({})", bot->GetName(),
                         oldLevel, bot->GetLevel(), order.kind == L::Move::Relevel ? "the module's placement" : "stays");
                return;
            }
            if (order.kind == L::Move::Place)
                ++s_pass.placed;
            LOG_INFO("playerbots", "Emberfall follow: {} level {} -> {}, zone {} -> {} ({})", bot->GetName(), oldLevel,
                     bot->GetLevel(), oldZone, zone, ZoneName(zone));
        }

        std::string Summary()
        {
            L::Plan const& p = s_pass.plan;
            return Acore::StringFormat(
                "pass for {} (level {}, window {}-{}): Accord {} bots, target {}, fitting {}, orders {}, kept outside {}; "
                "Kindred {} bots, target {}, fitting {}, orders {}, kept outside {}; carried out {} of {} in {} s: "
                "re-levelled {} ({} at another window level: no place at the planned one), new place only {}, busy and tried again {}, "
                "left for a later pass {}, logged out {}, module placement {}, no zone {}; slowest bot {} ms",
                s_pass.name, s_pass.level, p.window.lo, p.window.hi, p.accord.bots, p.accord.target, p.accord.fit,
                p.accord.orders, p.accord.reserve, p.kindred.bots, p.kindred.target, p.kindred.fit, p.kindred.orders,
                p.kindred.reserve, s_pass.done, p.orders.size(), s_pass.elapsedMs / 1000, s_pass.relevelled, s_pass.otherLevel,
                s_pass.placed, s_pass.retried, s_pass.exempt, s_pass.gone, s_pass.fallback, s_pass.noZone, s_pass.slowestMs);
        }

        void Start(Player* followed)
        {
            s_pass = Pass();
            s_pass.active = true;
            s_pass.startMs = getMSTime();
            s_pass.level = followed->GetLevel();
            s_pass.name = followed->GetName();
            L::Window const w = L::WindowFor(followed->GetLevel(), uint8(std::min<uint32>(s_levels, 60)), MaxLevel());
            std::vector<L::Bot> bots;
            for (Player* bot : OnlineBots())
            {
                L::Bot b;
                b.guid = bot->GetGUID().GetCounter();
                b.side = SideOf(bot);
                b.level = bot->GetLevel();
                b.zone = bot->GetZoneId();
                // only what lasts keeps a bot out of the plan; a busy one is tried again in the pass
                b.exempt = ExemptionsOf(bot).Lasting();
                bots.push_back(b);
                ++s_pass.zoneCounts[b.zone];
            }
            s_pass.plan = L::MakePlan(bots, s_zones, w, followed->GetLevel(), s_sharePct, MaxLevel());
            s_lastPassStart = s_pass.startMs;
            s_hadPass = true;
            s_pending = false;
            L::Plan const& p = s_pass.plan;
            LOG_INFO("playerbots", "Emberfall follow: pass for {} (level {}, window {}-{}, share {} %): Accord {} bots (target {}, "
                     "fitting {}, misplaced {}, outside {}, exempt {}, kept outside {}), Kindred {} bots (target {}, fitting {}, "
                     "misplaced {}, outside {}, exempt {}, kept outside {}): {} orders at {} a second",
                     s_pass.name, s_pass.level, w.lo, w.hi, s_sharePct, p.accord.bots, p.accord.target, p.accord.fit,
                     p.accord.misplaced, p.accord.outside, p.accord.exempt, p.accord.reserve, p.kindred.bots, p.kindred.target,
                     p.kindred.fit, p.kindred.misplaced, p.kindred.outside, p.kindred.exempt, p.kindred.reserve, p.orders.size(),
                     s_perSecond);
            if (p.orders.empty())
            {
                s_pass.active = false;
                s_lastSummary = Summary();
                LOG_INFO("playerbots", "Emberfall follow: {}", s_lastSummary);
            }
        }

        void Finish(char const* why)
        {
            s_pass.active = false;
            s_lastSummary = Summary();
            LOG_INFO("playerbots", "Emberfall follow: {}{}", why, s_lastSummary);
        }
    }

    bool Enabled() { return EmberfallBots::Enabled() && !s_accountNames.empty(); }

    void Load()
    {
        s_accountNames.clear();
        s_accountIds.clear();
        std::string const list = sConfigMgr->GetOption<std::string>("Playerbots.Emberfall.FollowAccounts", "");
        for (std::string_view part : Acore::Tokenize(list, ',', false))
        {
            std::string name(part);
            name.erase(0, name.find_first_not_of(" \t\""));
            name.erase(name.find_last_not_of(" \t\"") + 1);
            if (!name.empty())
                s_accountNames.push_back(name);
        }
        s_intervalMinutes = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("Playerbots.Emberfall.FollowIntervalMinutes", 120));
        s_sharePct = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("Playerbots.Emberfall.FollowShare", 90));
        s_levels = sConfigMgr->GetOption<uint32>("Playerbots.Emberfall.FollowLevels", 3);
        s_sightYards = sConfigMgr->GetOption<float>("Playerbots.Emberfall.FollowSightYards", 100.0f);
        s_perSecond = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("Playerbots.Emberfall.FollowBotsPerSecond", 2));
        s_maxPerZone = sConfigMgr->GetOption<uint32>("Playerbots.Emberfall.FollowMaxPerZone", 0);
        s_waitLoginMinutes = sConfigMgr->GetOption<uint32>("Playerbots.Emberfall.FollowWaitLoginMinutes", 10);
        s_retryMinutes = sConfigMgr->GetOption<uint32>("Playerbots.Emberfall.FollowRetryMinutes", 5);

        s_zones.clear();
        std::map<uint32, uint8> sides;
        std::string const prefix = "Playerbots.Emberfall.ZoneSide.";
        for (std::string const& key : sConfigMgr->GetKeysByString(prefix))
            if (key.size() > prefix.size())
                sides[uint32(atoi(key.substr(prefix.size()).c_str()))] =
                    L::ParseSide(sConfigMgr->GetOption<std::string>(key, "both", false));
        for (auto const& [zone, band] : sPlayerbotAIConfig.ZoneBrackets)
        {
            L::Zone z;
            z.id = zone;
            z.lo = uint8(std::min<uint32>(band.first, 255));
            z.hi = uint8(std::min<uint32>(band.second, 255));
            auto it = sides.find(zone);
            z.sides = it == sides.end() ? L::SIDE_BOTH : it->second;
            s_zones.push_back(z);
        }
        if (s_accountNames.empty())
        {
            LOG_INFO("server.loading", ">> Emberfall bots: following no player (Playerbots.Emberfall.FollowAccounts is empty)");
            return;
        }
        ResolveAccounts();
        std::string names;
        for (size_t i = 0; i < s_accountNames.size(); ++i)
            names += (i ? ", " : "") + s_accountNames[i] + (s_accountIds[i] ? "" : " (no such account yet)");
        LOG_INFO("server.loading", ">> Emberfall bots: following {} every {} min: {} % of the bots within {} levels, {} zones "
                 "({} with a side), {} bots a second, sight {} yd, at most {} a zone", names, s_intervalMinutes, s_sharePct,
                 s_levels, s_zones.size(), sides.size(), s_perSecond, s_sightYards,
                 s_maxPerZone ? std::to_string(s_maxPerZone) : std::string("any"));
    }

    void Update(uint32 diff)
    {
        if (!Enabled())
            return;
        if (EmberfallBots::Paused())
        {
            if (s_pass.active)
                Finish("stopped by the off switch: ");
            s_pending = false;
            return;
        }

        if (s_pass.active)
        {
            s_pass.elapsedMs += diff;
            // FollowBotsPerSecond: the plan's orders in turn, then the busy bots whose retry is due; no saving up while
            // nothing is due (no burst after a wait), at most that many in one tick
            uint32 const due = L::Due(s_pass.elapsedMs, s_perSecond);
            uint32 steps = 0;
            while (s_pass.carried < due && steps < std::max<uint32>(1, s_perSecond))
            {
                L::Order order;
                if (s_pass.next < s_pass.plan.orders.size())
                {
                    order = s_pass.plan.orders[s_pass.next++];
                    ++s_pass.done;
                }
                else if (!s_pass.retries.empty() && s_pass.retries.front().first <= s_pass.elapsedMs)
                {
                    order = s_pass.retries.front().second;
                    s_pass.retries.erase(s_pass.retries.begin());
                }
                else
                {
                    s_pass.carried = due;
                    break;
                }
                uint32 const t0 = getMSTime();
                Carry(order);
                s_pass.slowestMs = std::max(s_pass.slowestMs, GetMSTimeDiffToNow(t0));
                ++s_pass.carried;
                ++steps;
            }
            if (s_pass.next >= s_pass.plan.orders.size() && s_pass.retries.empty())
                Finish("");
        }

        s_pollMs += diff;
        if (s_pollMs < 1000)
            return;
        s_pollMs = 0;

        Player* followed = FindFollowed();
        if (!followed && AnyUnresolved() && !sRandomPlayerbotMgr.GetPlayers().empty())
        {
            // an account made after the start (a sync query, only while a name is unknown and players are online)
            static uint32 s_resolveMs = 0;
            if (GetMSTimeDiffToNow(s_resolveMs) > 60000 || !s_resolveMs)
            {
                s_resolveMs = getMSTime();
                ResolveAccounts();
                followed = FindFollowed();
            }
        }
        uint32 const guid = followed ? followed->GetGUID().GetCounter() : 0;
        if (guid != s_followedGuid)
        {
            if (s_pass.active && !guid)
                Finish("stopped (the followed character logged out): ");
            s_followedGuid = guid;
            s_followedName = followed ? followed->GetName() : "";
            if (followed)
            {
                // a login (or the first-listed account came online over another): a pass, once the bots are in
                LOG_INFO("playerbots", "Emberfall follow: following {} (level {})", followed->GetName(), followed->GetLevel());
                s_pending = true;
                s_pendingSince = getMSTime();
            }
            else
                s_pending = false;
        }
        if (!followed || s_pass.active)
            return;
        if (!s_pending && s_hadPass && GetMSTimeDiffToNow(s_lastPassStart) >= s_intervalMinutes * MINUTE * IN_MILLISECONDS)
        {
            s_pending = true;
            s_pendingSince = getMSTime();
        }
        if (!s_pending)
            return;
        // the bots still logging in (a fresh start): wait for them, at most FollowWaitLoginMinutes
        bool const loggingIn = sRandomPlayerbotMgr.GetPlayerbotsCount() < sRandomPlayerbotMgr.GetMaxAllowedBotCount();
        if (loggingIn && GetMSTimeDiffToNow(s_pendingSince) < s_waitLoginMinutes * MINUTE * IN_MILLISECONDS)
            return;
        Start(followed);
    }

    std::string RunNow()
    {
        if (!EmberfallBots::Enabled())
            return "Emberfall follow: Emberfall mode is off";
        if (s_accountNames.empty())
            return "Emberfall follow: no account to follow (Playerbots.Emberfall.FollowAccounts)";
        if (EmberfallBots::Paused())
            return "Emberfall follow: the bots are switched off (.playerbots rndbot on first)";
        if (s_pass.active)
            return Acore::StringFormat("Emberfall follow: a pass is running ({} of {} done)", s_pass.done, s_pass.plan.orders.size());
        ResolveAccounts();
        Player* followed = FindFollowed();
        if (!followed)
            return "Emberfall follow: no followed character is online: nothing to do";
        s_followedGuid = followed->GetGUID().GetCounter();
        s_followedName = followed->GetName();
        Start(followed);
        return Acore::StringFormat("Emberfall follow: pass started for {} (level {}): {} orders", s_pass.name, s_pass.level,
                                   s_pass.plan.orders.size());
    }

    std::string Status()
    {
        if (!Enabled())
            return "Emberfall follow: off (no account in Playerbots.Emberfall.FollowAccounts, or Emberfall mode off)";
        std::string names;
        for (size_t i = 0; i < s_accountNames.size(); ++i)
            names += (i ? ", " : "") + s_accountNames[i] + (i < s_accountIds.size() && s_accountIds[i] ? "" : " (unknown)");
        std::string next = "at the next login of a followed account";
        if (s_followedGuid && s_hadPass)
        {
            uint32 const since = GetMSTimeDiffToNow(s_lastPassStart) / 1000;
            uint32 const every = s_intervalMinutes * MINUTE;
            next = since >= every ? "due now" : Acore::StringFormat("in {} min", (every - since) / 60);
        }
        return Acore::StringFormat("Emberfall follow: accounts {}; every {} min, {} % within {} levels, {} a second, sight {} yd, "
                                   "cap a zone {}; following {}; {}; next pass {}; last: {}",
                                   names, s_intervalMinutes, s_sharePct, s_levels, s_perSecond, s_sightYards,
                                   s_maxPerZone ? std::to_string(s_maxPerZone) : std::string("none"),
                                   s_followedGuid ? s_followedName : std::string("nobody online"),
                                   s_pass.active ? Acore::StringFormat("a pass is running ({} of {})", s_pass.done,
                                                                       s_pass.plan.orders.size())
                                                 : std::string("no pass running"),
                                   next, s_lastSummary);
    }
}
