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

// The boss mechanics toolkit, core side (EfBossKit.h; dungeon-barrows.md §5b).

#include "EfBossKit.h"

#include "CellImpl.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "CreatureScript.h"
#include "EfCompanions.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptedCreature.h"
#include "SpellAuras.h"
#include "TemporarySummon.h"

#include <cmath>
#include <list>

namespace Emberfall::Bosses
{
    namespace
    {
        std::vector<SoakDef>& SoakDefs()
        {
            static std::vector<SoakDef> list;
            return list;
        }
    }

    std::vector<Unit*> GroupSideNear(WorldObject* src, float range)
    {
        std::list<Unit*> found;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(src, src->ToUnit(), range);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(src, found, check);
        Cell::VisitObjects(src, searcher, range);
        std::vector<Unit*> out;
        for (Unit* u : found)
            if (u->IsAlive() && u->IsInWorld() && u->GetCharmerOrOwnerPlayerOrPlayerItself() && !u->IsTotem())
                out.push_back(u);
        return out;
    }

    std::vector<Unit*> GroupSideIn(WorldObject* src, float x, float y, float radius)
    {
        std::vector<Unit*> out;
        for (Unit* u : GroupSideNear(src, std::max(radius + 10.0f, src->GetExactDist2d(x, y) + radius + 5.0f)))
            if (std::hypot(u->GetPositionX() - x, u->GetPositionY() - y) <= radius + u->GetCombatReach() * 0.5f)
                out.push_back(u);
        return out;
    }

    void BeginImmune(Creature* boss, uint32 shroud, Position const* to)
    {
        boss->InterruptNonMeleeSpells(false);
        boss->AttackStop();
        boss->SetReactState(REACT_PASSIVE);
        boss->StopMoving();
        boss->GetMotionMaster()->Clear();
        if (to)
            boss->GetMotionMaster()->MovePoint(0, *to);
        else
            boss->GetMotionMaster()->MoveIdle();
        if (shroud)
            boss->CastSpell(boss, shroud, true);
    }

    void EndImmune(Creature* boss, uint32 shroud)
    {
        if (shroud)
            boss->RemoveAurasDueToSpell(shroud);
        boss->SetReactState(REACT_AGGRESSIVE);
        boss->GetMotionMaster()->Clear();
        if (Unit* v = boss->SelectVictim())
        {
            boss->AI()->AttackStart(v);
            boss->GetMotionMaster()->MoveChase(v);
        }
    }

    void ShowEnergy(Creature* boss, uint32 energy)
    {
        if (boss->getPowerType() != POWER_ENERGY)
        {
            boss->setPowerType(POWER_ENERGY);
            boss->SetMaxPower(POWER_ENERGY, 100);
        }
        boss->SetPower(POWER_ENERGY, std::min<uint32>(energy, 100));
    }

    uint32 PulseOutsideArena(Creature* boss, Point2 centre, float radius, uint32 spell)
    {
        uint32 n = 0;
        for (Unit* u : GroupSideNear(boss, 80.0f))
            if (OutsideArena(Point2{ u->GetPositionX(), u->GetPositionY() }, centre, radius, u->GetCombatReach() * 0.5f))
            {
                boss->CastSpell(u, spell, true);
                ++n;
            }
        return n;
    }

    void SetArena(Creature* boss, Point2 centre, float radius) { Companions::SetArena(boss, centre.x, centre.y, radius); }
    void ClearArena(Creature* boss) { Companions::ClearArena(boss); }

    void RegisterSoakDef(SoakDef const& def)
    {
        for (SoakDef& d : SoakDefs())
            if (d.entry == def.entry)
            {
                d = def;
                Companions::RegisterSoak(def.entry, def.radius, def.needed);
                return;
            }
        SoakDefs().push_back(def);
        Companions::RegisterSoak(def.entry, def.radius, def.needed);
    }

    SoakDef const* FindSoakDef(uint32 entry)
    {
        for (SoakDef const& d : SoakDefs())
            if (d.entry == entry)
                return &d;
        return nullptr;
    }

    TempSummon* SummonSoak(Creature* boss, uint32 entry, float x, float y, float z)
    {
        SoakDef const* def = FindSoakDef(entry);
        if (!def)
            return nullptr;
        return boss->SummonCreature(entry, x, y, z, 0.0f, TEMPSUMMON_TIMED_DESPAWN, def->fuseMs + 3000);
    }

    TempSummon* SummonMarker(Creature* boss, uint32 entry, float x, float y, float z, uint32 aura, uint32 lifeMs)
    {
        TempSummon* m = boss->SummonCreature(entry, x, y, z, 0.0f, TEMPSUMMON_TIMED_DESPAWN, lifeMs);
        if (m && aura)
            m->CastSpell(m, aura, true);
        return m;
    }

    TempSummon* SummonGroundCaster(Creature* boss, uint32 entry, float x, float y, float z, uint32 spell, uint32 lifeMs)
    {
        TempSummon* m = boss->SummonCreature(entry, x, y, z, 0.0f, TEMPSUMMON_TIMED_DESPAWN, lifeMs);
        if (m && spell)
            m->CastSpell(x, y, z, spell, false);
        return m;
    }

    void RegisterGroundCaster(uint32 entry) { Companions::RegisterTelegraphCaster(entry); }
    void RegisterHazardAura(uint32 aura, float radius) { Companions::RegisterHazard(aura, radius); }
}

namespace
{
    using namespace Emberfall::Bosses;

    // A soak circle: shows its aura for the fuse, then shares the hit among those in it (or fails) and goes.
    struct npc_ef_soak_marker : public ScriptedAI
    {
        explicit npc_ef_soak_marker(Creature* c) : ScriptedAI(c) { }

        void IsSummonedBy(WorldObject* summoner) override
        {
            _summoner = summoner ? summoner->GetGUID() : ObjectGuid::Empty;
            me->SetReactState(REACT_PASSIVE);
            _def = FindSoakDef(me->GetEntry());
            if (_def)
            {
                _fuse = _def->fuseMs;
                if (_def->aura)
                    me->CastSpell(me, _def->aura, true);
            }
        }

        void AttackStart(Unit* /*who*/) override { }
        void MoveInLineOfSight(Unit* /*who*/) override { }
        void EnterEvadeMode(EvadeReason /*why*/) override { }

        void UpdateAI(uint32 diff) override
        {
            if (!_def || _done)
                return;
            _fuse = _fuse > diff ? _fuse - diff : 0;
            if (_fuse)
                return;
            _done = true;
            std::vector<Unit*> const in = GroupSideIn(me, me->GetPositionX(), me->GetPositionY(), _def->radius);
            SoakOutcome const o = ResolveSoak(uint32(in.size()), _def->needed, _def->total, _def->floor);
            Creature* boss = ObjectAccessor::GetCreature(*me, _summoner);
            if (o.failed)
            {
                if (_def->failSpell)
                    me->CastSpell(me, _def->failSpell, true);
                if (boss && boss->IsAlive() && boss->AI())
                    boss->AI()->SetData(kSoakFailedData, me->GetEntry());
            }
            else
            {
                for (Unit* u : in)
                    me->CastCustomSpell(_def->hitSpell, SPELLVALUE_BASE_POINT0, int32(o.perSoaker), u, true);
                if (boss && boss->IsAlive() && boss->AI())
                    boss->AI()->SetData(kSoakDoneData, uint32(in.size()));
            }
            LOG_INFO("module", "Emberfall boss kit: soak {} {} ({} of {} needed in it, {} each, instance {})", me->GetEntry(),
                     o.failed ? "failed" : "shared", in.size(), _def->needed, o.perSoaker, me->GetInstanceId());
            me->RemoveAllAuras();
            me->DespawnOrUnsummon(500ms);
        }

    private:
        ObjectGuid _summoner;
        SoakDef const* _def = nullptr;
        uint32 _fuse = 0;
        bool _done = false;
    };

    // A marker that only stands there with the aura its boss gave it (a shelter, an arena ring's middle).
    struct npc_ef_boss_marker : public ScriptedAI
    {
        explicit npc_ef_boss_marker(Creature* c) : ScriptedAI(c) { }

        void IsSummonedBy(WorldObject* /*summoner*/) override { me->SetReactState(REACT_PASSIVE); }
        void AttackStart(Unit* /*who*/) override { }
        void MoveInLineOfSight(Unit* /*who*/) override { }
        void EnterEvadeMode(EvadeReason /*why*/) override { }
        void UpdateAI(uint32 /*diff*/) override { }
    };

    template <typename AI>
    class EfBossKitCreatureScript : public CreatureScript
    {
    public:
        explicit EfBossKitCreatureScript(char const* name) : CreatureScript(name) { }
        CreatureAI* GetAI(Creature* creature) const override { return new AI(creature); }
    };
}

void AddEmberfallBossKitScripts()
{
    new EfBossKitCreatureScript<npc_ef_soak_marker>("npc_ef_soak_marker");
    new EfBossKitCreatureScript<npc_ef_boss_marker>("npc_ef_boss_marker");
}
