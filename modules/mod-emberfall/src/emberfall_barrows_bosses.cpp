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

// The Barrows of Cairnmoor's bosses (Emberfall docs/design/dungeon-barrows.md §5a): five BossAI scripts in the
// AzerothCore style (an EventMap for the timers, the SummonList for the adds) and their adds' AIs. Every ID, timer,
// threshold and place is src/dungeons/EfBarrowsBossLogic.h (tests/dungeon_tests.cpp checks the timers and the phases);
// the spells and texts are Emberfall's content (content/data/spells/barrows.yaml, content/data/barrows/creatures.yaml).
//
//   boss_ef_post_keeper     B1 the gate: Tally Sweep (a frontal), Numbered (a run-out mark), Shoulder the Post
//   boss_ef_keening_mother  B2: Keen (interrupt it), Grave Chill, Mourning Veil (a dispel), mourning shades at 70/35 %
//   boss_ef_burrow_mother   B3: the brood, Brood Bile (a poison), the burrow at 66/33 % (Heaving Earth, Emerge)
//   boss_ef_last_stoker     B4: Pyre Ash (at someone's feet), Stoker's Brand, pyre cinders at 75/50/25 %, Bank the Fires
//   boss_ef_first_interred  B5: Barrow Weight, Prefect's Sentence, Kneel, the Grave Ward at 70/45 %, enrage at 25 %
//   npc_ef_mourning_shade, npc_ef_pyre_cinder, npc_ef_ward_bearer   the adds that must die (kill priority for companions)
//
// The bosses report their deaths to instance_ef_barrows (the seals, emberfall_dungeons.cpp). The companions know the
// mechanics from the spells themselves (telegraphed casts, interruptible casts) and from what the scripts tell them here:
// the kill-priority adds, the run-out mark, the boss mark (src/companions/EfCompanions.h).

#include "CreatureAI.h"
#include "CreatureScript.h"
#include "EfBarrowsBossLogic.h"
#include "EfCompanions.h"
#include "EfDungeonLogic.h"
#include "InstanceScript.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptedCreature.h"
#include "SpellAuras.h"
#include "TemporarySummon.h"

#include <algorithm>
#include <bit>

using namespace Emberfall::Barrows;
namespace Companions = Emberfall::Companions;
namespace Dungeons = Emberfall::Dungeons;

namespace
{
    Milliseconds Ms(std::uint32_t ms) { return Milliseconds(ms); }

    // Schedules `id` at the timer's first time / again within its range.
    void First(EventMap& events, std::uint32_t id, Timer const& t, std::uint32_t group = 0) { events.ScheduleEvent(id, Ms(t.first), group); }
    void Again(EventMap& events, std::uint32_t id, Timer const& t, std::uint32_t group = 0) { events.ScheduleEvent(id, Ms(t.min), Ms(t.max), group); }

    // The ground height near (x, y), or z where the map has none.
    float GroundZ(Creature* me, float x, float y, float z)
    {
        float const g = me->GetMapHeight(x, y, z + 4.0f);
        return g > INVALID_HEIGHT && std::fabs(g - z) < 6.0f ? g : z;
    }

    // `p` pulled towards the room's middle until it is `margin` yards inside its wall.
    Point IntoRoom(Point p, Point centre, float radius, float margin)
    {
        float const dx = p.x - centre.x, dy = p.y - centre.y;
        float const d = std::hypot(dx, dy);
        float const max = std::max(radius - margin, 1.0f);
        if (d <= max)
            return p;
        return Point{ centre.x + dx / d * max, centre.y + dy / d * max, p.z };
    }

    // Someone on the threat list who is not the boss's victim (the tank), within `range`; else nobody.
    Unit* SomeoneElse(BossAI* ai, float range) { return ai->SelectTarget(SelectTargetMethod::Random, 0, range, false, false); }
    Unit* Anyone(BossAI* ai, float range) { return ai->SelectTarget(SelectTargetMethod::Random, 0, range, false, true); }

    float HealthPctAfter(Creature* me, std::uint32_t damage)
    {
        std::uint32_t const hp = me->GetHealth() > damage ? me->GetHealth() - damage : 0;
        return me->GetMaxHealth() ? 100.0f * float(hp) / float(me->GetMaxHealth()) : 0.0f;
    }

    void Report(InstanceScript* instance, std::uint32_t type)
    {
        if (instance)
            instance->SetData(type, Dungeons::kEncounterDone);
    }

    enum Actions : int32 { ACTION_SHADE_DRUNK = 1 };

    // ---- B1 the Post-Keeper ------------------------------------------------------------------------------------------
    // Ideas from: Ingvar the Plunderer's Smash (Utgarde Keep) for the frontal the tank turns away; Ionar's Static Overload
    // (Halls of Lightning) and Urom's Time Bomb (the Oculus) for the mark its bearer carries away from the group.
    struct boss_ef_post_keeper : public BossAI
    {
        enum Events : std::uint32_t { EVENT_SWEEP = 1, EVENT_NUMBERED, EVENT_SHOULDER };

        explicit boss_ef_post_keeper(Creature* c) : BossAI(c, BOSS_POST_KEEPER) { Companions::SetBossMark(me); }

        void Reset() override { _Reset(); }

        void JustEngagedWith(Unit* /*who*/) override
        {
            _JustEngagedWith();
            Talk(Text::KEEPER_AGGRO);
            First(events, EVENT_SWEEP, Timers::kTallySweep);
            First(events, EVENT_NUMBERED, Timers::kNumbered);
            First(events, EVENT_SHOULDER, Timers::kShoulder);
        }

        void JustDied(Unit* /*killer*/) override
        {
            _JustDied();
            Talk(Text::KEEPER_DEATH);
            Report(instance, Dungeons::DATA_POST_KEEPER);
        }

        void ExecuteEvent(std::uint32_t id) override
        {
            switch (id)
            {
                case EVENT_SWEEP:
                    Talk(Text::KEEPER_SWEEP);
                    DoCastVictim(SPELL_TALLY_SWEEP);
                    Again(events, EVENT_SWEEP, Timers::kTallySweep);
                    break;
                case EVENT_NUMBERED:
                    if (Unit* target = SomeoneElse(this, 40.0f))
                    {
                        Talk(Text::KEEPER_NUMBERED);
                        Talk(Text::KEEPER_NUMBERED_WHISPER, target);
                        DoCast(target, SPELL_NUMBERED, true);
                        ObjectGuid const guid = target->GetGUID();
                        // The fuse runs on the creature's own event list: a cast in between does not hold it up.
                        me->m_Events.AddEventAtOffset([this, guid]
                        {
                            if (!me->IsAlive() || !me->IsInCombat())
                                return;
                            if (Unit* t = ObjectAccessor::GetUnit(*me, guid); t && t->IsAlive() && t->IsInMap(me))
                                me->CastSpell(t, SPELL_STRUCK_FROM_THE_COUNT, true);
                        }, Ms(Timers::kNumberedFuse));
                    }
                    Again(events, EVENT_NUMBERED, Timers::kNumbered);
                    break;
                case EVENT_SHOULDER:
                    DoCastVictim(SPELL_SHOULDER_THE_POST);
                    Again(events, EVENT_SHOULDER, Timers::kShoulder);
                    break;
            }
        }
    };

    // ---- B2 the Keening Mother ---------------------------------------------------------------------------------------
    // Ideas from: Grandmaster Vorpil's void travellers (Shadow Labyrinth) and Bronjahm's soul fragments (the Forge of
    // Souls), adds that walk to the boss and feed it unless killed on the way; Salramm the Fleshcrafter's curse (the
    // Culling of Stratholme) for a debuff a healer takes off; the keen is the model's caster boss, interrupted.
    struct boss_ef_keening_mother : public BossAI
    {
        enum Events : std::uint32_t { EVENT_KEEN = 1, EVENT_CHILL, EVENT_VEIL };

        explicit boss_ef_keening_mother(Creature* c) : BossAI(c, BOSS_KEENING_MOTHER) { Companions::SetBossMark(me); }

        void Reset() override
        {
            _Reset();
            _fired = 0;
        }

        void JustEngagedWith(Unit* /*who*/) override
        {
            _JustEngagedWith();
            Talk(Text::KEENING_AGGRO);
            First(events, EVENT_KEEN, Timers::kKeen);
            First(events, EVENT_CHILL, Timers::kGraveChill);
            First(events, EVENT_VEIL, Timers::kMourningVeil);
        }

        void JustDied(Unit* /*killer*/) override
        {
            _JustDied();
            Talk(Text::KEENING_DEATH);
            Report(instance, Dungeons::DATA_SEAL_KEENING);
        }

        void DamageTaken(Unit* attacker, std::uint32_t& damage, DamageEffectType type, SpellSchoolMask school) override
        {
            BossAI::DamageTaken(attacker, damage, type, school);
            for (std::size_t i : CrossedThresholds(Thresholds::kShades, HealthPctAfter(me, damage), _fired))
                Shades(i);
        }

        void DoAction(int32 action) override
        {
            if (action == ACTION_SHADE_DRUNK && me->IsAlive())
            {
                Talk(Text::KEENING_DRINK);
                DoCastSelf(SPELL_DRINK_THE_GRIEF, true);
            }
        }

        void ExecuteEvent(std::uint32_t id) override
        {
            switch (id)
            {
                case EVENT_KEEN:
                    Talk(Text::KEENING_KEEN);
                    DoCastSelf(SPELL_KEEN);
                    Again(events, EVENT_KEEN, Timers::kKeen);
                    break;
                case EVENT_CHILL:
                    DoCastVictim(SPELL_GRAVE_CHILL);
                    Again(events, EVENT_CHILL, Timers::kGraveChill);
                    break;
                case EVENT_VEIL:
                    if (Unit* target = SomeoneElse(this, 40.0f))
                        DoCast(target, SPELL_MOURNING_VEIL, true);
                    Again(events, EVENT_VEIL, Timers::kMourningVeil);
                    break;
            }
        }

    private:
        // Three shades rise round her (inside the barrow's wall) and drift to her.
        void Shades(std::size_t wave)
        {
            Talk(Text::KEENING_SHADES_YELL);
            Talk(Text::KEENING_SHADES);
            Point const here{ me->GetPositionX(), me->GetPositionY(), me->GetPositionZ() };
            float const start = me->GetOrientation() + 0.5f * float(wave);
            std::size_t engaged = 0;
            for (Point p : FarthestOnRing(kWidowsBarrow, kShadeSpawnRadius, 8, kShadesPerWave, here, start))
            {
                if (TempSummon* s = me->SummonCreature(NPC_MOURNING_SHADE, p.x, p.y, GroundZ(me, p.x, p.y, here.z), 0.0f, TEMPSUMMON_CORPSE_TIMED_DESPAWN, 20000))
                    engaged = std::max<std::size_t>(engaged, s->GetThreatMgr().GetThreatListSize());
            }
            LOG_INFO("module", "Emberfall barrows: the Keening Mother calls {} mourning shades (wave {}, instance {}, on the threat lists of {})", kShadesPerWave,
                     wave + 1, me->GetInstanceId(), engaged);
        }

        std::uint32_t _fired = 0;
    };

    // A mourning shade: drifts to the Keening Mother; within reach it is drunk (she heals and keens louder).
    struct npc_ef_mourning_shade : public ScriptedAI
    {
        explicit npc_ef_mourning_shade(Creature* c) : ScriptedAI(c) { }

        void IsSummonedBy(WorldObject* summoner) override
        {
            _mother = summoner ? summoner->GetGUID() : ObjectGuid::Empty;
            me->SetReactState(REACT_PASSIVE);
            me->SetSpeedRate(MOVE_RUN, kShadeSpeedRate);
            Companions::SetKillPriority(me, true);
            me->SetInCombatWithZone();
            _repath = 0;
        }

        void AttackStart(Unit* /*who*/) override { }
        void MoveInLineOfSight(Unit* /*who*/) override { }
        void EnterEvadeMode(EvadeReason /*why*/) override { }

        void JustDied(Unit* /*killer*/) override
        {
            LOG_INFO("module", "Emberfall barrows: a mourning shade killed on its way (instance {})", me->GetInstanceId());
        }

        void UpdateAI(std::uint32_t diff) override
        {
            Creature* mother = ObjectAccessor::GetCreature(*me, _mother);
            if (!mother || !mother->IsAlive() || !mother->IsInCombat())
            {
                me->DespawnOrUnsummon();
                return;
            }
            if (me->GetExactDist2d(mother) <= kShadeReach + mother->GetCombatReach())
            {
                LOG_INFO("module", "Emberfall barrows: a mourning shade reached the Keening Mother (instance {})", me->GetInstanceId());
                if (mother->IsAIEnabled && mother->AI())
                    mother->AI()->DoAction(ACTION_SHADE_DRUNK);
                me->DespawnOrUnsummon();
                return;
            }
            _repath = _repath > diff ? _repath - diff : 0;
            if (!_repath)
            {
                _repath = 1000;
                me->GetMotionMaster()->MovePoint(1, mother->GetPositionX(), mother->GetPositionY(), mother->GetPositionZ());
            }
        }

    private:
        ObjectGuid _mother;
        std::uint32_t _repath = 0;
    };

    // ---- B3 the Burrow-Mother ----------------------------------------------------------------------------------------
    // Ideas from: Anub'arak's submerge phases (Azjol-Nerub), untouchable under the ground while spikes erupt under the
    // group and adds come; Krik'thir's swarms for the brood the tank gathers.
    struct boss_ef_burrow_mother : public BossAI
    {
        enum Events : std::uint32_t { EVENT_BROOD = 1, EVENT_BILE };

        explicit boss_ef_burrow_mother(Creature* c) : BossAI(c, BOSS_BURROW_MOTHER) { Companions::SetBossMark(me); }

        void Reset() override
        {
            _Reset();
            Surface();
            _burrow = BurrowState{};
            _fired = 0;
            _pending = 0;
        }

        void JustEngagedWith(Unit* /*who*/) override
        {
            _JustEngagedWith();
            Talk(Text::BURROW_AGGRO);
            ScheduleAbove();
        }

        void JustDied(Unit* /*killer*/) override
        {
            _JustDied();
            Report(instance, Dungeons::DATA_SEAL_BROOD);
        }

        void DamageTaken(Unit* attacker, std::uint32_t& damage, DamageEffectType type, SpellSchoolMask school) override
        {
            BossAI::DamageTaken(attacker, damage, type, school);
            _pending += std::uint32_t(CrossedThresholds(Thresholds::kBurrow, HealthPctAfter(me, damage), _fired).size());
        }

        void UpdateAI(std::uint32_t diff) override
        {
            if (!UpdateVictim())
                return;
            events.Update(diff);
            bool const casting = me->HasUnitState(UNIT_STATE_CASTING);
            if (_burrow.phase != BurrowPhase::Above)
            {
                switch (StepBurrow(_burrow, diff, casting))
                {
                    case BurrowAction::Heave:
                        if (Unit* target = Anyone(this, 60.0f))
                            me->CastSpell(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(), SPELL_HEAVING_EARTH, false);
                        break;
                    case BurrowAction::Emerge:
                        Talk(Text::BURROW_UP);
                        me->RemoveAurasDueToSpell(SPELL_BURROWED);
                        DoCastSelf(SPELL_EMERGE);
                        break;
                    case BurrowAction::Surface:
                        Surface();
                        if (Unit* v = me->SelectVictim())
                            AttackStart(v);
                        ScheduleAbove();
                        break;
                    default:
                        break;
                }
                return;
            }
            if (casting)
                return;
            if (_pending)
            {
                --_pending;
                Burrow();
                return;
            }
            while (std::uint32_t const id = events.ExecuteEvent())
            {
                ExecuteEvent(id);
                if (me->HasUnitState(UNIT_STATE_CASTING))
                    return;
            }
            DoMeleeAttackIfReady();
        }

        void ExecuteEvent(std::uint32_t id) override
        {
            switch (id)
            {
                case EVENT_BROOD:
                    Talk(Text::BURROW_BROOD);
                    Brood(kGrublingsPerBrood);
                    Again(events, EVENT_BROOD, Timers::kBrood);
                    break;
                case EVENT_BILE:
                    if (Unit* target = Anyone(this, 30.0f))
                        DoCast(target, SPELL_BROOD_BILE, true);
                    Again(events, EVENT_BILE, Timers::kBroodBile);
                    break;
            }
        }

    private:
        void ScheduleAbove()
        {
            events.Reset();
            First(events, EVENT_BROOD, Timers::kBrood);
            First(events, EVENT_BILE, Timers::kBroodBile);
        }

        void Brood(std::uint32_t count)
        {
            Point const here{ me->GetPositionX(), me->GetPositionY(), me->GetPositionZ() };
            for (Point p : Ring(here, 6.0f, count, me->GetOrientation() + 1.0f))
                me->SummonCreature(NPC_GRUBLING, p.x, p.y, GroundZ(me, p.x, p.y, here.z), 0.0f, TEMPSUMMON_CORPSE_TIMED_DESPAWN, 30000);
        }

        // Under the earth: not attackable, not swinging; the heaves and the grublings meanwhile (StepBurrow).
        void Burrow()
        {
            Talk(Text::BURROW_DOWN);
            events.Reset();
            me->AttackStop();
            me->StopMoving();
            me->GetMotionMaster()->Clear();
            me->GetMotionMaster()->MoveIdle();
            me->SetReactState(REACT_PASSIVE);
            me->SetUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE);
            DoCastSelf(SPELL_BURROWED, true);
            Brood(kGrublingsPerBurrow);
            StartBurrow(_burrow);
            LOG_INFO("module", "Emberfall barrows: the Burrow-Mother burrows ({} of 2, instance {})", Burrows(), me->GetInstanceId());
        }

        void Surface()
        {
            me->RemoveAurasDueToSpell(SPELL_BURROWED);
            me->RemoveUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE);
            me->SetReactState(REACT_AGGRESSIVE);
        }

        std::uint32_t Burrows() const { return std::uint32_t(std::popcount(_fired)) - _pending; }

        BurrowState _burrow;
        std::uint32_t _fired = 0, _pending = 0;
    };

    // ---- B4 the Last Stoker ------------------------------------------------------------------------------------------
    // Ideas from: Ick's pursuit (Pit of Saron) and Xevozz's ethereal spheres (the Violet Hold) for the cinders that hunt
    // someone; the classic burning ground under a random player (Pyre Ash, as before, now telegraphed); a healer's spike
    // read off the cast bar (the brand).
    struct boss_ef_last_stoker : public BossAI
    {
        enum Events : std::uint32_t { EVENT_ASH = 1, EVENT_BRAND, EVENT_BANK };

        explicit boss_ef_last_stoker(Creature* c) : BossAI(c, BOSS_LAST_STOKER) { Companions::SetBossMark(me); }

        void Reset() override
        {
            _Reset();
            _fired = 0;
            _bank = false;
        }

        void JustEngagedWith(Unit* /*who*/) override
        {
            _JustEngagedWith();
            Talk(Text::STOKER_AGGRO);
            First(events, EVENT_ASH, Timers::kPyreAsh);
            First(events, EVENT_BRAND, Timers::kBrand);
        }

        void JustDied(Unit* /*killer*/) override
        {
            _JustDied();
            Talk(Text::STOKER_DEATH);
            Report(instance, Dungeons::DATA_SEAL_PYRE);
        }

        void DamageTaken(Unit* attacker, std::uint32_t& damage, DamageEffectType type, SpellSchoolMask school) override
        {
            BossAI::DamageTaken(attacker, damage, type, school);
            float const pct = HealthPctAfter(me, damage);
            for (std::size_t i : CrossedThresholds(Thresholds::kCinders, pct, _fired))
                Cinders(i);
            if (!_bank && pct <= float(Thresholds::kBank))
            {
                _bank = true;
                Talk(Text::STOKER_HALF);
                First(events, EVENT_BANK, Timers::kBank);
            }
        }

        void ExecuteEvent(std::uint32_t id) override
        {
            switch (id)
            {
                case EVENT_ASH:
                    if (Unit* target = Anyone(this, 40.0f))
                        me->CastSpell(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(), SPELL_PYRE_ASH, false);
                    Again(events, EVENT_ASH, Timers::kPyreAsh);
                    break;
                case EVENT_BRAND:
                    DoCastVictim(SPELL_STOKERS_BRAND);
                    Again(events, EVENT_BRAND, Timers::kBrand);
                    break;
                case EVENT_BANK:
                    Talk(Text::STOKER_BANK);
                    DoCastSelf(SPELL_BANK_THE_FIRES);
                    Again(events, EVENT_BANK, Timers::kBank);
                    break;
            }
        }

    private:
        // Two cinders, each after someone who is not the tank (two different ones when there are two).
        void Cinders(std::size_t wave)
        {
            Talk(Text::STOKER_CINDERS);
            std::vector<Unit*> prey;
            for (std::uint32_t i = 0; i < kCindersPerWave * 4 && prey.size() < kCindersPerWave; ++i)
                if (Unit* t = SomeoneElse(this, 60.0f); t && std::find(prey.begin(), prey.end(), t) == prey.end())
                    prey.push_back(t);
            if (prey.empty())
                if (Unit* t = Anyone(this, 60.0f))
                    prey.push_back(t);
            Point const here{ me->GetPositionX(), me->GetPositionY(), me->GetPositionZ() };
            std::vector<Point> const spots = Ring(here, 12.0f, kCindersPerWave, me->GetOrientation() + float(M_PI) + 0.7f * float(wave));
            for (std::size_t i = 0; i < prey.size(); ++i)
            {
                Point const p = IntoRoom(spots[i % spots.size()], kPyreBarrow, kPyreRadius, 3.0f);
                if (TempSummon* cinder = me->SummonCreature(NPC_PYRE_CINDER, p.x, p.y, GroundZ(me, p.x, p.y, here.z), 0.0f,
                                                            TEMPSUMMON_TIMED_OR_CORPSE_DESPAWN, 30000))
                {
                    cinder->AI()->SetGUID(prey[i]->GetGUID());
                    DoCast(prey[i], SPELL_CINDER_MARKED, true);
                    Talk(Text::STOKER_CINDER_WHISPER, prey[i]);
                }
            }
            LOG_INFO("module", "Emberfall barrows: the Last Stoker's cinders ({} after {} of the group, wave {}, instance {})",
                     prey.size(), prey.size(), wave + 1, me->GetInstanceId());
        }

        std::uint32_t _fired = 0;
        bool _bank = false;
    };

    // A pyre cinder: crawls after its prey; within reach it bursts (Cinder Burst, 6 yd) and is gone.
    struct npc_ef_pyre_cinder : public ScriptedAI
    {
        explicit npc_ef_pyre_cinder(Creature* c) : ScriptedAI(c) { }

        void IsSummonedBy(WorldObject* /*summoner*/) override
        {
            me->SetReactState(REACT_PASSIVE);
            me->SetSpeedRate(MOVE_RUN, kCinderRunSpeed);
            Companions::SetKillPriority(me, true);
            me->SetInCombatWithZone();
        }

        void SetGUID(ObjectGuid const& guid, int32 /*id*/) override
        {
            _prey = guid;
            if (Unit* prey = ObjectAccessor::GetUnit(*me, _prey))
                me->GetMotionMaster()->MoveFollow(prey, 0.5f, 0.0f);
        }

        void AttackStart(Unit* /*who*/) override { }
        void MoveInLineOfSight(Unit* /*who*/) override { }
        void EnterEvadeMode(EvadeReason /*why*/) override { }

        void JustDied(Unit* /*killer*/) override
        {
            LOG_INFO("module", "Emberfall barrows: a pyre cinder killed before it reached its prey (instance {})", me->GetInstanceId());
            Unmark();
        }

        void UpdateAI(std::uint32_t diff) override
        {
            if (_done)
                return;
            _check = _check > diff ? _check - diff : 0;
            if (_check)
                return;
            _check = 250;
            Unit* prey = ObjectAccessor::GetUnit(*me, _prey);
            if (!prey || !prey->IsAlive() || !prey->IsInMap(me))
            {
                me->DespawnOrUnsummon();
                return;
            }
            if (me->GetExactDist2d(prey) <= kCinderReach + prey->GetCombatReach())
            {
                _done = true;
                LOG_INFO("module", "Emberfall barrows: a pyre cinder reached its prey and bursts (instance {})", me->GetInstanceId());
                DoCastSelf(SPELL_CINDER_BURST, true);
                Unmark();
                me->DespawnOrUnsummon(300ms);
                return;
            }
            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
                me->GetMotionMaster()->MoveFollow(prey, 0.5f, 0.0f);
        }

    private:
        void Unmark()
        {
            if (Unit* prey = ObjectAccessor::GetUnit(*me, _prey))
                prey->RemoveAurasDueToSpell(SPELL_CINDER_MARKED);
        }

        ObjectGuid _prey;
        std::uint32_t _check = 0;
        bool _done = false;
    };

    // ---- B5 the First Interred ---------------------------------------------------------------------------------------
    // Ideas from: Loken's Lightning Nova (Halls of Lightning) and Keli'dan's Burning Nova (the Blood Furnace) for the
    // nova everyone near runs from; Novos the Summoner (Drak'Tharon Keep) and Selin Fireheart's crystals (Magisters'
    // Terrace) for the ward its bearers hold until killed; the model's aura, adds and enrage as before.
    struct boss_ef_first_interred : public BossAI
    {
        enum Events : std::uint32_t { EVENT_WEIGHT = 1, EVENT_SENTENCE, EVENT_KNEEL, EVENT_HARD_ENRAGE };

        explicit boss_ef_first_interred(Creature* c) : BossAI(c, BOSS_FIRST_INTERRED) { Companions::SetBossMark(me); }

        void Reset() override
        {
            _Reset();
            _fired = 0;
            _enraged = false;
            _ward = WardState{};
        }

        void JustEngagedWith(Unit* /*who*/) override
        {
            _JustEngagedWith();
            Talk(Text::INTERRED_AGGRO);
            First(events, EVENT_WEIGHT, Timers::kWeight);
            First(events, EVENT_SENTENCE, Timers::kSentence);
            First(events, EVENT_KNEEL, Timers::kKneel);
            events.ScheduleEvent(EVENT_HARD_ENRAGE, Ms(Timers::kHardEnrageMs));
        }

        void JustDied(Unit* /*killer*/) override
        {
            _JustDied();
            Talk(Text::INTERRED_DEATH);
            Report(instance, Dungeons::DATA_FIRST_INTERRED);
        }

        void DamageTaken(Unit* attacker, std::uint32_t& damage, DamageEffectType type, SpellSchoolMask school) override
        {
            BossAI::DamageTaken(attacker, damage, type, school);
            float const pct = HealthPctAfter(me, damage);
            for (std::size_t i : CrossedThresholds(Thresholds::kWard, pct, _fired))
                Ward(i);
            if (!_enraged && pct <= float(Thresholds::kEnrage))
            {
                _enraged = true;
                Talk(Text::INTERRED_ENRAGE);
                DoCastSelf(SPELL_EMPIRE_REMEMBERS, true);
            }
        }

        void SummonedCreatureDies(Creature* summon, Unit* killer) override
        {
            BossAI::SummonedCreatureDies(summon, killer);
            if (summon->GetEntry() == NPC_WARD_BEARER && BearerDied(_ward))
            {
                me->RemoveAurasDueToSpell(SPELL_GRAVE_WARD);
                Talk(Text::INTERRED_WARD_DOWN);
                LOG_INFO("module", "Emberfall barrows: the Grave Ward breaks (instance {})", me->GetInstanceId());
            }
        }

        void ExecuteEvent(std::uint32_t id) override
        {
            switch (id)
            {
                case EVENT_WEIGHT:
                    DoCastSelf(SPELL_BARROW_WEIGHT, true);
                    events.ScheduleEvent(EVENT_WEIGHT, Ms(_ward.up ? Timers::kWeightWardedMs : Timers::kWeight.min));
                    break;
                case EVENT_SENTENCE:
                    DoCastVictim(SPELL_PREFECTS_SENTENCE);
                    Again(events, EVENT_SENTENCE, Timers::kSentence);
                    break;
                case EVENT_KNEEL:
                    Talk(Text::INTERRED_KNEEL_YELL);
                    Talk(Text::INTERRED_KNEEL);
                    DoCastSelf(SPELL_KNEEL);
                    Again(events, EVENT_KNEEL, Timers::kKneel);
                    break;
                case EVENT_HARD_ENRAGE:
                    DoCastSelf(SPELL_LONG_PATIENCE_ENDS, true);
                    break;
            }
        }

    private:
        // The Grave Ward: two bearers at the barrow's edge on either side of it hold it; two servants come for the group.
        void Ward(std::size_t wave)
        {
            Talk(Text::INTERRED_WARD_YELL);
            Talk(Text::INTERRED_WARD);
            DoCastSelf(SPELL_GRAVE_WARD, true);
            RaiseWard(_ward, kBearersPerWard);
            Point const here{ me->GetPositionX(), me->GetPositionY(), me->GetPositionZ() };
            float const side = me->GetOrientation() + float(M_PI) * 0.5f + 0.4f * float(wave);
            for (Point p : Ring(kFirstBarrow, kBearerRadius, kBearersPerWard, side))
            {
                p = IntoRoom(p, kFirstBarrow, kFirstRadius, 4.0f);
                me->SummonCreature(NPC_WARD_BEARER, p.x, p.y, GroundZ(me, p.x, p.y, kFirstBarrow.z), 0.0f, TEMPSUMMON_CORPSE_TIMED_DESPAWN, 30000);
            }
            for (Point p : Ring(here, 8.0f, kServantsPerWard, me->GetOrientation()))
                me->SummonCreature(NPC_RISEN_SERVANT, p.x, p.y, GroundZ(me, p.x, p.y, here.z), 0.0f, TEMPSUMMON_CORPSE_TIMED_DESPAWN, 60000);
            LOG_INFO("module", "Emberfall barrows: the Grave Ward rises ({} of 2, instance {})", _ward.wards, me->GetInstanceId());
        }

        std::uint32_t _fired = 0;
        bool _enraged = false;
        WardState _ward;
    };

    // A ward-bearer: stands where it came and holds the ward over its prefect (Bear the Ward, a channel) until killed.
    struct npc_ef_ward_bearer : public ScriptedAI
    {
        explicit npc_ef_ward_bearer(Creature* c) : ScriptedAI(c) { }

        void IsSummonedBy(WorldObject* summoner) override
        {
            _prefect = summoner ? summoner->GetGUID() : ObjectGuid::Empty;
            me->SetReactState(REACT_PASSIVE);
            Companions::SetKillPriority(me, true);
            me->SetInCombatWithZone();
            _recast = 500;
        }

        void AttackStart(Unit* /*who*/) override { }
        void MoveInLineOfSight(Unit* /*who*/) override { }
        void EnterEvadeMode(EvadeReason /*why*/) override { }

        void UpdateAI(std::uint32_t diff) override
        {
            Creature* prefect = ObjectAccessor::GetCreature(*me, _prefect);
            if (!prefect || !prefect->IsAlive() || !prefect->IsInCombat())
            {
                me->DespawnOrUnsummon();
                return;
            }
            _recast = _recast > diff ? _recast - diff : 0;
            if (_recast || me->IsNonMeleeSpellCast(false))
                return;
            _recast = 2000;
            me->SetFacingToObject(prefect);
            me->CastSpell(prefect, SPELL_BEAR_THE_WARD, false);
        }

    private:
        ObjectGuid _prefect;
        std::uint32_t _recast = 0;
    };

    template <typename AI>
    class EfBarrowsCreatureScript : public CreatureScript
    {
    public:
        explicit EfBarrowsCreatureScript(char const* name) : CreatureScript(name) { }
        CreatureAI* GetAI(Creature* creature) const override { return new AI(creature); }
    };
}

void AddEmberfallBarrowsBossScripts()
{
    new EfBarrowsCreatureScript<boss_ef_post_keeper>("boss_ef_post_keeper");
    new EfBarrowsCreatureScript<boss_ef_keening_mother>("boss_ef_keening_mother");
    new EfBarrowsCreatureScript<boss_ef_burrow_mother>("boss_ef_burrow_mother");
    new EfBarrowsCreatureScript<boss_ef_last_stoker>("boss_ef_last_stoker");
    new EfBarrowsCreatureScript<boss_ef_first_interred>("boss_ef_first_interred");
    new EfBarrowsCreatureScript<npc_ef_mourning_shade>("npc_ef_mourning_shade");
    new EfBarrowsCreatureScript<npc_ef_pyre_cinder>("npc_ef_pyre_cinder");
    new EfBarrowsCreatureScript<npc_ef_ward_bearer>("npc_ef_ward_bearer");
    // The Post-Keeper's mark: Struck from the Count hits 8 yd round its bearer when it ends (companions walk clear).
    Emberfall::Companions::RegisterRunOut(SPELL_NUMBERED, 8.0f);
}
