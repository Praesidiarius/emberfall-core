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

// The respawn floor of the enclosed places (Emberfall feedback #593, docs/design/caves.md §13, respawn.md §3.1): the
// realm's dynamic respawn shortens a dying creature's respawn by the players in its whole zone (bots included), so a
// cave's creatures came back after the 45-s minimum while a party was still inside. Emberfall's compiler writes an
// ef_respawn_floor row for every unnamed creature spawn inside a cave, a mine or a cellar; when one dies this puts its
// respawn back to at least its floor after its death (plus its corpse's time). Instanced maps are left alone (the
// dungeons keep their own times). No rows: nothing happens. The rule lives in src/respawnfloor/EfRespawnFloorLogic.h
// (tests/respawnfloor_tests.cpp).

#include "Creature.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "ScriptMgr.h"
#include "UnitScript.h"
#include "WorldScript.h"
#include "respawnfloor/EfRespawnFloorLogic.h"

#include <unordered_map>

namespace
{
    namespace RF = Emberfall::RespawnFloor;

    // spawn id (creature.guid) -> floor in seconds. Written once at startup (before any map updates), only read after.
    std::unordered_map<uint32, uint32> s_floors;

    void LoadFloors()
    {
        s_floors.clear();
        if (QueryResult r = WorldDatabase.Query("SELECT `guid`, `floor_secs` FROM `ef_respawn_floor`"))
        {
            do
            {
                Field* f = r->Fetch();
                s_floors[f[0].Get<uint32>()] = f[1].Get<uint32>();
            } while (r->NextRow());
        }
        LOG_INFO("server.loading", ">> Emberfall respawn floor: {} spawn(s) in enclosed places", s_floors.size());
    }

    class emberfall_respawn_floor_world : public WorldScript
    {
    public:
        emberfall_respawn_floor_world()
            : WorldScript("emberfall_respawn_floor_world", { WORLDHOOK_ON_LOAD_CUSTOM_DATABASE_TABLE }) { }

        void OnLoadCustomDatabaseTable() override { LoadFloors(); }
    };

    class emberfall_respawn_floor_unit : public UnitScript
    {
    public:
        emberfall_respawn_floor_unit() : UnitScript("emberfall_respawn_floor_unit", true, { UNITHOOK_ON_UNIT_DEATH }) { }

        // Unit::Kill calls this after setDeathState(JustDied), which set the creature's respawn time (the dynamic delay
        // plus the corpse's) and, for an elite, saved it.
        void OnUnitDeath(Unit* unit, Unit* /*killer*/) override
        {
            if (s_floors.empty() || !unit->IsCreature())
                return;
            Creature* creature = unit->ToCreature();
            if (!creature->GetSpawnId() || creature->IsAlive() || creature->GetMap()->Instanceable())
                return;
            auto it = s_floors.find(creature->GetSpawnId());
            if (it == s_floors.end())
                return;
            int64_t const now = int64_t(GameTime::GetGameTime().count());
            uint32 const delay = RF::DelayFor(now, int64_t(creature->GetRespawnTime()), it->second, creature->GetCorpseDelay());
            if (!delay)
                return;
            creature->SetRespawnTime(delay);
            // the core saved an elite's (and a world boss's) time at its death: save the floored one over it
            if (creature->isWorldBoss() || creature->GetCreatureTemplate()->rank >= CREATURE_ELITE_ELITE)
                creature->SaveRespawnTime();
        }
    };
}

void AddEmberfallRespawnFloorScripts()
{
    new emberfall_respawn_floor_world();
    new emberfall_respawn_floor_unit();
}
