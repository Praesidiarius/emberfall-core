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

// The Barrows of Cairnmoor's bosses, v2 (Emberfall docs/design/dungeon-barrows.md §5a, 2026-10-31 [proposal]): five
// BossAI scripts in the AzerothCore style (an EventMap with phases for the timers, the SummonList for the adds) and their
// adds' AIs, built on the boss toolkit (src/dungeons/EfBossToolkit.h, EfBossKit.h: intermissions, the energy bar, cast
// chains, soaks, the shrinking arena). Every ID, timer, threshold and place is src/dungeons/EfBarrowsBossLogic.h
// (tests/dungeon_tests.cpp checks the timers and the phases); the spells and texts are Emberfall's content
// (content/data/spells/barrows.yaml, content/data/barrows/creatures.yaml).
//
//   boss_ef_post_keeper     B1 the gate, 2 phases: Tally Sweep, Numbered, Shoulder the Post; from 50 % the recount:
//                           two numbered at once and Driven Posts under three of the group as each sweep lands
//   boss_ef_keening_mother  B2, 3 phases: Keen, Grave Chill, Mourning Veil; at 65 % the wake (immune in the middle, three
//                           keening mourners lament: interrupt and kill them); then the Keening Chorus (three keens in a
//                           row: an interrupt rotation) and mourning shades at 35 %
//   boss_ef_burrow_mother   B3: the brood, Brood Bile, the brood clutch (stamp it out: a soak); at 66/33 % she burrows
//                           and tunnels after someone (the furrow) while the earth heaves; frenzied below 33 %
//   boss_ef_last_stoker     B4, the heat bar: Pyre Ash, Stoker's Brand (Branded stacks on the tank), cinders at
//                           75/50/25 %; at 100 heat the Last Pyre (shelter in a cold hearth; a hearth clears the brand);
//                           from 50 % Bank the Fires and a faster pyre
//   boss_ef_first_interred  B5, 3 phases: Barrow Weight, Prefect's Sentence, Kneel; at 70 % the Grave Ward (immune while
//                           its two bearers live); phase 2 the Imperial Tithe (a soak of two); at 40 % the mound falls
//                           (immune 5 s, the arena shrinks to 20 yd, its edge hurts); 25 % enrage; 15 % the arena to 15 yd
//   npc_ef_mourning_shade, npc_ef_pyre_cinder, npc_ef_ward_bearer, npc_ef_keening_mourner   the adds that must die
//
// The bosses report their deaths to instance_ef_barrows (the seals, emberfall_dungeons.cpp). The companions know the
// mechanics from the spells themselves (telegraphed casts, interruptible casts) and from what is registered here: the
// run-out mark, the soaks and the shelter, the ground casters, the furrow (EfCompanions.h, through EfBossKit.h).

#include "CreatureAI.h"
#include "CreatureScript.h"
#include "EfBarrowsBossLogic.h"
#include "EfBossKit.h"
#include "EfCompanions.h"
#include "EfDungeonLogic.h"
#include "InstanceScript.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptedCreature.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "TemporarySummon.h"

#include <algorithm>
#include <bit>
#include <list>

using namespace Emberfall::Barrows;
namespace Kit = Emberfall::Bosses;
namespace Companions = Emberfall::Companions;
namespace Dungeons = Emberfall::Dungeons;

namespace
{
    Milliseconds Ms(std::uint32_t ms) { return Milliseconds(ms); }

    // Schedules `id` at the timer's first time / again within its range (in `phase`, 0: every phase).
    void First(EventMap& events, std::uint32_t id, Timer const& t, std::uint8_t phase = 0) { events.ScheduleEvent(id, Ms(t.first), 0, phase); }
    void Again(EventMap& events, std::uint32_t id, Timer const& t, std::uint8_t phase = 0) { events.ScheduleEvent(id, Ms(t.min), Ms(t.max), 0, phase); }

    // The ground height near (x, y), or z where the map has none.
    float GroundZ(Creature* me, float x, float y, float z)
    {
        float const g = me->GetMapHeight(x, y, z + 4.0f);
        return g > INVALID_HEIGHT && std::fabs(g - z) < 6.0f ? g : z;
    }

    Point Where(WorldObject const* o) { return Point{ o->GetPositionX(), o->GetPositionY(), o->GetPositionZ() }; }

    // Someone on the threat list who is not the boss's victim (the tank), within `range`; else nobody.
    Unit* SomeoneElse(BossAI* ai, float range) { return ai->SelectTarget(SelectTargetMethod::Random, 0, range, false, false); }
    Unit* Anyone(BossAI* ai, float range) { return ai->SelectTarget(SelectTargetMethod::Random, 0, range, false, true); }
    Unit* SomeoneElseOrAnyone(BossAI* ai, float range)
    {
        Unit* u = SomeoneElse(ai, range);
        return u ? u : Anyone(ai, range);
    }

    // `count` different ones of the group, the tank last (only when there are not enough others).
    std::vector<Unit*> Several(BossAI* ai, std::uint32_t count, float range)
    {
        std::list<Unit*> others;
        ai->SelectTargetList(others, count, SelectTargetMethod::Random, 0, range, false, false);
        std::vector<Unit*> out(others.begin(), others.end());
        if (out.size() < count)
            if (Unit* tank = ai->SelectTarget(SelectTargetMethod::MaxThreat, 0, range, false, true);
                tank && std::find(out.begin(), out.end(), tank) == out.end())
                out.push_back(tank);
        return out;
    }

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

    TempSummon* SummonAt(Creature* me, std::uint32_t entry, Point p, TempSummonType type, std::uint32_t ms)
    {
        return me->SummonCreature(entry, p.x, p.y, GroundZ(me, p.x, p.y, p.z), 0.0f, type, ms);
    }

    enum Actions : int32 { ACTION_SHADE_DRUNK = 1, ACTION_CINDER_BURST = 2 };
    enum Data : std::uint32_t { DATA_MOURNER_INDEX = 1 };

    // ---- B1 the Post-Keeper ------------------------------------------------------------------------------------------
    // Ideas from: Ingvar the Plunderer's Smash (Utgarde Keep) for the frontal the tank turns away; Ionar's Static Overload
    // (Halls of Lightning) and Urom's Time Bomb (the Oculus) for the mark its bearer carries away from the group; v2 (the
    // Midnight research, docs/research/dungeon-mechanics-midnight.md: Lightwarden Ruia's kit at a threshold, the frontal
    // plus ground combos of Emberdawn and Lightblossom Trinity) for the recount: a second phase that adds ground to the
    // frontal and doubles the mark.
    struct boss_ef_post_keeper : public BossAI
    {
        enum Events : std::uint32_t { EVENT_SWEEP = 1, EVENT_NUMBERED, EVENT_SHOULDER, EVENT_POSTS };

        explicit boss_ef_post_keeper(Creature* c) : BossAI(c, BOSS_POST_KEEPER) { Companions::SetBossMark(me); }

        void Reset() override
        {
            _Reset();
            _phase = PHASE_ONE;
            _fired = 0;
        }

        void JustEngagedWith(Unit* /*who*/) override
        {
            _JustEngagedWith();
            Talk(Text::KEEPER_AGGRO);
            events.SetPhase(PHASE_ONE);
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

        void DamageTaken(Unit* attacker, std::uint32_t& damage, DamageEffectType type, SpellSchoolMask school) override
        {
            BossAI::DamageTaken(attacker, damage, type, school);
            if (!CrossedThresholds(Thresholds::kRecount, HealthPctAfter(me, damage), _fired).empty())
            {
                _phase = PHASE_TWO;
                events.SetPhase(PHASE_TWO);
                Talk(Text::KEEPER_RECOUNT_YELL);
                Talk(Text::KEEPER_RECOUNT);
                LOG_INFO("module", "Emberfall barrows: the Post-Keeper's recount, phase 2 (instance {})", me->GetInstanceId());
            }
        }

        void ExecuteEvent(std::uint32_t id) override
        {
            switch (id)
            {
                case EVENT_SWEEP:
                    Talk(Text::KEEPER_SWEEP);
                    DoCastVictim(SPELL_TALLY_SWEEP);
                    if (_phase >= PHASE_TWO)
                        events.ScheduleEvent(EVENT_POSTS, Ms(Timers::kPostsAfterSweepMs));
                    Again(events, EVENT_SWEEP, Timers::kTallySweep);
                    break;
                case EVENT_POSTS:
                    Posts();
                    break;
                case EVENT_NUMBERED:
                {
                    std::vector<Unit*> marked;
                    for (std::uint32_t i = 0; i < NumberedPerMark(_phase) * 3 && marked.size() < NumberedPerMark(_phase); ++i)
                        if (Unit* t = SomeoneElse(this, 40.0f); t && std::find(marked.begin(), marked.end(), t) == marked.end())
                            marked.push_back(t);
                    if (!marked.empty())
                        Talk(Text::KEEPER_NUMBERED);
                    for (Unit* target : marked)
                    {
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
                }
                case EVENT_SHOULDER:
                    DoCastVictim(SPELL_SHOULDER_THE_POST);
                    Again(events, EVENT_SHOULDER, Timers::kShoulder);
                    break;
            }
        }

    private:
        // Phase 2: a count-post driven in under three of the group (the tank last) as the sweep lands.
        void Posts()
        {
            std::vector<Unit*> const under = Several(this, kPostsPerSweep, 40.0f);
            if (under.empty())
                return;
            Talk(Text::KEEPER_POSTS);
            for (Unit* u : under)
                Kit::SummonGroundCaster(me, NPC_DRIVEN_POST, u->GetPositionX(), u->GetPositionY(),
                                           GroundZ(me, u->GetPositionX(), u->GetPositionY(), u->GetPositionZ()), SPELL_DRIVEN_POST,
                                           Timers::kPostCastMs + 1500);
        }

        std::uint8_t _phase = PHASE_ONE;
        std::uint32_t _fired = 0;
    };

    // ---- B2 the Keening Mother ---------------------------------------------------------------------------------------
    // Ideas from: Grandmaster Vorpil's void travellers (Shadow Labyrinth) and Bronjahm's soul fragments (the Forge of
    // Souls), adds that walk to the boss and feed it unless killed on the way; Salramm the Fleshcrafter's curse for a
    // debuff a healer takes off; v2 (docs/research/dungeon-mechanics-midnight.md): an immune intermission held by adds
    // that cast (Commander Kroluk's and Corewarden Nysarra's interrupt-target waves, Kystia Manaheart's copies) and a
    // must-interrupt cast three times in a row (the Writhing Coil) for the chorus.
    struct boss_ef_keening_mother : public BossAI
    {
        enum Events : std::uint32_t { EVENT_KEEN = 1, EVENT_CHILL, EVENT_VEIL, EVENT_CHORUS };

        explicit boss_ef_keening_mother(Creature* c) : BossAI(c, BOSS_KEENING_MOTHER) { Companions::SetBossMark(me); }

        void Reset() override
        {
            _Reset();
            me->RemoveAurasDueToSpell(SPELL_SHROUD_OF_THE_MOURNED);
            me->SetReactState(REACT_AGGRESSIVE);
            _fired = _shadesFired = 0;
            _phase = PHASE_ONE;
            _wake = Kit::Intermission{};
            _chain = Kit::CastChain{};
        }

        void JustEngagedWith(Unit* /*who*/) override
        {
            _JustEngagedWith();
            Talk(Text::KEENING_AGGRO);
            events.SetPhase(PHASE_ONE);
            First(events, EVENT_KEEN, Timers::kKeen, PHASE_ONE);
            First(events, EVENT_CHILL, Timers::kGraveChill, PHASE_ONE);
            First(events, EVENT_VEIL, Timers::kMourningVeil, PHASE_ONE);
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
            float const pct = HealthPctAfter(me, damage);
            if (_phase == PHASE_ONE && !CrossedThresholds(Thresholds::kWake, pct, _fired).empty())
            {
                StartWake();
                return;
            }
            if (_phase == PHASE_TWO)
                for (std::size_t i : CrossedThresholds(Thresholds::kShades, pct, _shadesFired))
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

        void SummonedCreatureDies(Creature* summon, Unit* killer) override
        {
            BossAI::SummonedCreatureDies(summon, killer);
            if (summon->GetEntry() == NPC_KEENING_MOURNER && Kit::IntermissionAddDied(_wake))
                EndWake();
        }

        void UpdateAI(std::uint32_t diff) override
        {
            if (!UpdateVictim())
                return;
            if (_wake.active && Kit::StepIntermission(_wake, diff))
                WakeTimedOut();
            bool const casting = me->HasUnitState(UNIT_STATE_CASTING);
            if (Kit::StepChain(_chain, diff, casting))
                DoCastSelf(SPELL_CHORUS_KEEN);
            events.Update(diff);
            // While a chorus runs nothing else starts between its keens (the others wait for it).
            if (me->HasUnitState(UNIT_STATE_CASTING) || _chain.left)
                return;
            while (std::uint32_t const id = events.ExecuteEvent())
            {
                ExecuteEvent(id);
                if (me->HasUnitState(UNIT_STATE_CASTING))
                    return;
            }
            if (!_wake.active)
                DoMeleeAttackIfReady();
        }

        void ExecuteEvent(std::uint32_t id) override
        {
            switch (id)
            {
                case EVENT_KEEN:
                    Talk(Text::KEENING_KEEN);
                    DoCastSelf(SPELL_KEEN);
                    Again(events, EVENT_KEEN, Timers::kKeen, PHASE_ONE);
                    break;
                case EVENT_CHILL:
                    DoCastVictim(SPELL_GRAVE_CHILL);
                    Again(events, EVENT_CHILL, Timers::kGraveChill, _phase);
                    break;
                case EVENT_VEIL:
                    if (Unit* target = SomeoneElseOrAnyone(this, 40.0f))
                        DoCast(target, SPELL_MOURNING_VEIL, true);
                    Again(events, EVENT_VEIL, _wake.active ? Timers::kWakeVeil : Timers::kMourningVeil, _phase);
                    break;
                case EVENT_CHORUS:
                    Talk(Text::KEENING_CHORUS);
                    Kit::StartChain(_chain, Timers::kChorusCasts, Timers::kChorusGapMs);
                    Again(events, EVENT_CHORUS, Timers::kChorus, PHASE_TWO);
                    break;
            }
        }

    private:
        // The wake: she rises into the barrow's middle under the shroud (no damage taken) while three mourners at its wall
        // lament (interrupt them); it ends with the last of them.
        void StartWake()
        {
            _phase = PHASE_INTERMISSION;
            events.Reset();
            events.SetPhase(PHASE_INTERMISSION);
            Talk(Text::KEENING_WAKE_YELL);
            Talk(Text::KEENING_WAKE);
            Position const middle(kWidowsBarrow.x, kWidowsBarrow.y, kWidowsBarrow.z, 0.0f);
            Kit::BeginImmune(me, SPELL_SHROUD_OF_THE_MOURNED, &middle);
            float const start = me->GetOrientation();
            std::uint32_t i = 0;
            for (Point p : Ring(kWidowsBarrow, kMournerRadius, kMournersPerWake, start))
                if (TempSummon* m = SummonAt(me, NPC_KEENING_MOURNER, p, TEMPSUMMON_CORPSE_TIMED_DESPAWN, 20000))
                    m->AI()->SetData(DATA_MOURNER_INDEX, i++);
            Kit::StartIntermission(_wake, Timers::kWakeLimitMs, i);
            First(events, EVENT_VEIL, Timers::kWakeVeil, PHASE_INTERMISSION);
            LOG_INFO("module", "Emberfall barrows: the Keening Mother's wake, {} mourners (instance {})", i, me->GetInstanceId());
            if (!i)
                EndWake();
        }

        // Time is up with mourners left: she drinks them (each a Drink the Grief) and the wake ends.
        void WakeTimedOut()
        {
            std::list<Creature*> left;
            me->GetCreatureListWithEntryInGrid(left, NPC_KEENING_MOURNER, 80.0f);
            for (Creature* m : left)
                if (m->IsAlive())
                {
                    DoAction(ACTION_SHADE_DRUNK);
                    m->DespawnOrUnsummon();
                }
            EndWake();
        }

        void EndWake()
        {
            _wake.active = false;
            _phase = PHASE_TWO;
            Kit::EndImmune(me, SPELL_SHROUD_OF_THE_MOURNED);
            Talk(Text::KEENING_WAKE_END);
            events.Reset();
            events.SetPhase(PHASE_TWO);
            First(events, EVENT_CHILL, Timers::kGraveChill, PHASE_TWO);
            First(events, EVENT_VEIL, Timers::kMourningVeil, PHASE_TWO);
            First(events, EVENT_CHORUS, Timers::kChorus, PHASE_TWO);
            LOG_INFO("module", "Emberfall barrows: the Keening Mother's wake ends, phase 2 (instance {})", me->GetInstanceId());
        }

        // Three shades rise round her (inside the barrow's wall) and drift to her.
        void Shades(std::size_t wave)
        {
            Talk(Text::KEENING_SHADES_YELL);
            Talk(Text::KEENING_SHADES);
            Point const here = Where(me);
            float const start = me->GetOrientation() + 0.5f * float(wave);
            for (Point p : FarthestOnRing(kWidowsBarrow, kShadeSpawnRadius, 8, kShadesPerWave, here, start))
                SummonAt(me, NPC_MOURNING_SHADE, p, TEMPSUMMON_CORPSE_TIMED_DESPAWN, 20000);
            LOG_INFO("module", "Emberfall barrows: the Keening Mother calls {} mourning shades (instance {})", kShadesPerWave, me->GetInstanceId());
        }

        std::uint8_t _phase = PHASE_ONE;
        std::uint32_t _fired = 0, _shadesFired = 0;
        Kit::Intermission _wake;
        Kit::CastChain _chain;
    };

    // A keening mourner (the wake): stands at the barrow's wall and laments (interruptible) until killed; each lament
    // heard swells the Keening Mother's grief.
    struct npc_ef_keening_mourner : public ScriptedAI
    {
        explicit npc_ef_keening_mourner(Creature* c) : ScriptedAI(c) { }

        void IsSummonedBy(WorldObject* summoner) override
        {
            _mother = summoner ? summoner->GetGUID() : ObjectGuid::Empty;
            me->SetReactState(REACT_PASSIVE);
            me->SetControlled(true, UNIT_STATE_ROOT);
            Companions::SetKillPriority(me, true);
            // Its laments are kicked like a boss's casts (the companions keep their kicks for a boss's while one fights).
            Companions::SetBossMark(me);
            me->SetInCombatWithZone();
            _next = Timers::kLament.first;
        }

        void SetData(std::uint32_t id, std::uint32_t value) override
        {
            if (id == DATA_MOURNER_INDEX)
                _next = Timers::kLament.first + value * Timers::kLamentStaggerMs;
        }

        void AttackStart(Unit* /*who*/) override { }
        void MoveInLineOfSight(Unit* /*who*/) override { }
        void EnterEvadeMode(EvadeReason /*why*/) override { }

        void OnSpellCast(SpellInfo const* spell) override
        {
            if (spell->Id != SPELL_LAMENT)
                return;
            if (Creature* mother = ObjectAccessor::GetCreature(*me, _mother); mother && mother->IsAlive())
                mother->CastSpell(mother, SPELL_GRIEF_SWELLS, true);
        }

        void UpdateAI(std::uint32_t diff) override
        {
            Creature* mother = ObjectAccessor::GetCreature(*me, _mother);
            if (!mother || !mother->IsAlive() || !mother->IsInCombat())
            {
                me->DespawnOrUnsummon();
                return;
            }
            _next = _next > diff ? _next - diff : 0;
            if (_next || me->HasUnitState(UNIT_STATE_CASTING))
                return;
            _next = urand(Timers::kLament.min, Timers::kLament.max);
            me->SetFacingToObject(mother);
            me->CastSpell(me, SPELL_LAMENT, false);
        }

    private:
        ObjectGuid _mother;
        std::uint32_t _next = 0;
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
    // group and adds come; Krik'thir's swarms for the brood the tank gathers; v2 (docs/research/dungeon-mechanics-
    // midnight.md): the timed soaks that burst into trouble when nobody takes them (the Hoardmonger's supplies, the
    // Sentinel of Winter's add-then-soak chain) for the clutch; a fixate that hunts one player while the others keep clear
    // (Commander Kroluk's spin, Ikuzz's gaze) for the furrow; Ikuzz's frenzy at a threshold for the last third.
    struct boss_ef_burrow_mother : public BossAI
    {
        enum Events : std::uint32_t { EVENT_BROOD = 1, EVENT_BILE, EVENT_CLUTCH };

        explicit boss_ef_burrow_mother(Creature* c) : BossAI(c, BOSS_BURROW_MOTHER) { Companions::SetBossMark(me); }

        void Reset() override
        {
            _Reset();
            Surface();
            _burrow = BurrowState{};
            _fired = 0;
            _pending = 0;
            _frenzied = false;
        }

        void JustEngagedWith(Unit* /*who*/) override
        {
            _JustEngagedWith();
            Talk(Text::BURROW_AGGRO);
            _runRate = me->GetSpeedRate(MOVE_RUN);
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

        // A clutch nobody stamped out hatches (the soak marker tells us, EfBossKit.h).
        void SetData(std::uint32_t id, std::uint32_t value) override
        {
            if (id != Kit::kSoakFailedData || value != NPC_BROOD_CLUTCH || !me->IsAlive())
                return;
            Talk(Text::BURROW_HATCH);
            for (Point p : Ring(_clutchAt, 1.5f, kGrublingsPerHatch, 0.0f))
                SummonAt(me, NPC_GRUBLING, p, TEMPSUMMON_CORPSE_TIMED_DESPAWN, 30000);
        }

        void UpdateAI(std::uint32_t diff) override
        {
            if (!UpdateVictim())
                return;
            events.Update(diff);
            bool const casting = me->HasUnitState(UNIT_STATE_CASTING);
            if (_burrow.phase != BurrowPhase::Above)
            {
                BurrowTick const t = StepBurrow(_burrow, diff, casting);
                if (t.retarget)
                    Retarget();
                if (t.churn)
                {
                    DoCastSelf(SPELL_CHURNING_EARTH, true);
                    if (Unit* prey = ObjectAccessor::GetUnit(*me, _prey); prey && prey->IsAlive())
                        me->GetMotionMaster()->MovePoint(0, prey->GetPositionX(), prey->GetPositionY(), prey->GetPositionZ());
                }
                switch (t.action)
                {
                    case BurrowAction::Heave:
                        if (Unit* target = Anyone(this, 60.0f))
                            me->CastSpell(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(), SPELL_HEAVING_EARTH, false);
                        break;
                    case BurrowAction::Emerge:
                        Talk(Text::BURROW_UP);
                        me->StopMoving();
                        me->GetMotionMaster()->Clear();
                        me->RemoveAurasDueToSpell(SPELL_BURROWED);
                        DoCastSelf(SPELL_EMERGE);
                        break;
                    case BurrowAction::Surface:
                        Surface();
                        if (Unit* v = me->SelectVictim())
                            AttackStart(v);
                        if (_burrow.burrows >= 2 && !_frenzied)
                        {
                            _frenzied = true;
                            Talk(Text::BURROW_FRENZY);
                        }
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
                    Again(events, EVENT_BROOD, _frenzied ? Timers::kBroodFrenzied : Timers::kBrood);
                    break;
                case EVENT_BILE:
                    if (Unit* target = Anyone(this, 30.0f))
                        DoCast(target, SPELL_BROOD_BILE, true);
                    Again(events, EVENT_BILE, Timers::kBroodBile);
                    break;
                case EVENT_CLUTCH:
                    Clutch();
                    Again(events, EVENT_CLUTCH, _frenzied ? Timers::kClutchFrenzied : Timers::kClutch);
                    break;
            }
        }

    private:
        void ScheduleAbove()
        {
            events.Reset();
            events.SetPhase(PHASE_ONE);
            First(events, EVENT_BROOD, _frenzied ? Timers::kBroodFrenzied : Timers::kBrood);
            First(events, EVENT_BILE, Timers::kBroodBile);
            First(events, EVENT_CLUTCH, _frenzied ? Timers::kClutchFrenzied : Timers::kClutch);
        }

        void Brood(std::uint32_t count)
        {
            Point const here = Where(me);
            for (Point p : Ring(here, 6.0f, count, me->GetOrientation() + 1.0f))
                SummonAt(me, NPC_GRUBLING, p, TEMPSUMMON_CORPSE_TIMED_DESPAWN, 30000);
        }

        // A clutch laid a few yards from someone of the group (towards the barrow's middle): one of them stamps it out.
        void Clutch()
        {
            Unit* near = SomeoneElseOrAnyone(this, 40.0f);
            if (!near)
                return;
            Point p = IntoRoom(Towards(Where(near), kBroodBarrow, 6.0f), kBroodBarrow, kBroodRadius, 4.0f);
            p.z = GroundZ(me, p.x, p.y, near->GetPositionZ());
            if (Kit::SummonSoak(me, NPC_BROOD_CLUTCH, p.x, p.y, p.z))
            {
                _clutchAt = p;
                Talk(Text::BURROW_CLUTCH);
            }
        }

        // The furrow: under the earth she tunnels after someone (not the tank when there is anyone else), told by a
        // whisper; a new prey every few seconds.
        void Retarget()
        {
            Unit* prey = SomeoneElseOrAnyone(this, 60.0f);
            if (!prey)
                return;
            if (prey->GetGUID() != _prey)
                Talk(Text::BURROW_FURROW_WHISPER, prey);
            _prey = prey->GetGUID();
            me->GetMotionMaster()->MovePoint(0, prey->GetPositionX(), prey->GetPositionY(), prey->GetPositionZ());
        }

        // Under the earth: not attackable, not swinging, tunnelling after someone; the heaves and the grublings meanwhile.
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
            me->SetSpeedRate(MOVE_RUN, _runRate * kFurrowSpeedRate);
            DoCastSelf(SPELL_BURROWED, true);
            Brood(kGrublingsPerBurrow);
            StartBurrow(_burrow);
            LOG_INFO("module", "Emberfall barrows: the Burrow-Mother burrows ({} of 2, instance {})", _burrow.burrows, me->GetInstanceId());
        }

        void Surface()
        {
            me->RemoveAurasDueToSpell(SPELL_BURROWED);
            me->RemoveUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE);
            me->SetReactState(REACT_AGGRESSIVE);
            if (_runRate > 0.0f)
                me->SetSpeedRate(MOVE_RUN, _runRate);
            _prey.Clear();
        }

        BurrowState _burrow;
        std::uint32_t _fired = 0, _pending = 0;
        bool _frenzied = false;
        float _runRate = 0.0f;
        ObjectGuid _prey;
        Point _clutchAt{ kBroodBarrow };
    };

    // ---- B4 the Last Stoker ------------------------------------------------------------------------------------------
    // Ideas from: Ick's pursuit (Pit of Saron) and Xevozz's ethereal spheres (the Violet Hold) for the cinders that hunt
    // someone; the classic burning ground under a random player; v2 (docs/research/dungeon-mechanics-midnight.md): an
    // energy bar whose ultimate at 100 the group answers by standing somewhere (the Derelict Duo's and Lothraxion's
    // energy, Seranel Sunlash's safe zone, Zul'jarra's shelter), and a stacking tank debuff a five-player group clears by
    // touching something rather than by a tank swap (the Restless Heart's arrows, Atroxus's creeper) for the brand.
    struct boss_ef_last_stoker : public BossAI
    {
        enum Events : std::uint32_t { EVENT_ASH = 1, EVENT_BRAND, EVENT_BANK };

        explicit boss_ef_last_stoker(Creature* c) : BossAI(c, BOSS_LAST_STOKER) { Companions::SetBossMark(me); }

        void Reset() override
        {
            _Reset();
            _fired = 0;
            _phase = PHASE_ONE;
            _heat = Kit::EnergyBar{};
            _heat.perSecond = kHeatPerSecond;
            _pyreUp = false;
            Kit::ShowEnergy(me, 0);
        }

        void JustEngagedWith(Unit* /*who*/) override
        {
            _JustEngagedWith();
            Talk(Text::STOKER_AGGRO);
            events.SetPhase(PHASE_ONE);
            First(events, EVENT_ASH, Timers::kPyreAsh);
            First(events, EVENT_BRAND, Timers::kBrand);
        }

        void JustDied(Unit* /*killer*/) override
        {
            _JustDied();
            Talk(Text::STOKER_DEATH);
            Kit::ShowEnergy(me, 0);
            Report(instance, Dungeons::DATA_SEAL_PYRE);
        }

        void DamageTaken(Unit* attacker, std::uint32_t& damage, DamageEffectType type, SpellSchoolMask school) override
        {
            BossAI::DamageTaken(attacker, damage, type, school);
            float const pct = HealthPctAfter(me, damage);
            for (std::size_t i : CrossedThresholds(Thresholds::kCinders, pct, _fired))
                Cinders(i);
            if (_phase == PHASE_ONE && pct <= float(Thresholds::kBank))
            {
                _phase = PHASE_TWO;
                events.SetPhase(PHASE_TWO);
                _heat.perSecond = kHeatPerSecondBanked;
                Talk(Text::STOKER_HALF);
                First(events, EVENT_BANK, Timers::kBank);
            }
        }

        void DoAction(int32 action) override
        {
            if (action == ACTION_CINDER_BURST)
                Kit::AddEnergy(_heat, kHeatPerBurst);
        }

        // The brand lands: a stack of Branded (the tank's to clear in a hearth).
        void SpellHitTarget(Unit* target, SpellInfo const* spell) override
        {
            if (spell->Id == SPELL_STOKERS_BRAND && target && target->IsAlive())
                me->CastSpell(target, SPELL_BRANDED, true);
        }

        void UpdateAI(std::uint32_t diff) override
        {
            if (!UpdateVictim())
                return;
            Kit::StepEnergy(_heat, diff);
            Kit::ShowEnergy(me, Kit::Energy(_heat));
            events.Update(diff);
            if (me->HasUnitState(UNIT_STATE_CASTING))
                return;
            if (!_pyreUp && Kit::Energy(_heat) >= 100)
            {
                LastPyre();
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
                    Again(events, EVENT_BANK, Timers::kBank, PHASE_TWO);
                    break;
            }
        }

    private:
        // The heat is full: three cold hearths open round the barrow's middle and the pyre roars (4 s). Whoever is not in
        // a hearth when it ends is burned; whoever is, is cooled (Branded gone). The heat starts again from nothing.
        void LastPyre()
        {
            _pyreUp = true;
            _heat.paused = true;
            Talk(Text::STOKER_PYRE_YELL);
            Talk(Text::STOKER_PYRE);
            std::vector<Kit::Point2> hearths;
            for (Point p : Ring(kPyreBarrow, kHearthRing, kHearths, frand(0.0f, 6.28f)))
            {
                p = IntoRoom(p, kPyreBarrow, kPyreRadius, 4.0f);
                p.z = GroundZ(me, p.x, p.y, kPyreBarrow.z);
                if (Kit::SummonMarker(me, NPC_COLD_HEARTH, p.x, p.y, p.z, SPELL_COLD_HEARTH, Timers::kHearthLifeMs))
                    hearths.push_back(Kit::Point2{ p.x, p.y });
            }
            events.DelayEvents(Ms(Timers::kLastPyreCastMs + 1000));
            DoCastSelf(SPELL_THE_LAST_PYRE);
            me->m_Events.AddEventAtOffset([this, hearths]
            {
                if (!me->IsAlive() || !me->IsInCombat())
                    return;
                std::uint32_t burned = 0, cooled = 0;
                for (Unit* u : Kit::GroupSideNear(me, 80.0f))
                {
                    if (Kit::InAnyShelter(Kit::Point2{ u->GetPositionX(), u->GetPositionY() }, hearths, kHearthRadius + u->GetCombatReach() * 0.5f))
                    {
                        u->RemoveAurasDueToSpell(SPELL_BRANDED);
                        ++cooled;
                    }
                    else
                    {
                        me->CastSpell(u, SPELL_PYRE_ROAR, true);
                        ++burned;
                    }
                }
                Kit::ResetEnergy(_heat);
                _heat.paused = false;
                _pyreUp = false;
                LOG_INFO("module", "Emberfall barrows: the Last Pyre ({} sheltered, {} burned, instance {})", cooled, burned, me->GetInstanceId());
            }, Ms(Timers::kLastPyreCastMs));
        }

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
            Point const here = Where(me);
            std::vector<Point> const spots = Ring(here, 12.0f, kCindersPerWave, me->GetOrientation() + float(M_PI) + 0.7f * float(wave));
            for (std::size_t i = 0; i < prey.size(); ++i)
            {
                Point const p = IntoRoom(spots[i % spots.size()], kPyreBarrow, kPyreRadius, 3.0f);
                if (TempSummon* cinder = SummonAt(me, NPC_PYRE_CINDER, p, TEMPSUMMON_TIMED_OR_CORPSE_DESPAWN, 30000))
                {
                    cinder->AI()->SetGUID(prey[i]->GetGUID());
                    DoCast(prey[i], SPELL_CINDER_MARKED, true);
                    Talk(Text::STOKER_CINDER_WHISPER, prey[i]);
                }
            }
            LOG_INFO("module", "Emberfall barrows: the Last Stoker's cinders ({}, wave {}, instance {})", prey.size(), wave + 1, me->GetInstanceId());
        }

        std::uint32_t _fired = 0;
        std::uint8_t _phase = PHASE_ONE;
        Kit::EnergyBar _heat;
        bool _pyreUp = false;
    };

    // A pyre cinder: crawls after its prey; within reach it bursts (Cinder Burst, 6 yd), stokes the pyre and is gone.
    struct npc_ef_pyre_cinder : public ScriptedAI
    {
        explicit npc_ef_pyre_cinder(Creature* c) : ScriptedAI(c) { }

        void IsSummonedBy(WorldObject* summoner) override
        {
            _stoker = summoner ? summoner->GetGUID() : ObjectGuid::Empty;
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
                if (Creature* stoker = ObjectAccessor::GetCreature(*me, _stoker); stoker && stoker->IsAlive() && stoker->AI())
                    stoker->AI()->DoAction(ACTION_CINDER_BURST);
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

        ObjectGuid _prey, _stoker;
        std::uint32_t _check = 0;
        bool _done = false;
    };

    // ---- B5 the First Interred ---------------------------------------------------------------------------------------
    // Ideas from: Loken's Lightning Nova (Halls of Lightning) and Keli'dan's Burning Nova (the Blood Furnace) for the
    // nova everyone near runs from; Novos the Summoner (Drak'Tharon Keep) and Selin Fireheart's crystals (Magisters'
    // Terrace) for the ward its bearers hold; v2 (docs/research/dungeon-mechanics-midnight.md): Lightwarden Ruia's three
    // phases at 70 and 40 %, the burn-the-shield intermission (Rav'i), timed soaks of several (the Blinding Vale's circles,
    // Degentrius's split soaks) for the tithe, and an arena that closes in (Arcanotron Custos and Lothraxion fill theirs
    // with lasting puddles, Xathuux's rage leaves less than half the room) for the mound falling in.
    struct boss_ef_first_interred : public BossAI
    {
        enum Events : std::uint32_t { EVENT_WEIGHT = 1, EVENT_SENTENCE, EVENT_KNEEL, EVENT_TITHE };

        explicit boss_ef_first_interred(Creature* c) : BossAI(c, BOSS_FIRST_INTERRED) { Companions::SetBossMark(me); }

        void Reset() override
        {
            _Reset();
            me->RemoveAurasDueToSpell(SPELL_GRAVE_WARD);
            me->RemoveAurasDueToSpell(SPELL_EARTHEN_MANTLE);
            me->SetReactState(REACT_AGGRESSIVE);
            Kit::ClearArena(me);
            _wardFired = _fallFired = _closeFired = 0;
            _enraged = false;
            _phase = PHASE_ONE;
            _ward = _fall = Kit::Intermission{};
            _arenaStage = 0;
            _pulse = 0;
            _enrageIn = 0;
            _ring.Clear();
        }

        void JustEngagedWith(Unit* /*who*/) override
        {
            _JustEngagedWith();
            Talk(Text::INTERRED_AGGRO);
            _enrageIn = Timers::kHardEnrageMs;
            EnterPhase(PHASE_ONE);
        }

        void JustDied(Unit* /*killer*/) override
        {
            _JustDied();
            Kit::ClearArena(me);
            Talk(Text::INTERRED_DEATH);
            Report(instance, Dungeons::DATA_FIRST_INTERRED);
        }

        void EnterEvadeMode(EvadeReason why) override
        {
            Kit::ClearArena(me);
            BossAI::EnterEvadeMode(why);
        }

        void DamageTaken(Unit* attacker, std::uint32_t& damage, DamageEffectType type, SpellSchoolMask school) override
        {
            BossAI::DamageTaken(attacker, damage, type, school);
            float const pct = HealthPctAfter(me, damage);
            if (_phase == PHASE_ONE && !CrossedThresholds(Thresholds::kWard, pct, _wardFired).empty())
                Ward();
            else if (_phase == PHASE_TWO && !CrossedThresholds(Thresholds::kMoundFalls, pct, _fallFired).empty())
                MoundFalls();
            else if (_phase == PHASE_THREE && !CrossedThresholds(Thresholds::kCloses, pct, _closeFired).empty())
                Closes();
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
            if (summon->GetEntry() == NPC_WARD_BEARER && Kit::IntermissionAddDied(_ward))
            {
                me->RemoveAurasDueToSpell(SPELL_GRAVE_WARD);
                Talk(Text::INTERRED_WARD_DOWN);
                LOG_INFO("module", "Emberfall barrows: the Grave Ward breaks, phase 2 (instance {})", me->GetInstanceId());
                EnterPhase(PHASE_TWO);
            }
        }

        // An unpaid tithe (the soak marker, EfBossKit.h): a risen servant comes up where it stood.
        void SetData(std::uint32_t id, std::uint32_t value) override
        {
            if (id != Kit::kSoakFailedData || value != NPC_IMPERIAL_TITHE || !me->IsAlive())
                return;
            Talk(Text::INTERRED_TITHE_UNPAID);
            SummonAt(me, NPC_RISEN_SERVANT, _titheAt, TEMPSUMMON_CORPSE_TIMED_DESPAWN, 60000);
        }

        void UpdateAI(std::uint32_t diff) override
        {
            if (!UpdateVictim())
                return;
            if (_enrageIn)
            {
                _enrageIn = _enrageIn > diff ? _enrageIn - diff : 0;
                if (!_enrageIn)
                    DoCastSelf(SPELL_LONG_PATIENCE_ENDS, true);
            }
            if (_fall.active && Kit::StepIntermission(_fall, diff))
                AfterTheFall();
            if (_arenaStage)
            {
                _pulse = _pulse > diff ? _pulse - diff : 0;
                if (!_pulse)
                {
                    _pulse = Timers::kArenaPulseMs;
                    Kit::PulseOutsideArena(me, Flat(kFirstBarrow), ArenaRadius(), SPELL_FALLEN_EARTH);
                }
            }
            events.Update(diff);
            if (me->HasUnitState(UNIT_STATE_CASTING))
                return;
            while (std::uint32_t const id = events.ExecuteEvent())
            {
                ExecuteEvent(id);
                if (me->HasUnitState(UNIT_STATE_CASTING))
                    return;
            }
            if (!_fall.active)
                DoMeleeAttackIfReady();
        }

        void ExecuteEvent(std::uint32_t id) override
        {
            switch (id)
            {
                case EVENT_WEIGHT:
                    DoCastSelf(SPELL_BARROW_WEIGHT, true);
                    events.ScheduleEvent(EVENT_WEIGHT, Ms(_ward.active ? Timers::kWeightWardedMs
                                                          : _phase == PHASE_THREE ? Timers::kWeightArenaMs : Timers::kWeight.min), 0, _phase);
                    break;
                case EVENT_SENTENCE:
                    DoCastVictim(SPELL_PREFECTS_SENTENCE);
                    Again(events, EVENT_SENTENCE, Timers::kSentence, _phase);
                    break;
                case EVENT_KNEEL:
                    Talk(Text::INTERRED_KNEEL_YELL);
                    Talk(Text::INTERRED_KNEEL);
                    DoCastSelf(SPELL_KNEEL);
                    Again(events, EVENT_KNEEL, Timers::kKneel, _phase);
                    break;
                case EVENT_TITHE:
                    Tithe();
                    Again(events, EVENT_TITHE, Timers::kTithe, _phase);
                    break;
            }
        }

    private:
        float ArenaRadius() const { return Kit::ArenaRadius(kFirstRadius, kArenaStages, _arenaStage); }

        // A phase's own events (phase 1: the weight, the sentence, Kneel; 2 and 3 the tithe too).
        void EnterPhase(std::uint8_t phase)
        {
            _phase = phase;
            events.Reset();
            events.SetPhase(phase);
            First(events, EVENT_WEIGHT, Timers::kWeight, phase);
            First(events, EVENT_SENTENCE, Timers::kSentence, phase);
            First(events, EVENT_KNEEL, Timers::kKneel, phase);
            if (phase >= PHASE_TWO)
                First(events, EVENT_TITHE, Timers::kTithe, phase);
        }

        // 70 %: the Grave Ward (no damage taken) while its two bearers at the barrow's edge live; two servants come for
        // the group; the weight faster. It still fights its tank.
        void Ward()
        {
            _phase = PHASE_INTERMISSION;
            events.Reset();
            events.SetPhase(PHASE_INTERMISSION);
            Talk(Text::INTERRED_WARD_YELL);
            Talk(Text::INTERRED_WARD);
            DoCastSelf(SPELL_GRAVE_WARD, true);
            Point const here = Where(me);
            float const side = me->GetOrientation() + float(M_PI) * 0.5f;
            std::uint32_t bearers = 0;
            for (Point p : Ring(kFirstBarrow, kBearerRadius, kBearersPerWard, side))
                if (SummonAt(me, NPC_WARD_BEARER, IntoRoom(p, kFirstBarrow, kFirstRadius, 4.0f), TEMPSUMMON_CORPSE_TIMED_DESPAWN, 30000))
                    ++bearers;
            for (Point p : Ring(here, 8.0f, kServantsPerWard, me->GetOrientation()))
                SummonAt(me, NPC_RISEN_SERVANT, p, TEMPSUMMON_CORPSE_TIMED_DESPAWN, 60000);
            Kit::StartIntermission(_ward, 0, bearers);
            First(events, EVENT_WEIGHT, Timers::kWeight, PHASE_INTERMISSION);
            First(events, EVENT_SENTENCE, Timers::kSentence, PHASE_INTERMISSION);
            LOG_INFO("module", "Emberfall barrows: the Grave Ward rises ({} bearers, instance {})", bearers, me->GetInstanceId());
            if (!bearers)
            {
                me->RemoveAurasDueToSpell(SPELL_GRAVE_WARD);
                EnterPhase(PHASE_TWO);
            }
        }

        // 40 %: the mound falls in. Untouchable for its cast (5 s); then the barrow is a ring of 20 yd round its middle
        // and the fallen earth beyond hurts every second.
        void MoundFalls()
        {
            _phase = PHASE_INTERMISSION;
            events.Reset();
            events.SetPhase(PHASE_INTERMISSION);
            Talk(Text::INTERRED_FALL_YELL);
            Talk(Text::INTERRED_FALL);
            Kit::BeginImmune(me, SPELL_EARTHEN_MANTLE, nullptr);
            DoCastSelf(SPELL_THE_MOUND_FALLS);
            if (TempSummon* ring = Kit::SummonMarker(me, NPC_CLOSING_BARROW, kFirstBarrow.x, kFirstBarrow.y,
                                                        GroundZ(me, kFirstBarrow.x, kFirstBarrow.y, kFirstBarrow.z), SPELL_CLOSING_BARROW, 900000))
                _ring = ring->GetGUID();
            Kit::StartIntermission(_fall, Timers::kMoundFallsMs, 0);
            LOG_INFO("module", "Emberfall barrows: the First Barrow falls in (instance {})", me->GetInstanceId());
        }

        void AfterTheFall()
        {
            Kit::EndImmune(me, SPELL_EARTHEN_MANTLE);
            _arenaStage = 1;
            _pulse = Timers::kArenaPulseMs;
            Kit::SetArena(me, Flat(kFirstBarrow), ArenaRadius());
            EnterPhase(PHASE_THREE);
            LOG_INFO("module", "Emberfall barrows: the arena is {:.0f} yd, phase 3 (instance {})", ArenaRadius(), me->GetInstanceId());
        }

        // 15 %: the barrow closes further.
        void Closes()
        {
            Talk(Text::INTERRED_CLOSE);
            _arenaStage = 2;
            Kit::SetArena(me, Flat(kFirstBarrow), ArenaRadius());
            if (Creature* ring = ObjectAccessor::GetCreature(*me, _ring))
            {
                ring->RemoveAurasDueToSpell(SPELL_CLOSING_BARROW);
                ring->CastSpell(ring, SPELL_CLOSING_BARROW_LAST, true);
            }
            LOG_INFO("module", "Emberfall barrows: the arena closes to {:.0f} yd (instance {})", ArenaRadius(), me->GetInstanceId());
        }

        // The tithe: a circle between it and someone of the group (never under the tank), for two to share.
        void Tithe()
        {
            Unit* who = SomeoneElseOrAnyone(this, 40.0f);
            if (!who)
                return;
            Point p = Towards(Where(me), Where(who), kTitheFromBoss);
            p = IntoRoom(p, kFirstBarrow, _arenaStage ? ArenaRadius() : kFirstRadius, kTitheRadius);
            p.z = GroundZ(me, p.x, p.y, me->GetPositionZ());
            if (Kit::SummonSoak(me, NPC_IMPERIAL_TITHE, p.x, p.y, p.z))
            {
                _titheAt = p;
                Talk(Text::INTERRED_TITHE);
            }
        }

        std::uint32_t _wardFired = 0, _fallFired = 0, _closeFired = 0;
        bool _enraged = false;
        std::uint8_t _phase = PHASE_ONE;
        Kit::Intermission _ward, _fall;
        std::size_t _arenaStage = 0;
        std::uint32_t _pulse = 0, _enrageIn = 0;
        ObjectGuid _ring;
        Point _titheAt{ kFirstBarrow };
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
    new EfBarrowsCreatureScript<npc_ef_keening_mourner>("npc_ef_keening_mourner");
    new EfBarrowsCreatureScript<npc_ef_pyre_cinder>("npc_ef_pyre_cinder");
    new EfBarrowsCreatureScript<npc_ef_ward_bearer>("npc_ef_ward_bearer");

    // What the companions play (EfCompanions.h, through the toolkit): the Post-Keeper's mark (Struck from the Count hits
    // 8 yd round its bearer when it ends), its driven posts (ground casters), the clutch and the tithe (soaks), the cold
    // hearths (a shelter: everyone into one), the furrow (keep clear of the burrowed mother).
    Companions::RegisterRunOut(SPELL_NUMBERED, 8.0f);
    Kit::RegisterGroundCaster(NPC_DRIVEN_POST);
    Kit::RegisterSoakDef(Kit::SoakDef{ NPC_BROOD_CLUTCH, SPELL_BROOD_CLUTCH, kClutchRadius, 1, Timers::kClutchFuseMs,
                                             SPELL_CLUTCH_CRUSHED, kClutchDamage, kClutchFloor, 0 });
    Kit::RegisterSoakDef(Kit::SoakDef{ NPC_IMPERIAL_TITHE, SPELL_IMPERIAL_TITHE, kTitheRadius, kTitheNeeded, Timers::kTitheFuseMs,
                                             SPELL_TITHE_PAID, kTitheDamage, kTitheFloor, SPELL_TITHE_UNPAID });
    Companions::RegisterSoak(NPC_COLD_HEARTH, kHearthRadius, 0);
    Kit::RegisterHazardAura(SPELL_BURROWED, kFurrowRadius);
}
