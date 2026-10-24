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

// Emberfall unstuck (Emberfall docs/design/unstuck.md). Two ways out for a player who is stuck in the world:
//
// Nearby (`.stuck nearby`, the game menu's "Unstuck (nearby)", first choice): the nearest good standing spot within
// 40 yd. Refused while dead (ghosts take the town unstuck: their graveyard), in combat, on a flight path, a transport or
// a vehicle, in a battleground or arena, and within its own cooldown (Emberfall.Unstuck.NearbyCooldown, 300 s). The
// search (EfUnstuckNearby.h) probes rings of rising radius around the player: each point snaps to the map's navmesh
// (ground polygons only: no water, magma, slime), and the spot is good when there is solid floor under it (map/vmap
// height), nothing above it to a player's height and at most one of four short chest-high rays blocked (vmap and game
// objects), it is not under water, at least 150 yd² of navmesh connect to it within 20 yd (no closed pocket: inside a
// rock, a sealed room) and at most two hostile creatures stand near it. The best by ground distance, climb and hostiles
// wins; NearTeleportTo takes the player there. Nothing good (or no navmesh on the map): the town unstuck takes over.
//
// To town (`.unstuck`, `.stuck`, the menu's "To the nearest town"): the stock `.unstuck` (cs_misc) casts SPELL_STUCK
// 7355 for a player account; Emberfall's Spell.dbc makes 7355 instant (2026-10-20; content/data/spells/engine.yaml).
// This module's spell script on 7355:
//   - CheckCast refuses it in combat, on a flight path and within the cooldown (Emberfall.Unstuck.Cooldown, 300 s),
//     with an "Unstuck: ..." system message;
//   - on its Stuck effect a living player goes to the nearest ef_unstuck_point of their map (Emberfall's compiler
//     writes the rows from world.yaml teleports with `town:`); in an instance to its start point (the door's
//     areatrigger_teleport target); with no town on the map, home (the hearth's bind). The dead keep the stock effect:
//     released to their graveyard.
//
// Messages are in the player's language (the session's locale: deDE German, else English; "Unstuck: ", "Befreien: ").
// After every request, 5 s after login and on `.stuck status`, the player's client gets an EFU addon whisper in their own name: the
// seconds until each way can be used again ("<nearby> <town>", -1 off), for the menu's cooldowns.
// Each use is logged (logger "module"): the character, from where, to where; a failed nearby search with its reasons.
//
// Config: Emberfall.Unstuck.Enable (1), Emberfall.Unstuck.Cooldown (300, seconds; 0: none), Emberfall.Unstuck.Nearby
// (1), Emberfall.Unstuck.NearbyCooldown (300, seconds; 0: none).

#include "CellImpl.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "Map.h"
#include "MapDefines.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "WorldPacket.h"
#include "WorldScript.h"
#include "WorldSession.h"

#include "unstuck/EfUnstuckLogic.h"
#include "unstuck/EfUnstuckNearby.h"

#include <array>
#include <cmath>
#include <list>
#include <unordered_map>
#include <vector>

using namespace Acore::ChatCommands;
using namespace Emberfall::Unstuck;

namespace
{
    constexpr uint32 kSpellStuck = 7355;
    constexpr char const* kStatePrefix = "EFU\t";

    bool s_enable = true;
    int64 s_cooldown = 300;
    bool s_nearby = true;
    int64 s_nearbyCooldown = 300;
    std::vector<Point> s_points;
    // The last use per character (seconds, game time); kept in memory: a restart forgets it.
    std::unordered_map<ObjectGuid::LowType, int64> s_lastUse;
    std::unordered_map<ObjectGuid::LowType, int64> s_lastNearby;

    // The nearby search's shape and rules (EfUnstuckNearby.h) and the probe's sizes (yd).
    Nearby::Search const kSearch;
    Nearby::Rules const kRules;
    constexpr float kHeadroom = 2.4f;        // a player's height and a little
    constexpr float kChest = 1.0f;           // the side rays' height above the floor
    constexpr float kSideRay = 0.8f;         // and length
    constexpr float kAreaRadius = 20.0f;     // the connected navmesh counted for "open ground"
    constexpr int kAreaPolys = 64;           // that many polygons or more: open ground whatever their size
    constexpr float kHostileRadius = 12.0f;  // hostiles this near a spot count against it

    void LoadPoints()
    {
        s_points.clear();
        QueryResult result = WorldDatabase.Query("SELECT `id`, `map`, `x`, `y`, `z`, `o`, `name` FROM `ef_unstuck_point`");
        if (result)
        {
            do
            {
                Field* f = result->Fetch();
                Point p;
                p.id = f[0].Get<uint32>();
                p.map = f[1].Get<uint32>();
                p.x = f[2].Get<float>();
                p.y = f[3].Get<float>();
                p.z = f[4].Get<float>();
                p.o = f[5].Get<float>();
                p.name = f[6].Get<std::string>();
                s_points.push_back(std::move(p));
            } while (result->NextRow());
        }
        LOG_INFO("server.loading", ">> Emberfall unstuck: {} ({} town points, cooldown {} s; nearby {}, cooldown {} s)",
                 s_enable ? "on" : "off", s_points.size(), s_cooldown, s_enable && s_nearby ? "on" : "off", s_nearbyCooldown);
    }

    class emberfall_unstuck_world : public WorldScript
    {
    public:
        emberfall_unstuck_world()
            : WorldScript("emberfall_unstuck_world", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_LOAD_CUSTOM_DATABASE_TABLE }) { }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            s_enable = sConfigMgr->GetOption<bool>("Emberfall.Unstuck.Enable", true, false);
            s_cooldown = sConfigMgr->GetOption<int32>("Emberfall.Unstuck.Cooldown", 300, false);
            s_nearby = sConfigMgr->GetOption<bool>("Emberfall.Unstuck.Nearby", true, false);
            s_nearbyCooldown = sConfigMgr->GetOption<int32>("Emberfall.Unstuck.NearbyCooldown", 300, false);
        }

        void OnLoadCustomDatabaseTable() override { LoadPoints(); }
    };

    int64 Now() { return int64(GameTime::GetGameTime().count()); }

    int64 LastOf(std::unordered_map<ObjectGuid::LowType, int64> const& uses, Player* player)
    {
        auto it = uses.find(player->GetGUID().GetCounter());
        return it == uses.end() ? 0 : it->second;
    }

    Lang LangOf(Player* player)
    {
        return player->GetSession() && player->GetSession()->GetSessionDbLocaleIndex() == LOCALE_deDE ? Lang::De : Lang::En;
    }

    void Say(Player* player, std::string const& text)
    {
        ChatHandler(player->GetSession()).SendSysMessage(text);
    }

    // The EFU line: the seconds until each way can be used again (-1: off).
    void SendState(Player* player)
    {
        int64 const now = Now();
        int64 const nearLeft = s_enable && s_nearby ? Remaining(now, LastOf(s_lastNearby, player), s_nearbyCooldown) : -1;
        int64 const townLeft = s_enable ? Remaining(now, LastOf(s_lastUse, player), s_cooldown) : -1;
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player,
                                     std::string(kStatePrefix) + Nearby::StateLine(nearLeft, townLeft));
        player->SendDirectMessage(&data);
    }

    // ---- to town ---------------------------------------------------------------------------------------------------

    Ask AskFor(Player* player)
    {
        Ask a;
        a.enabled = s_enable;
        a.alive = player->IsAlive();
        a.inCombat = player->IsInCombat();
        a.inFlight = player->IsInFlight();
        a.now = Now();
        a.lastUse = LastOf(s_lastUse, player);
        a.cooldown = s_cooldown;
        return a;
    }

    // Refused: the message in the player's chat; true if refused.
    bool Refuse(Player* player)
    {
        Ask a = AskFor(player);
        Refusal r = Check(a);
        if (r == Refusal::None)
            return false;
        Say(player, Message(r, Remaining(a.now, a.lastUse, a.cooldown), LangOf(player)));
        SendState(player);
        return true;
    }

    void Send(Player* player)
    {
        Map* map = player->GetMap();
        uint32 mapId = player->GetMapId();
        bool instance = map && map->Instanceable();
        AreaTriggerTeleport const* door = instance ? sObjectMgr->GetMapEntranceTrigger(mapId) : nullptr;
        Target t = Choose(s_points, mapId, instance, door != nullptr, player->GetPositionX(), player->GetPositionY(),
                          player->GetPositionZ());

        uint32 toMap = 0;
        float x = 0, y = 0, z = 0, o = player->GetOrientation();
        std::string to;
        switch (t.where)
        {
            case Where::Entrance:
                toMap = door->target_mapId, x = door->target_X, y = door->target_Y, z = door->target_Z, o = door->target_Orientation;
                to = "the entrance";
                break;
            case Where::Town:
            {
                Point const& p = s_points[*t.town];
                toMap = p.map, x = p.x, y = p.y, z = p.z, o = p.o;
                to = p.name;
                break;
            }
            default:
                toMap = player->m_homebindMapId, x = player->m_homebindX, y = player->m_homebindY, z = player->m_homebindZ;
                to = "home";
                break;
        }
        LOG_INFO("module", "Emberfall unstuck: {} (guid {}) from map {} ({:.1f}, {:.1f}, {:.1f}) to {} map {} ({:.1f}, {:.1f}, {:.1f})",
                 player->GetName(), player->GetGUID().GetCounter(), mapId, player->GetPositionX(), player->GetPositionY(),
                 player->GetPositionZ(), to, toMap, x, y, z);
        s_lastUse[player->GetGUID().GetCounter()] = Now();
        Say(player, TakingYouTo(t.where, to, LangOf(player)));
        SendState(player);
        player->TeleportTo(toMap, x, y, z, o);
    }

    // ---- nearby ------------------------------------------------------------------------------------------------------

    Nearby::Ask AskNearby(Player* player)
    {
        Nearby::Ask a;
        a.enabled = s_enable && s_nearby;
        a.alive = player->IsAlive();
        a.inCombat = player->IsInCombat();
        a.inFlight = player->IsInFlight();
        a.onTransport = player->GetTransport() != nullptr;
        a.onVehicle = player->GetVehicle() != nullptr;
        a.inBattleground = player->GetMap() && player->GetMap()->IsBattlegroundOrArena();
        a.now = Now();
        a.lastUse = LastOf(s_lastNearby, player);
        a.cooldown = s_nearbyCooldown;
        return a;
    }

    // Living hostile creatures about (no critters): their positions, for the camp check.
    struct HostileCheck
    {
        Player const* player;
        float range;
        bool operator()(Unit* u) const
        {
            return u->IsCreature() && u->IsAlive() && !u->IsCritter() && u->IsHostileTo(player) &&
                   player->IsWithinDist2d(u, range);
        }
    };

    std::vector<Position> HostilesAround(Player* player, float range)
    {
        std::list<Unit*> units;
        HostileCheck check{ player, range };
        Acore::UnitListSearcher<HostileCheck> searcher(player, units, check);
        Cell::VisitObjects(player, searcher, range);
        std::vector<Position> out;
        for (Unit* u : units)
            out.push_back(u->GetPosition());
        return out;
    }

    // The square yards of navmesh connected to `ref` within kAreaRadius of `at` (recast coordinates).
    float OpenArea(dtNavMesh const* mesh, dtNavMeshQuery const* query, dtQueryFilter const& filter, dtPolyRef ref, float const* at)
    {
        std::array<dtPolyRef, kAreaPolys> refs{};
        int count = 0;
        if (dtStatusFailed(query->findPolysAroundCircle(ref, at, kAreaRadius, &filter, refs.data(), nullptr, nullptr, &count, kAreaPolys)))
            return 0;
        if (count >= kAreaPolys)
            return 1e6f;
        float area = 0;
        std::vector<float> xy;
        for (int i = 0; i < count; ++i)
        {
            dtMeshTile const* tile = nullptr;
            dtPoly const* poly = nullptr;
            if (dtStatusFailed(mesh->getTileAndPolyByRef(refs[i], &tile, &poly)) || poly->getType() != DT_POLYTYPE_GROUND)
                continue;
            xy.clear();
            for (unsigned v = 0; v < poly->vertCount; ++v)
            {
                float const* p = &tile->verts[poly->verts[v] * 3];
                xy.push_back(p[2]);   // recast (y, z, x)
                xy.push_back(p[0]);
            }
            area += Nearby::PolygonArea(xy);
        }
        return area;
    }

    // One probe point (cx, cy) around the player at (px, py, pz).
    Nearby::Probe ProbeAt(Player* player, dtNavMesh const* mesh, dtNavMeshQuery const* query, dtQueryFilter const& filter,
                          std::vector<Position> const& hostiles, float cx, float cy, float pz, float radius)
    {
        Map* map = player->GetMap();
        uint32 const phase = player->GetPhaseMask();
        Nearby::Probe p;
        p.radius = radius;

        // The polygon near the point at the player's height; else near the top floor there (a player under the map).
        float const extents[3] = { 1.5f, 8.0f, 1.5f };
        dtPolyRef ref = 0;
        float at[3] = { 0, 0, 0 };
        float center[3] = { cy, pz, cx };
        if (dtStatusFailed(query->findNearestPoly(center, extents, &filter, &ref, at)) || !ref)
        {
            float const top = map->GetHeight(phase, cx, cy, pz + 50.0f, true, 100.0f);
            if (top <= INVALID_HEIGHT)
                return p;
            center[1] = top;
            ref = 0;
            if (dtStatusFailed(query->findNearestPoly(center, extents, &filter, &ref, at)) || !ref)
                return p;
        }
        p.onMesh = true;
        p.x = at[2];
        p.y = at[0];
        float const navZ = at[1];

        float const floorZ = map->GetHeight(phase, p.x, p.y, navZ + 1.0f, true, 4.0f);
        p.floor = floorZ > INVALID_HEIGHT;
        p.floorGap = p.floor ? std::fabs(floorZ - navZ) : 0.0f;
        p.z = p.floor ? floorZ : navZ;

        auto clear = [&](float x2, float y2, float z1, float z2) {
            return map->isInLineOfSight(p.x, p.y, z1, x2, y2, z2, phase, LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing);
        };
        p.headroom = clear(p.x, p.y, p.z + 0.3f, p.z + kHeadroom);
        for (int i = 0; i < 4; ++i)
        {
            float const a = 1.57079632679f * float(i);
            if (!clear(p.x + std::cos(a) * kSideRay, p.y + std::sin(a) * kSideRay, p.z + kChest, p.z + kChest))
                ++p.blockedSides;
        }
        p.underWater = map->IsUnderWater(phase, p.x, p.y, p.z, player->GetCollisionHeight());
        p.openArea = OpenArea(mesh, query, filter, ref, at);
        for (Position const& h : hostiles)
            if (Nearby::Distance2D(h.GetPositionX(), h.GetPositionY(), p.x, p.y) <= kHostileRadius &&
                std::fabs(h.GetPositionZ() - p.z) <= 8.0f)
                ++p.hostiles;
        return p;
    }

    struct Found
    {
        std::optional<Nearby::Probe> spot;
        std::size_t probed = 0;
        std::array<int, 10> rejects{};   // per Nearby::Reject
        bool noMesh = false;             // the map has no navmesh
    };

    Found FindSpot(Player* player)
    {
        Found f;
        Map* map = player->GetMap();
        MMapData& mmap = map->GetMapCollisionData().GetMMapData();
        dtNavMesh const* mesh = mmap.GetNavMesh();
        dtNavMeshQuery const* query = mmap.GetNavMeshQuery();
        if (!mesh || !query)
        {
            f.noMesh = true;
            return f;
        }
        dtQueryFilter filter;
        filter.setIncludeFlags(NAV_GROUND);
        filter.setExcludeFlags(NAV_WATER | NAV_MAGMA | NAV_SLIME);

        float const px = player->GetPositionX(), py = player->GetPositionY(), pz = player->GetPositionZ();
        std::vector<Position> const hostiles = HostilesAround(player, kSearch.radii.back() + kHostileRadius);
        std::vector<Nearby::Probe> probes;
        std::optional<float> firstGood;
        for (Nearby::Offset const& o : Nearby::Rings(kSearch))
        {
            if (Nearby::Stop(kSearch, firstGood, o.radius))
                break;
            Nearby::Probe p = ProbeAt(player, mesh, query, filter, hostiles, px + o.dx, py + o.dy, pz, o.radius);
            Nearby::Reject const why = Nearby::Judge(p, px, py, kRules);
            ++f.rejects[std::size_t(why)];
            if (why == Nearby::Reject::None && !firstGood)
                firstGood = o.radius;
            probes.push_back(p);
        }
        f.probed = probes.size();
        if (auto best = Nearby::Pick(probes, px, py, pz, kRules))
            f.spot = probes[*best];
        return f;
    }

    // `.stuck nearby`: the rules, the search, the teleport; nothing found: to town.
    void FreeNearby(Player* player)
    {
        Lang const lang = LangOf(player);
        Nearby::Ask a = AskNearby(player);
        Nearby::Refusal r = Nearby::Check(a);
        if (r != Nearby::Refusal::None)
        {
            Say(player, Nearby::Message(r, Remaining(a.now, a.lastUse, a.cooldown), lang));
            SendState(player);
            return;
        }

        float const px = player->GetPositionX(), py = player->GetPositionY(), pz = player->GetPositionZ();
        Found f = FindSpot(player);
        if (!f.spot)
        {
            LOG_INFO("module", "Emberfall unstuck: {} (guid {}) nearby on map {} ({:.1f}, {:.1f}, {:.1f}): no spot ({}; {} probed: "
                     "no mesh {}, too close {}, too far {}, no floor {}, headroom {}, wedged {}, under water {}, pocket {}, camp {}); to town",
                     player->GetName(), player->GetGUID().GetCounter(), player->GetMapId(), px, py, pz,
                     f.noMesh ? "the map has no navmesh" : "nothing good", f.probed, f.rejects[1], f.rejects[2], f.rejects[3],
                     f.rejects[4], f.rejects[5], f.rejects[6], f.rejects[7], f.rejects[8], f.rejects[9]);
            Say(player, Nearby::NothingNear(kSearch.radii.back(), lang));
            if (!Refuse(player))
                Send(player);
            return;
        }

        Nearby::Probe const& s = *f.spot;
        float const moved = Nearby::Distance2D(s.x, s.y, px, py);
        float const o = std::atan2(s.y - py, s.x - px);   // facing on, away from where they were stuck
        LOG_INFO("module", "Emberfall unstuck: {} (guid {}) nearby on map {} from ({:.1f}, {:.1f}, {:.1f}) to ({:.1f}, {:.1f}, {:.1f}), "
                 "{:.1f} yd (ring {:.0f}, {} probed, open {:.0f} yd2, {} hostiles)",
                 player->GetName(), player->GetGUID().GetCounter(), player->GetMapId(), px, py, pz, s.x, s.y, s.z, moved,
                 s.radius, f.probed, std::min(s.openArea, 9999.0f), s.hostiles);
        s_lastNearby[player->GetGUID().GetCounter()] = Now();
        Say(player, Nearby::Freed(moved, lang));
        SendState(player);
        player->NearTeleportTo(s.x, s.y, s.z + 0.1f, o);
    }
}

// SPELL_STUCK 7355 (spell_script_names from Emberfall's compiler): the rules and the nearest town.
class spell_ef_unstuck : public SpellScript
{
    PrepareSpellScript(spell_ef_unstuck);

    SpellCastResult CheckCast()
    {
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (player && Refuse(player))
            return SPELL_FAILED_DONT_REPORT;
        return SPELL_CAST_OK;
    }

    void HandleStuck(SpellEffIndex effIndex)
    {
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!player)
            return;
        if (!player->IsAlive())
        {
            // The stock effect: released to the graveyard.
            LOG_INFO("module", "Emberfall unstuck: {} (guid {}) dead on map {} ({:.1f}, {:.1f}, {:.1f}): to the graveyard",
                     player->GetName(), player->GetGUID().GetCounter(), player->GetMapId(), player->GetPositionX(),
                     player->GetPositionY(), player->GetPositionZ());
            return;
        }
        PreventHitDefaultEffect(effIndex);
        if (Refuse(player))  // combat started during the cast
            return;
        Send(player);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_ef_unstuck::CheckCast);
        OnEffectHit += SpellEffectFn(spell_ef_unstuck::HandleStuck, EFFECT_0, SPELL_EFFECT_STUCK);
    }
};

// Shortly after login (the UI is up by then), the EFU line: a relogged player's cooldowns, and the client learns the
// realm has both ways.
class emberfall_unstuck_player : public PlayerScript
{
public:
    emberfall_unstuck_player() : PlayerScript("emberfall_unstuck_player", { PLAYERHOOK_ON_LOGIN }) { }

    void OnPlayerLogin(Player* player) override
    {
        player->m_Events.AddEventAtOffset([player]() { SendState(player); }, 5s);
    }
};

// `.stuck`: the same as the stock `.unstuck` for a player (`.unstuck` is the core's, cs_misc); `.stuck nearby`: free
// nearby; `.stuck status`: only the EFU line (the client's menu asks it when it opens).
class emberfall_unstuck_commands : public CommandScript
{
public:
    emberfall_unstuck_commands() : CommandScript("emberfall_unstuck_commands") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable stuckTable =
        {
            { "nearby", HandleNearby, SEC_PLAYER, Console::No },
            { "status", HandleStatus, SEC_PLAYER, Console::No },
            { "",       HandleStuck,  SEC_PLAYER, Console::No },
        };
        static ChatCommandTable commandTable =
        {
            { "stuck", stuckTable },
        };
        return commandTable;
    }

    static Player* PlayerOf(ChatHandler* handler)
    {
        return handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
    }

    static bool HandleStuck(ChatHandler* handler)
    {
        Player* player = PlayerOf(handler);
        if (!player)
            return false;
        player->CastSpell(player, kSpellStuck, false);
        return true;
    }

    static bool HandleNearby(ChatHandler* handler)
    {
        Player* player = PlayerOf(handler);
        if (!player)
            return false;
        FreeNearby(player);
        return true;
    }

    static bool HandleStatus(ChatHandler* handler)
    {
        Player* player = PlayerOf(handler);
        if (!player)
            return false;
        SendState(player);
        return true;
    }
};

void AddEmberfallUnstuckScripts()
{
    new emberfall_unstuck_world();
    new emberfall_unstuck_commands();
    new emberfall_unstuck_player();
    RegisterSpellScript(spell_ef_unstuck);
}
