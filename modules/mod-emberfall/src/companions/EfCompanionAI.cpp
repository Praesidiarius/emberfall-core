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

// Emberfall companions: the AI (creature_template.ScriptName `npc_ef_companion`). One class for every role; the role
// picks the think function, the archetype's spell rows (by priority) the abilities. companions.md §3 has the rules.

#include "CreatureScript.h"
#include "StringFormat.h"
#include "DynamicObject.h"
#include "EfCompanions.h"
#include "Log.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TemporarySummon.h"
#include "ThreatManager.h"

#include <map>

namespace Emberfall::Companions
{
    namespace
    {
        constexpr uint32 kThinkMs = 200;
        constexpr uint32 kWeakenedSoul = 6788;
        constexpr uint32 kAvoidPoint = 0xEFC0;
        constexpr uint32 kMovePoint = 0xEFC1;
        constexpr uint32 kDodgePoint = 0xEFC2;      // out of a telegraph or away from a run-out mark
        constexpr float kMeleeAoeRadius = 8.0f;

        class CompanionAI : public CompanionAIBase
        {
        public:
            explicit CompanionAI(Creature* c) : CompanionAIBase(c) { }

            // ---- setup ----------------------------------------------------------------------------------------------
            void Configure(Archetype const* arch, uint32 index, ObjectGuid owner) override
            {
                _a = *arch;
                _index = index;
                _owner = owner;
                _configured = true;
                std::stable_sort(_a.spells.begin(), _a.spells.end(), [](SpellRow const& x, SpellRow const& y) { return x.priority < y.priority; });
                _ranks.clear();
                _moving = false;
                me->SetReactState(REACT_PASSIVE);
                if (Player* o = Owner())
                    Follow(o, true);
            }

            uint8 ArchetypeId() const override { return _a.id; }

            std::string Debug() const override
            {
                return _why + (_lastFail.empty() ? "" : "; last failed cast " + _lastFail);
            }

            char FrameState() const override
            {
                if (_sitting)
                    return 'E';
                if (me->IsInCombat() || me->GetVictim())
                    return 'C';
                if (Player* o = const_cast<CompanionAI*>(this)->Owner())
                    if (Roster* r = GetRoster(o, false); r && r->stay)
                        return 'S';
                return 'F';
            }

            // ---- engine callbacks: the companion decides itself -------------------------------------------------------
            void MoveInLineOfSight(Unit* /*who*/) override { }
            void AttackStart(Unit* /*who*/) override { }
            void AttackedBy(Unit* /*attacker*/) override { }
            void OwnerAttackedBy(Unit* /*attacker*/) override { }
            void OwnerAttacked(Unit* /*target*/) override { }
            void EnterEvadeMode(EvadeReason /*why*/) override { me->AttackStop(); }
            void JustDied(Unit* /*killer*/) override
            {
                StandUp();
                OnCompanionDied(me);
            }

            void DamageDealt(Unit* victim, uint32& damage, DamageEffectType /*type*/, SpellSchoolMask /*school*/) override
            {
                if (!damage || victim == me)
                    return;
                OnDamage(me, damage);
                // The biggest recent hit (decaying), for the threat cap's look ahead.
                _bigHit = std::max(_bigHit * 0.9f, float(damage) * std::max(_a.threatMod, 1.0f));
                // The tank's threat: (threat_mod - 1) x the damage on top of the normal threat.
                if (_a.threatMod > 1.0f && victim->CanHaveThreatList())
                    victim->GetThreatMgr().AddThreat(me, float(damage) * (_a.threatMod - 1.0f), nullptr, true, true);
            }

            void HealDone(Unit* /*to*/, uint32& heal) override { OnHeal(me, heal); }

            void MovementInform(uint32 type, uint32 id) override
            {
                if (type == POINT_MOTION_TYPE && id == kAvoidPoint)
                    _avoidMs = 0;
                if (type == POINT_MOTION_TYPE && id == kMovePoint)
                    _moving = false;
            }

            // ---- the update ------------------------------------------------------------------------------------------
            void UpdateAI(uint32 diff) override
            {
                if (!_configured)
                    return;
                Player* owner = Owner();
                if (!owner)
                {
                    // The owner left the map (or the game): a guardian left behind goes.
                    if (TempSummon* s = me->ToTempSummon())
                        s->UnSummon();
                    return;
                }
                Tick(diff);
                _think += diff;
                if (_think >= kThinkMs)
                {
                    uint32 const dt = _think;
                    _think = 0;
                    Think(owner, dt);
                }
                if (me->GetVictim() && _a.range <= 0.0f && !_holding)
                    DoMeleeAttackIfReady();
            }

        private:
            Player* Owner() { return ObjectAccessor::GetPlayer(*me, _owner); }

            void Tick(uint32 diff)
            {
                _gcd = _gcd > diff ? _gcd - diff : 0;
                for (auto& [spell, ms] : _cd)
                    ms = ms > diff ? ms - diff : 0;
                _sinceCast += diff;
                _clock += diff;
                if (_avoidMs)
                    _avoidMs = _avoidMs > diff ? _avoidMs - diff : 0;
                if (_dodgeMs)
                    _dodgeMs = _dodgeMs > diff ? _dodgeMs - diff : 0;
            }

            // ---- movement --------------------------------------------------------------------------------------------
            // Point movement only, every destination at the height of the unit it is next to: the core's follow and chase
            // generators recompute their destination from the map's ground, and in an instance without vmaps (the profiles
            // run without them; a dungeon is a WMO with no ground in the map files) a guardian following its owner climbs
            // into the air. A point path keeps the height it is given.
            bool Moving() const { return _moving && !me->movespline->Finalized(); }

            void MoveTo(float x, float y, float z, float tolerance)
            {
                if (me->GetExactDist(x, y, z) <= tolerance)
                    return;
                if (Moving() && _dest.GetExactDist(x, y, z) <= tolerance)
                    return;
                // A path (with the ground's heights) only where the map has ground; else the straight line at this height.
                bool const ground = me->GetMapHeight(x, y, z) > INVALID_HEIGHT;
                me->GetMotionMaster()->MovePoint(kMovePoint, x, y, z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f, ground, true);
                _dest.Relocate(x, y, z);
                _moving = true;
            }

            void Halt()
            {
                if (Moving() || me->isMoving())
                {
                    me->GetMotionMaster()->Clear();
                    me->GetMotionMaster()->MoveIdle();
                    me->StopMoving();
                }
                _moving = false;
            }

            void Follow(Player* owner, bool force = false)
            {
                Roster* r = GetRoster(owner, false);
                if (r && r->stay)
                {
                    if (force)
                        Halt();
                    return;
                }
                if (force)
                    _moving = false;
                Position const at = FormationPoint(owner, _index);
                MoveTo(at.GetPositionX(), at.GetPositionY(), at.GetPositionZ(), 1.5f);
                if (!Moving() && !owner->isMoving() && me->GetExactDist2d(&at) <= 2.0f && std::fabs(me->GetOrientation() - owner->GetOrientation()) > 0.3f)
                    me->SetFacingTo(owner->GetOrientation());
            }

            // A spot `dist` yards from `target` at `angle` (absolute), at the target's height.
            // Burning ground we know of (a hostile dynamic object we stood in, or a telegraphed cast that leaves one):
            // where it is, how far it reaches, until when.
            struct Fire
            {
                float x, y, r;
                uint32 until;
            };

            void RememberFire(float x, float y, float r, uint32 ms)
            {
                std::erase_if(_fires, [this](Fire const& f) { return f.until <= _clock; });
                for (Fire& f : _fires)
                    if (std::hypot(f.x - x, f.y - y) < 1.5f)
                    {
                        f.r = std::max(f.r, r);
                        f.until = std::max(f.until, _clock + ms);
                        return;
                    }
                if (_fires.size() < 8)
                    _fires.push_back(Fire{ x, y, r, _clock + ms });
            }

            // A destination inside known fire is pushed out past its edge (away from its middle): no companion walks
            // back into burning ground, and a tank whose mob stands in it stands outside, so the mob follows it out.
            void OutOfFire(float& x, float& y) const
            {
                for (Fire const& f : _fires)
                {
                    if (f.until <= _clock)
                        continue;
                    float const dx = x - f.x, dy = y - f.y, d = std::hypot(dx, dy);
                    if (d >= f.r + 1.0f)
                        continue;
                    float const a = d > 0.3f ? std::atan2(dy, dx) : me->GetAbsoluteAngle(f.x, f.y) + float(M_PI);
                    x = f.x + std::cos(a) * (f.r + 2.5f);
                    y = f.y + std::sin(a) * (f.r + 2.5f);
                }
            }

            void Approach(Unit* target, float dist, float angle, float tolerance)
            {
                float x = target->GetPositionX() + std::cos(angle) * dist;
                float y = target->GetPositionY() + std::sin(angle) * dist;
                OutOfFire(x, y);
                float const z = target->GetPositionZ() + 0.2f;
                if (!target->IsWithinLOS(x, y, z))
                {
                    MoveTo(target->GetPositionX(), target->GetPositionY(), z, tolerance);
                    return;
                }
                MoveTo(x, y, z, tolerance);
            }

            void Face(Unit* target)
            {
                if (!Moving() && !me->HasInArc(float(M_PI) / 3.0f, target))
                    me->SetFacingToObject(target);
            }

            // Where this role fights from: tanks face the mob (it faces them), melee from behind, ranged at their range
            // (closer without line of sight). Movement only when the current spot does not do.
            bool TakePosition(Unit* target)
            {
                float const reach = target->GetCombatReach() + 1.2f;
                if (_a.role == Role::Tank)
                {
                    if (!me->IsWithinMeleeRange(target))
                    {
                        Approach(target, reach, target->GetAbsoluteAngle(me), 1.0f);
                        return false;
                    }
                    Face(target);
                    return true;
                }
                if (_a.range <= 0.0f)
                {
                    // Behind: within 60 degrees of the target's back. In range but not behind yet still fights (a mob
                    // turning on us).
                    bool const behind = target->isInBackInMap(me, 50.0f, float(M_PI) / 3.0f);
                    bool const inRange = me->IsWithinMeleeRange(target);
                    if (!inRange || !behind)
                        Approach(target, reach, target->GetOrientation() + float(M_PI), 1.0f);
                    else
                        Face(target);
                    return inRange;
                }
                float const dist = me->GetDistance(target);
                if (!me->IsWithinLOSInMap(target))
                {
                    Approach(target, 5.0f, target->GetAbsoluteAngle(me), 2.0f);
                    return false;
                }
                // Out of melee: at least 8 yd (the mob's frontal attacks and cleaves), at most the range. Moving there
                // comes first; a mob hitting us is fought from where we stand.
                if (dist > _a.range - 1.0f || (dist < 8.0f && target->GetVictim() != me))
                {
                    Approach(target, _a.range - 6.0f, target->GetAbsoluteAngle(me), 2.0f);
                    return false;
                }
                Face(target);
                return true;
            }

            void StandUp()
            {
                if (_sitting)
                {
                    me->SetStandState(UNIT_STAND_STATE_STAND);
                    _sitting = false;
                }
            }

            // Out of the ground effects of hostile casters (best effort): a hostile area aura on us that a dynamic
            // object carries (a rain, a pool, a cloud) -> walk out of its radius, away from its center.
            bool AvoidGround(Player* owner)
            {
                if (_avoidMs)
                    return true;
                for (auto const& [id, app] : me->GetAppliedAuras())
                {
                    Aura* aura = app->GetBase();
                    if (aura->GetType() != DYNOBJ_AURA_TYPE || app->IsPositive())
                        continue;
                    Unit* caster = aura->GetCaster();
                    if (!caster || !me->IsHostileTo(caster) || InGroup(owner, caster))
                        continue;
                    DynamicObject* dyn = aura->GetDynobjOwner();
                    if (!dyn)
                        continue;
                    float const radius = dyn->GetRadius();
                    RememberFire(dyn->GetPositionX(), dyn->GetPositionY(), radius, uint32(std::max(dyn->GetDuration(), 0)));
                    float angle = dyn->GetAbsoluteAngle(me);
                    if (dyn->GetExactDist2d(me) < 0.5f)
                        angle = dyn->GetAbsoluteAngle(owner);
                    float x = dyn->GetPositionX() + std::cos(angle) * (radius + 3.0f);
                    float y = dyn->GetPositionY() + std::sin(angle) * (radius + 3.0f);
                    float const z = me->GetPositionZ(); // no ground lookup: see FormationPoint
                    me->InterruptNonMeleeSpells(false);
                    me->GetMotionMaster()->Clear();
                    me->GetMotionMaster()->MovePoint(kAvoidPoint, x, y, z);
                    _moving = false;
                    _avoidMs = 3000;
                    return true;
                }
                return false;
            }

            // ---- boss mechanics ----------------------------------------------------------------------------------------
            // A spot we can walk to: in sight, and on a floor near our height where the map knows its ground.
            bool Walkable(float x, float y, float z) const
            {
                if (!me->IsWithinLOS(x, y, z + 1.0f))
                    return false;
                float const ground = me->GetMapHeight(x, y, z + 3.0f);
                return ground <= INVALID_HEIGHT || std::fabs(ground - z) < 3.5f;
            }

            void Dodge(float x, float y, uint32 holdMs, std::string const& why)
            {
                float const z = me->GetPositionZ();
                me->InterruptNonMeleeSpells(false);
                me->GetMotionMaster()->Clear();
                me->GetMotionMaster()->MovePoint(kDodgePoint, x, y, z);
                _moving = false;
                _dodgeMs = holdMs;
                _why = why;
            }

            // The area a hostile's telegraphed cast will hit (mob-abilities.md: a cone ahead of it, a circle round it, a
            // circle at a point), with the core's own leeway: the effect radius plus both bodies. Nothing for casts that
            // cover a whole room (Keen's 30 yd: those are for interrupting, not dodging).
            bool DangerOf(Unit* caster, Spell* spell, Danger& d) const
            {
                SpellInfo const* info = spell->GetSpellInfo();
                for (SpellEffectInfo const& eff : info->Effects)
                {
                    if (!eff.Effect)
                        continue;
                    float const radius = eff.CalcRadius(caster);
                    if (radius <= 0.0f || radius > 15.0f)
                        continue;
                    uint32 const a = eff.TargetA.GetTarget(), b = eff.TargetB.GetTarget();
                    auto is = [&](uint32 t) { return a == t || b == t; };
                    if (is(TARGET_UNIT_CONE_ENEMY_104) || is(TARGET_UNIT_CONE_ENEMY_54) || is(TARGET_UNIT_CONE_ENEMY_24))
                    {
                        d.shape = DangerShape::Cone;
                        d.origin = Vec2{ caster->GetPositionX(), caster->GetPositionY() };
                        d.radius = radius + caster->GetCombatReach() + me->GetCombatReach();
                        float deg = is(TARGET_UNIT_CONE_ENEMY_24) ? 24.0f : is(TARGET_UNIT_CONE_ENEMY_54) ? 54.0f : 104.0f;
                        if (SpellCone const* sc = sSpellMgr->GetSpellCone(info->Id))
                            deg = float(sc->cone_degrees);
                        d.angle = deg * float(M_PI) / 180.0f;
                        // The caster faces its explicit target from the cast's start (mob-abilities.md §3).
                        Unit* t = spell->m_targets.GetUnitTarget();
                        d.facing = t && t != caster ? caster->GetAbsoluteAngle(t) : caster->GetOrientation();
                        return true;
                    }
                    if (is(TARGET_UNIT_SRC_AREA_ENEMY))
                    {
                        d.shape = DangerShape::Circle;
                        d.origin = Vec2{ caster->GetPositionX(), caster->GetPositionY() };
                        d.radius = radius + me->GetCombatReach();
                        return true;
                    }
                    if (is(TARGET_UNIT_DEST_AREA_ENEMY) || (eff.Effect == SPELL_EFFECT_PERSISTENT_AREA_AURA && spell->m_targets.HasDst()))
                    {
                        d.shape = DangerShape::Circle;
                        if (spell->m_targets.HasDst())
                            d.origin = Vec2{ spell->m_targets.GetDstPos()->GetPositionX(), spell->m_targets.GetDstPos()->GetPositionY() };
                        else if (Unit* t = spell->m_targets.GetUnitTarget())
                            d.origin = Vec2{ t->GetPositionX(), t->GetPositionY() };
                        else
                            continue;
                        d.radius = radius + me->GetCombatReach();
                        return true;
                    }
                }
                return false;
            }

            // Out of a telegraph before its cast ends: the shortest walk to outside its edge (round a cone's side, out of
            // a circle), another direction if a wall is in the way; we hold there until the cast is over.
            bool AvoidTelegraphs(Player* owner, std::vector<Unit*> const& enemies)
            {
                if (_dodgeMs)
                    return true;
                Vec2 const self{ me->GetPositionX(), me->GetPositionY() };
                for (Unit* e : enemies)
                {
                    Spell* spell = e->GetCurrentSpell(CURRENT_GENERIC_SPELL);
                    if (!spell || spell->getState() != SPELL_STATE_PREPARING || spell->GetCastTime() < 1000)
                        continue;
                    int32 const left = spell->GetCastTimeRemaining();
                    if (left <= 150)
                        continue;
                    // A cast that leaves burning ground where it lands (Pyre Ash): remembered, so nobody walks back in.
                    for (SpellEffectInfo const& eff : spell->GetSpellInfo()->Effects)
                    {
                        if (eff.Effect != SPELL_EFFECT_PERSISTENT_AREA_AURA)
                            continue;
                        uint32 const ms = uint32(left + std::max(spell->GetSpellInfo()->GetMaxDuration(), 0));
                        if (eff.TargetA.GetTarget() == TARGET_DEST_CASTER)
                            RememberFire(e->GetPositionX(), e->GetPositionY(), eff.CalcRadius(e), ms);   // round its own feet (Bank the Fires)
                        else if (spell->m_targets.HasDst())
                            RememberFire(spell->m_targets.GetDstPos()->GetPositionX(), spell->m_targets.GetDstPos()->GetPositionY(),
                                         eff.CalcRadius(e), ms);
                    }
                    Danger d;
                    if (!DangerOf(e, spell, d) || !InDanger(d, self, 1.5f))
                        continue;
                    Vec2 const prefer{ owner->GetPositionX(), owner->GetPositionY() };
                    Vec2 to = EscapePoint(d, self, 2.5f, prefer);
                    float const z = me->GetPositionZ();
                    if (!Walkable(to.x, to.y, z))
                    {
                        // Round the danger's middle in steps until a spot is outside and walkable.
                        float const r = Dist(d.origin, to);
                        float const a0 = std::atan2(to.y - d.origin.y, to.x - d.origin.x);
                        bool found = false;
                        for (int i = 1; i <= 8 && !found; ++i)
                            for (float sgn : { 1.0f, -1.0f })
                            {
                                float const a = a0 + sgn * 0.4f * float(i);
                                Vec2 const c{ d.origin.x + std::cos(a) * r, d.origin.y + std::sin(a) * r };
                                if (!InDanger(d, c, 1.0f) && Walkable(c.x, c.y, z))
                                {
                                    to = c;
                                    found = true;
                                    break;
                                }
                            }
                    }
                    Dodge(to.x, to.y, uint32(left) + 300, Acore::StringFormat("out of {}'s {}", e->GetName(), spell->GetSpellInfo()->SpellName[0]));
                    return true;
                }
                return false;
            }

            // Run-out marks (RegisterRunOut): with one on us, walk clear of the group and stay there until it ends; next
            // to someone carrying one, step out of its reach.
            bool RunOut(Player* owner)
            {
                if (RunOuts().empty())
                    return false;
                if (_dodgeMs)
                    return true;
                std::vector<Unit*> const group = GroupUnits(owner, 60.0f);
                Vec2 const self{ me->GetPositionX(), me->GetPositionY() };
                float const z = me->GetPositionZ();
                for (auto const& [spell, radius] : RunOuts())
                {
                    if (Aura const* mark = me->GetAura(spell))
                    {
                        std::vector<Vec2> others;
                        float nearest = 1e9f;
                        for (Unit* u : group)
                            if (u != me && u->IsAlive())
                            {
                                others.push_back(Vec2{ u->GetPositionX(), u->GetPositionY() });
                                nearest = std::min(nearest, Dist(others.back(), self));
                            }
                        uint32 const left = uint32(std::max(mark->GetDuration(), 0));
                        if (nearest > radius + 1.0f)
                        {
                            // Clear: stand still until it ends (never back into the group).
                            if (me->GetVictim())
                                me->AttackStop();
                            _holding = true;
                            _why = Acore::StringFormat("clear of the group with {}", spell);
                            return true;
                        }
                        Vec2 const to = RunOutPoint(self, others, radius, 2.0f, radius + 4.0f,
                                                    [&](Vec2 c) { return Walkable(c.x, c.y, z); });
                        Dodge(to.x, to.y, std::min<uint32>(left, 2500), Acore::StringFormat("running out with {}", spell));
                        return true;
                    }
                    for (Unit* u : group)
                    {
                        if (u == me || !u->HasAura(spell))
                            continue;
                        Vec2 const bearer{ u->GetPositionX(), u->GetPositionY() };
                        if (!NearRunOut(bearer, self, radius, 1.5f))
                            continue;
                        float a = std::atan2(self.y - bearer.y, self.x - bearer.x);
                        if (Dist(bearer, self) < 0.3f)
                            a += float(M_PI) * 0.5f;
                        Vec2 to{ bearer.x + std::cos(a) * (radius + 3.0f), bearer.y + std::sin(a) * (radius + 3.0f) };
                        for (int i = 1; i <= 6 && !Walkable(to.x, to.y, z); ++i)
                        {
                            float const b = a + (i % 2 ? 1.0f : -1.0f) * 0.5f * float((i + 1) / 2);
                            to = Vec2{ bearer.x + std::cos(b) * (radius + 3.0f), bearer.y + std::sin(b) * (radius + 3.0f) };
                        }
                        Dodge(to.x, to.y, 1500, Acore::StringFormat("away from {}'s {}", u->GetName(), spell));
                        return true;
                    }
                }
                return false;
            }

            // A kick for an interruptible cast (the core stops only casts of the silence prevention type with the
            // interrupt flag): one companion per cast (the roster's claims), after a human's reaction, a boss's casts
            // first (while a boss is in the fight the kick waits for it).
            bool TryInterrupt(Player* owner, Roster* roster, std::vector<Unit*> const& enemies)
            {
                SpellRow const* row = nullptr;
                for (SpellRow const& r : _a.spells)
                    if (r.kind == Kind::Interrupt && RankOf(r) && Ready(RankOf(r)))
                    {
                        row = &r;
                        break;
                    }
                if (!row)
                    return false;
                uint32 const id = RankOf(*row);
                SpellInfo const* kick = sSpellMgr->GetSpellInfo(id);
                if (!kick)
                    return false;
                bool bossIn = false;
                for (Unit* e : enemies)
                    if (IsBoss(e))
                        bossIn = true;
                KickRule const rule;
                for (Unit* e : enemies)
                {
                    Spell* spell = e->GetCurrentSpell(CURRENT_GENERIC_SPELL);
                    if (!spell || spell->getState() != SPELL_STATE_PREPARING)
                        continue;
                    SpellInfo const* info = spell->GetSpellInfo();
                    if (!(info->InterruptFlags & SPELL_INTERRUPT_FLAG_INTERRUPT) || !(info->PreventionType & SPELL_PREVENTION_TYPE_SILENCE))
                        continue;
                    int32 const castMs = spell->GetCastTime(), left = spell->GetCastTimeRemaining();
                    if (castMs <= 0 || left <= 0 || !WorthKicking(rule, uint32(castMs), uint32(left), IsBoss(e), bossIn))
                        continue;
                    auto claim = roster->kickClaims.find(e->GetGUID());
                    if (claim != roster->kickClaims.end() && roster->clockMs - claim->second < uint32(castMs))
                        continue;   // someone went for this cast already
                    float const dist = me->GetDistance(e);
                    float const maxRange = kick->GetMaxRange(false, me);
                    if ((maxRange > 0.0f && dist > maxRange) || !me->IsWithinLOSInMap(e))
                        continue;
                    if (me->IsNonMeleeSpellCast(false))
                        me->InterruptNonMeleeSpells(false);
                    uint32 flags = TRIGGERED_IGNORE_GCD | TRIGGERED_IGNORE_SHAPESHIFT | TRIGGERED_IGNORE_EQUIPPED_ITEM_REQUIREMENT |
                                   TRIGGERED_IGNORE_CASTER_AURASTATE;
                    if (_a.freeCost)
                        flags |= TRIGGERED_IGNORE_POWER_AND_REAGENT_COST;
                    if (me->CastSpell(e, id, TriggerCastFlags(flags)) != SPELL_CAST_OK)
                    {
                        _cd[id] = 1000;
                        continue;
                    }
                    roster->kickClaims[e->GetGUID()] = roster->clockMs;
                    _cd[id] = std::max<uint32>({ kick->RecoveryTime, kick->CategoryRecoveryTime, row->cooldownMs, 1000 });
                    _why = Acore::StringFormat("interrupting {}'s {}", e->GetName(), info->SpellName[0]);
                    LOG_DEBUG("module", "Emberfall companions: {} interrupts {}'s {} ({} ms left)", me->GetName(), e->GetName(), info->Id, left);
                    (void)owner;
                    return true;
                }
                return false;
            }

            // ---- casting ---------------------------------------------------------------------------------------------
            uint32 RankOf(SpellRow const& row)
            {
                uint8 const level = me->GetLevel();
                auto key = std::make_pair(row.spell, level);
                auto it = _ranks.find(key);
                if (it != _ranks.end())
                    return it->second;
                uint32 id = 0;
                if (level >= row.minLevel && level <= row.maxLevel)
                    id = RankFor(row.spell, level);
                if (!id && level >= row.minLevel && level <= row.maxLevel)
                    if (SpellInfo const* info = sSpellMgr->GetSpellInfo(row.spell); info && !info->IsRanked())
                        id = row.spell; // an unranked spell (ours, or a creature spell) whatever its SpellLevel
                _ranks[key] = id;
                return id;
            }

            bool Ready(uint32 spell) const
            {
                auto it = _cd.find(spell);
                return it == _cd.end() || it->second == 0;
            }

            // Casts the row's rank at the target if everything allows it now.
            bool TryCast(SpellRow const& row, Unit* target)
            {
                if (_gcd || me->HasUnitState(UNIT_STATE_CASTING) || me->IsNonMeleeSpellCast(false))
                    return false;
                uint32 const id = RankOf(row);
                if (!id || !Ready(id))
                    return false;
                SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
                if (!info)
                    return false;
                if (!_a.freeCost && info->PowerType == POWER_MANA)
                {
                    int32 const cost = info->CalcPowerCost(me, info->GetSchoolMask());
                    if (cost > int32(me->GetPower(POWER_MANA)))
                    {
                        _lastFail = Acore::StringFormat("{} needs {} mana", id, cost);
                        return false;
                    }
                }
                bool const positive = info->IsPositive();
                if (target != me)
                {
                    float const maxRange = info->GetMaxRange(positive, me);
                    float const minRange = info->GetMinRange(positive);
                    float const dist = me->GetDistance(target);
                    if ((maxRange > 0.0f && dist > maxRange) || (minRange > 0.0f && dist < minRange) || !me->IsWithinLOSInMap(target))
                    {
                        _lastFail = Acore::StringFormat("{} out of range ({:.0f} yd)", id, dist);
                        return false;
                    }
                }
                if (info->CalcCastTime(me) > 0 && (me->isMoving() || Moving()))
                {
                    me->GetMotionMaster()->Clear();
                    me->GetMotionMaster()->MoveIdle();
                    me->StopMoving();
                    _moving = false;
                }
                uint32 flags = TRIGGERED_IGNORE_SHAPESHIFT | TRIGGERED_IGNORE_EQUIPPED_ITEM_REQUIREMENT | TRIGGERED_IGNORE_CASTER_AURASTATE;
                if (_a.freeCost)
                    flags |= TRIGGERED_IGNORE_POWER_AND_REAGENT_COST;
                SpellCastResult const res = me->CastSpell(target, id, TriggerCastFlags(flags));
                if (res != SPELL_CAST_OK)
                {
                    _cd[id] = 1500; // try another ability, this one again in a while
                    _lastFail = Acore::StringFormat("{} failed ({})", id, uint32(res));
                    return false;
                }
                _lastCast = id;
                uint32 cd = std::max<uint32>({ info->RecoveryTime, info->CategoryRecoveryTime, row.cooldownMs });
                _cd[id] = cd;
                _gcd = std::max<uint32>(info->StartRecoveryTime ? info->StartRecoveryTime : 1500, 1000);
                if (info->PowerType == POWER_MANA && !_a.freeCost)
                    _sinceCast = 0;
                return true;
            }

            // The rows of the kinds an offensive rotation uses, in priority order, each with its condition.
            bool Rotation(Player* owner, Unit* target, std::vector<Unit*> const& enemies)
            {
                for (SpellRow const& row : _a.spells)
                {
                    bool ok = false;
                    Unit* at = target;
                    switch (row.kind)
                    {
                        case Kind::Threat:
                        case Kind::Nuke:
                        case Kind::Shoot:
                            ok = true;
                            break;
                        case Kind::Behind:
                            ok = target->isInBackInMap(me, 6.0f);
                            break;
                        case Kind::Dot:
                        {
                            uint32 const id = RankOf(row);
                            ok = id && !target->HasAura(id, me->GetGUID());
                            break;
                        }
                        case Kind::Aoe:
                        {
                            uint32 near = 0;
                            for (Unit* e : enemies)
                                if (e->IsWithinDistInMap(target, kMeleeAoeRadius) && !IsCrowdControlled(e))
                                    ++near;
                            ok = near >= std::max<uint8>(row.minTargets, 2);
                            // A cleave or splash would break crowd control next to the target, and puts threat on every
                            // mob it hits: a DPS uses it only when each of them is the tank's, held long enough and well
                            // under the threat cap.
                            Unit* tank = Tank(owner);
                            for (Unit* e : enemies)
                            {
                                if (e == target || !e->IsWithinDistInMap(target, kMeleeAoeRadius))
                                    continue;
                                if (IsCrowdControlled(e))
                                    ok = false;
                                else if (_a.role != Role::Tank &&
                                         (!tank || e->GetVictim() != tank || TankHeldMs(owner, e) < Cfg().gate.waitMs ||
                                          MustHold(Cfg().gate, true, e->GetThreatMgr().GetThreat(me), e->GetThreatMgr().GetThreat(tank), _bigHit)))
                                    ok = false;
                            }
                            break;
                        }
                        case Kind::AoeThreat:
                        {
                            uint32 near = 0;
                            bool cc = false;
                            for (Unit* e : enemies)
                                if (e->IsWithinDistInMap(me, kMeleeAoeRadius))
                                {
                                    if (IsCrowdControlled(e))
                                        cc = true;
                                    else
                                        ++near;
                                }
                            ok = !cc && near >= std::max<uint8>(row.minTargets, 2);
                            at = me;
                            break;
                        }
                        default:
                            break;
                    }
                    if (ok && TryCast(row, at == me && row.kind == Kind::AoeThreat ? target : at))
                        return true;
                }
                return false;
            }

            SpellRow const* FirstOfKind(Kind k, Unit* target)
            {
                for (SpellRow const& row : _a.spells)
                    if (row.kind == k && RankOf(row) && Ready(RankOf(row)))
                    {
                        (void)target;
                        return &row;
                    }
                return nullptr;
            }

            bool CastKind(Kind k, Unit* target)
            {
                for (SpellRow const& row : _a.spells)
                    if (row.kind == k && TryCast(row, target))
                        return true;
                return false;
            }

            // ---- think -------------------------------------------------------------------------------------------------
            void Think(Player* owner, uint32 dt)
            {
                Config const& cfg = Cfg();
                Roster* roster = GetRoster(owner, false);
                if (!roster)
                    return;
                float const distOwner = me->GetDistance(owner);
                std::vector<Unit*> const enemies = EngagedEnemies(owner);
                bool const fight = !enemies.empty();

                // Mobs never aggro on a companion first: out of a fight it is immune to NPCs.
                if (cfg.noProximityAggro)
                {
                    bool const immune = !fight && !me->IsInCombat();
                    if (immune != me->HasUnitFlag(UNIT_FLAG_IMMUNE_TO_NPC))
                    {
                        if (immune)
                            me->SetUnitFlag(UNIT_FLAG_IMMUNE_TO_NPC);
                        else
                            me->RemoveUnitFlag(UNIT_FLAG_IMMUNE_TO_NPC);
                    }
                }

                // Far behind (the owner teleported, a fall, a door): catch up at once, in combat too. Running there would
                // pull everything on the way.
                if (distOwner > cfg.teleportBeyond)
                {
                    me->AttackStop();
                    me->InterruptNonMeleeSpells(false);
                    StandUp();
                    Position const at = FormationPoint(owner, _index);
                    me->NearTeleportTo(at.GetPositionX(), at.GetPositionY(), at.GetPositionZ(), at.GetOrientation());
                    Halt();
                    return;
                }

                if (!fight)
                {
                    _why = "no fight";
                    _holding = false;
                    _idleMs += dt;
                    if (me->GetVictim())
                        me->AttackStop();
                    OutOfCombat(owner, roster, dt);
                    return;
                }
                _idleMs = 0;
                StandUp();
                _resGuid.Clear();

                if (AvoidGround(owner))
                {
                    _why = "avoiding a ground effect";
                    return;
                }
                // Boss mechanics (companions.md §3 "Mechanics"): out of telegraphs, clear of run-out marks, a kick for an
                // interruptible cast.
                if (AvoidTelegraphs(owner, enemies))
                    return;
                if (RunOut(owner))
                    return;
                if (TryInterrupt(owner, roster, enemies))
                    return;

                // The leash: too far from the owner, break off and come back.
                if (distOwner > cfg.leash || (_returning && distOwner > cfg.leash * 0.5f))
                {
                    _returning = true;
                    _why = Acore::StringFormat("leash ({:.0f} yd)", distOwner);
                    me->AttackStop();
                    me->InterruptNonMeleeSpells(false);
                    Follow(owner);
                    return;
                }
                _returning = false;

                switch (_a.role)
                {
                    case Role::Tank: TankThink(owner, enemies); break;
                    case Role::Healer: HealerThink(owner, roster, enemies, true, dt); break;
                    default: DpsThink(owner, roster, enemies); break;
                }
                CombatRegen(dt);
            }

            // A little mana in combat ("spirit"): 0.5 % of max per second, 1.5 % after 5 s without a mana cast.
            void CombatRegen(uint32 dt)
            {
                if (!_a.usesMana || !me->GetMaxPower(POWER_MANA))
                    return;
                _regenAcc += float(me->GetMaxPower(POWER_MANA)) * (_sinceCast >= 5000 ? 0.015f : 0.005f) * float(dt) / 1000.0f;
                if (_regenAcc >= 1.0f)
                {
                    me->ModifyPower(POWER_MANA, int32(_regenAcc));
                    _regenAcc -= float(int32(_regenAcc));
                }
            }

            bool Valid(Player* owner, Unit* mob, std::vector<Unit*> const& enemies) const
            {
                (void)owner;
                return mob && mob->IsAlive() && std::find(enemies.begin(), enemies.end(), mob) != enemies.end() && !IsCrowdControlled(mob);
            }

            // ---- tank -------------------------------------------------------------------------------------------------
            void TankThink(Player* owner, std::vector<Unit*> const& enemies)
            {
                // A loose mob: on someone else of the group. The healer's attacker first, then the most hurt victim's.
                Unit* loose = nullptr;
                float looseScore = -1;
                for (Unit* e : enemies)
                {
                    if (IsCrowdControlled(e))
                        continue;
                    Unit* v = e->GetVictim();
                    if (!v || v == me || !InGroup(owner, v))
                        continue;
                    float score = 100.0f - v->GetHealthPct();
                    if (Creature* vc = v->ToCreature())
                        if (CompanionAIBase* ai = AIOf(vc))
                            if (Archetype const* a = FindArchetype(ai->ArchetypeId()); a && a->role == Role::Healer)
                                score += 100.0f;
                    if (score > looseScore)
                    {
                        loose = e;
                        looseScore = score;
                    }
                }
                Unit* target = nullptr;
                if (loose)
                {
                    // Taunted, it comes to us: switch to it. Without a taunt ready, switch only when we hold nothing
                    // (else the tank would run between two mobs; its area threat works on the loose one meanwhile).
                    bool taunted = false;
                    for (SpellRow const& row : _a.spells)
                        if (row.kind == Kind::Taunt && TryCast(row, loose))
                        {
                            taunted = true;
                            break;
                        }
                    Unit* current = me->GetVictim();
                    if (taunted || !Valid(owner, current, enemies) || current->GetVictim() != me)
                        target = loose;
                }
                if (!target && Valid(owner, me->GetVictim(), enemies))
                    target = me->GetVictim();
                if (!target)
                    if (Unit* sel = owner->GetSelectedUnit(); Valid(owner, sel, enemies))
                        target = sel;
                if (!target)
                {
                    float best = 1e9f;
                    for (Unit* e : enemies)
                        if (!IsCrowdControlled(e) && me->GetDistance(e) < best)
                        {
                            best = me->GetDistance(e);
                            target = e;
                        }
                }
                if (!target)
                {
                    _why = "tank: nothing to hold";
                    return;
                }
                Engage(target, true);
                Rotation(owner, target, enemies);
                _why = Acore::StringFormat("tanking {}{}", target->GetName(), loose ? " (loose)" : "");
            }

            bool Engage(Unit* target, bool melee)
            {
                _holding = false;
                if (me->GetVictim() != target)
                    me->Attack(target, melee);
                return TakePosition(target);
            }

            // ---- DPS --------------------------------------------------------------------------------------------------
            void DpsThink(Player* owner, Roster* roster, std::vector<Unit*> const& enemies)
            {
                Config const& cfg = Cfg();
                Unit* tank = Tank(owner);
                bool const haveTank = tank && tank->IsAlive();
                Unit* target = nullptr;
                // An add that must die (SetKillPriority: a shade on its way, a cinder, a ward-bearer) before anything else,
                // the nearest first.
                float bestPrio = 1e9f;
                for (Unit* e : enemies)
                    if (IsKillPriority(e) && Valid(owner, e, enemies) && me->GetDistance(e) < bestPrio)
                    {
                        bestPrio = me->GetDistance(e);
                        target = e;
                    }
                if (!target && roster->assist)
                {
                    // The owner's target, else the tank's, else what we had, else what attacks a group member.
                    if (owner->IsAlive())
                        if (Unit* sel = owner->GetSelectedUnit(); Valid(owner, sel, enemies))
                            target = sel;
                    if (!target && haveTank && Valid(owner, tank->GetVictim(), enemies))
                        target = tank->GetVictim();
                    if (!target && Valid(owner, ObjectAccessor::GetUnit(*me, _target), enemies))
                        target = ObjectAccessor::GetUnit(*me, _target);
                    if (!target)
                        for (Unit* e : enemies)
                            if (Valid(owner, e, enemies) && e->GetVictim() && InGroup(owner, e->GetVictim()))
                            {
                                target = e;
                                break;
                            }
                }
                else if (!target)
                {
                    // Assist off: only what attacks this companion.
                    for (Unit* e : enemies)
                        if (Valid(owner, e, enemies) && e->GetVictim() == me)
                        {
                            target = e;
                            break;
                        }
                }
                if (!target)
                {
                    _why = Acore::StringFormat("no target ({} engaged)", enemies.size());
                    _holding = false;
                    if (me->GetVictim())
                        me->AttackStop();
                    Follow(owner);
                    return;
                }
                _target = target->GetGUID();

                bool const onMe = target->GetVictim() == me;
                bool const tankHolds = haveTank && target->GetVictim() == tank;
                float const tankThreat = haveTank ? target->GetThreatMgr().GetThreat(tank) : 0.0f;
                uint32 const held = tankHolds ? TankHeldMs(owner, target) : 0;
                bool wait = !onMe && haveTank && !MayStart(cfg.gate, haveTank, held, tankThreat);
                // A mob nobody holds (the tank has not reached it yet): wait for the tank too.
                if (!onMe && haveTank && !tankHolds)
                    wait = true;
                float const myThreat = target->GetThreatMgr().GetThreat(me);
                // A priority add is nobody's to hold: at once, whatever the threat.
                bool const priority = IsKillPriority(target);
                if (priority)
                    wait = false;
                bool const hold = !priority && (wait || MustHold(cfg.gate, tankHolds, myThreat, tankThreat, _bigHit));
                bool const placed = TakePosition(target);
                if (hold)
                {
                    _why = Acore::StringFormat("{} {}: held {} ms, threat {:.0f}/{:.0f}", wait ? "waiting on" : "holding on", target->GetName(), held,
                                               myThreat, tankThreat);
                    if (!_holding)
                    {
                        me->AttackStop();
                        _holding = true;
                    }
                    return;
                }
                _holding = false;
                if (me->GetVictim() != target)
                    me->Attack(target, _a.range <= 0.0f);
                bool const cast = placed && Rotation(owner, target, enemies);
                _why = Acore::StringFormat("attacking {} at {:.0f} yd{}", target->GetName(), me->GetDistance(target), cast ? ", cast" : "");
            }

            // ---- healer -----------------------------------------------------------------------------------------------
            void HealerThink(Player* owner, Roster* roster, std::vector<Unit*> const& enemies, bool fight, uint32 /*dt*/)
            {
                Config const& cfg = Cfg();
                Unit* tank = Tank(owner);
                // Stay in heal range of the tank, out of melee.
                if (fight && tank && tank != me)
                {
                    // 12-32 yd from the tank: in heal range, out of the melee and of most area attacks around it.
                    float const d = me->GetDistance(tank);
                    if (d > cfg.healRange - 8.0f || d < 12.0f || !me->IsWithinLOSInMap(tank))
                        Approach(tank, 18.0f, tank->GetAbsoluteAngle(me), 3.0f);
                }
                if (me->HasUnitState(UNIT_STATE_CASTING) || _gcd)
                    return;

                std::vector<Unit*> const group = GroupUnits(owner, cfg.healRange + 20.0f);
                std::vector<HealTarget> list;
                uint32 const hotRank = FirstRankOf(Kind::Hot);
                uint32 const shieldRank = FirstRankOf(Kind::Shield);
                for (size_t i = 0; i < group.size(); ++i)
                {
                    Unit* u = group[i];
                    HealTarget h;
                    h.id = uint32(i);
                    h.alive = u->IsAlive();
                    h.hpPct = u->GetHealthPct();
                    h.tank = u == tank;
                    h.hasHot = hotRank && u->HasAura(hotRank, me->GetGUID());
                    // Weakened Soul is the lockout; the module keeps its own 15 s too (the stock script that applies it
                    // may want a player caster).
                    auto const shielded = _shieldAt.find(u->GetGUID());
                    h.hasShield = !shieldRank || u->HasAura(kWeakenedSoul) || u->HasAura(shieldRank) ||
                                  (shielded != _shieldAt.end() && _clock - shielded->second < 15000);
                    h.inRange = me->IsWithinDistInMap(u, cfg.healRange) && me->IsWithinLOSInMap(u);
                    list.push_back(h);
                }
                float const manaPct = me->GetMaxPower(POWER_MANA) ? 100.0f * me->GetPower(POWER_MANA) / me->GetMaxPower(POWER_MANA) : 100.0f;
                // A fresh pull (a mob the tank has held for less than the DPS wait): heals draw threat on every mob the
                // healed one fights, so only members in danger get one, and no shields or HoTs yet.
                HealConfig hc;
                bool fresh = false;
                for (Unit* e : enemies)
                {
                    if (TankHeldMs(owner, e) < Cfg().gate.waitMs)
                        fresh = true;
                    // Heals put threat on every mob fighting the healed one: close to the tank's on a mob, the same care.
                    else if (tank && e->GetVictim() == tank && e->GetThreatMgr().GetThreat(me) >= 0.6f * e->GetThreatMgr().GetThreat(tank))
                        fresh = true;
                }
                if (fresh)
                {
                    // The tank is healed as usual (it holds the mobs anyway; the shield draws no threat); the others only
                    // when in danger, and no HoTs (their ticks threaten every mob).
                    hc.bigBelow = 50.0f;
                    hc.smallBelow = 0.0f;
                    hc.hotBelow = 0.0f;
                }
                HealChoice const choice = ChooseHeal(list, manaPct, fight, hc);
                bool danger = false;
                for (HealTarget const& h : list)
                    if (h.alive && h.hpPct < 50.0f)
                        danger = true;
                // A dispel when nobody is in danger, before a top-up (a boss's DoT on someone is worth more than a small
                // heal: companions.md §3 "Mechanics").
                if (!danger && choice.kind != HealKind::Fast && choice.kind != HealKind::Big && TryDispel(group))
                    return;
                if (choice.kind != HealKind::None)
                {
                    Unit* u = group[choice.id];
                    Kind k = Kind::HealSmall;
                    switch (choice.kind)
                    {
                        case HealKind::Fast: k = Kind::HealFast; break;
                        case HealKind::Big: k = Kind::HealBig; break;
                        case HealKind::Small: k = Kind::HealSmall; break;
                        case HealKind::Hot: k = Kind::Hot; break;
                        case HealKind::Shield: k = Kind::Shield; break;
                        default: break;
                    }
                    _why = Acore::StringFormat("heal kind {} on {} ({:.0f} %)", uint32(choice.kind), u->GetName(), u->GetHealthPct());
                    // The tank's heals and fast heals (someone below 35 %) go out whatever the threat (a dead tank is worse
                    // than a taunt); the others wait while they would take a mob off the tank.
                    if (u != tank && k != Kind::HealFast && k != Kind::Shield && !HealThreatSafe(owner, u, k))
                    {
                        _why += " (held back: threat)";
                        return;
                    }
                    if (CastKind(k, u))
                    {
                        if (k == Kind::Shield)
                            _shieldAt[u->GetGUID()] = _clock;
                        return;
                    }
                    // A fast heal that could not go (cooldown, mana): any direct heal.
                    if ((k == Kind::HealFast || k == Kind::HealBig) && (CastKind(Kind::HealBig, u) || CastKind(Kind::HealFast, u) || CastKind(Kind::HealSmall, u)))
                        return;
                    if (!list[choice.id].inRange)
                        Approach(u, 15.0f, u->GetAbsoluteAngle(me), 3.0f);
                }
                // Dispels when nobody is in danger (a heal went for someone below 50 % or could not go).
                if (!danger && TryDispel(group))
                    return;
                // Idle with mana in a fight: a little damage on the tank's target, once the tank has held it.
                if (fight && !danger && manaPct >= 80.0f && tank && tank->GetVictim() && Valid(owner, tank->GetVictim(), enemies) &&
                    TankHeldMs(owner, tank->GetVictim()) >= Cfg().gate.waitMs)
                {
                    bool everyoneTopped = true;
                    for (HealTarget const& h : list)
                        if (h.alive && h.hpPct < 95.0f)
                            everyoneTopped = false;
                    Unit* t = tank->GetVictim();
                    if (everyoneTopped && !MustHold(Cfg().gate, true, t->GetThreatMgr().GetThreat(me), t->GetThreatMgr().GetThreat(tank)))
                        CastKind(Kind::Nuke, t);
                }
                (void)roster;
            }

            // A group member with a debuff our dispel removes (by the dispel effects' types): the dispel at them.
            bool TryDispel(std::vector<Unit*> const& group)
            {
                for (SpellRow const& row : _a.spells)
                {
                    if (row.kind != Kind::Dispel)
                        continue;
                    uint32 const id = RankOf(row);
                    SpellInfo const* info = id ? sSpellMgr->GetSpellInfo(id) : nullptr;
                    if (!info || !Ready(id))
                        continue;
                    uint32 mask = 0;
                    for (SpellEffectInfo const& eff : info->Effects)
                        if (eff.Effect == SPELL_EFFECT_DISPEL)
                            mask |= SpellInfo::GetDispelMask(DispelType(eff.MiscValue));
                    for (Unit* u : group)
                    {
                        if (!u->IsAlive() || !me->IsWithinDistInMap(u, Cfg().healRange))
                            continue;
                        DispelChargesList dl;
                        u->GetDispellableAuraList(me, mask, dl, info);
                        if (!dl.empty() && TryCast(row, u))
                        {
                            _why = Acore::StringFormat("dispelling {}", u->GetName());
                            return true;
                        }
                    }
                }
                return false;
            }

            // Would this heal on `healed` take the healer past 110 % of the tank's threat on a mob the tank holds? Heal
            // threat is half the healing, split over every mob fighting the healed unit (ThreatManager::
            // ForwardThreatForAssistingMe).
            bool HealThreatSafe(Player* owner, Unit* healed, Kind k)
            {
                Unit* tank = Tank(owner);
                if (!tank)
                    return true;
                uint32 const id = FirstRankOf(k);
                SpellInfo const* info = id ? sSpellMgr->GetSpellInfo(id) : nullptr;
                if (!info)
                    return true;
                float amount = 0;
                for (SpellEffectInfo const& eff : info->Effects)
                {
                    if (eff.Effect == SPELL_EFFECT_HEAL)
                        amount += float(eff.CalcValue(me));
                    else if (eff.ApplyAuraName == SPELL_AURA_PERIODIC_HEAL && eff.Amplitude)
                        amount += float(eff.CalcValue(me)) * float(info->GetMaxDuration()) / float(eff.Amplitude);
                }
                auto const& threatened = healed->GetThreatMgr().GetThreatenedByMeList();
                if (threatened.empty() || amount <= 0)
                    return true;
                float const per = 0.5f * amount / float(threatened.size());
                for (auto const& [guid, ref] : threatened)
                {
                    Creature* mob = ref->GetOwner();
                    if (!mob || mob->GetVictim() != tank)
                        continue;
                    if (mob->GetThreatMgr().GetThreat(me) + per >= 1.1f * mob->GetThreatMgr().GetThreat(tank))
                        return false;
                }
                return true;
            }

            uint32 FirstRankOf(Kind k)
            {
                for (SpellRow const& row : _a.spells)
                    if (row.kind == k)
                        if (uint32 id = RankOf(row))
                            return id;
                return 0;
            }

            // ---- between pulls ------------------------------------------------------------------------------------------
            void OutOfCombat(Player* owner, Roster* roster, uint32 dt)
            {
                if (_a.role == Role::Healer && Resurrect(owner, dt))
                    return;
                if (_a.role == Role::Healer && !_sitting && _idleMs >= 1000)
                {
                    // Top the group up before anyone sits (heals and HoTs, no shields out of combat).
                    HealerThink(owner, roster, {}, false, dt);
                    if (me->HasUnitState(UNIT_STATE_CASTING))
                        return;
                }
                RegenRule const rule;
                float const hp = me->GetHealthPct();
                float const mana = me->GetMaxPower(POWER_MANA) ? 100.0f * me->GetPower(POWER_MANA) / me->GetMaxPower(POWER_MANA) : 100.0f;
                bool const usesMana = _a.usesMana && me->GetMaxPower(POWER_MANA);
                if (_sitting)
                {
                    if (me->GetDistance(owner) > 12.0f || RegenDone(rule, hp, mana, usesMana))
                    {
                        StandUp();
                        Follow(owner, true);
                        return;
                    }
                    _regenAcc += dt;
                    if (_regenAcc >= 1000.0f)
                    {
                        _regenAcc -= 1000.0f;
                        me->ModifyHealth(int32(me->GetMaxHealth() * rule.pctPerSecond / 100.0f));
                        if (usesMana)
                            me->ModifyPower(POWER_MANA, int32(me->GetMaxPower(POWER_MANA) * rule.pctPerSecond / 100.0f));
                    }
                    return;
                }
                if (_idleMs >= 1500 && !owner->isMoving() && me->GetDistance(owner) < 8.0f && WantsRegen(rule, hp, mana, usesMana) &&
                    !me->HasUnitState(UNIT_STATE_CASTING))
                {
                    Halt();
                    me->SetStandState(UNIT_STAND_STATE_SIT);
                    _sitting = true;
                    _regenAcc = 0;
                    return;
                }
                Follow(owner);
            }

            // Out of combat: a dead group member back. The owner gets the res spell (its request); a companion is
            // summoned again where it died after a 2.5 s "cast" (a creature corpse cannot take the player spell).
            bool Resurrect(Player* owner, uint32 dt)
            {
                if (_resMs)
                {
                    if (me->HasUnitState(UNIT_STATE_CASTING))
                        return true;
                    _resMs = _resMs > dt ? _resMs - dt : 0;
                    if (!_resMs && _resGuid)
                    {
                        Revive(owner, _resGuid, 0.5f);
                        me->HandleEmoteCommand(EMOTE_ONESHOT_NONE);
                        _resGuid.Clear();
                    }
                    return true;
                }
                if (_idleMs < 2000)
                    return false;
                SpellRow const* res = nullptr;
                for (SpellRow const& row : _a.spells)
                    if (row.kind == Kind::Res && RankOf(row))
                        res = &row;
                if (!res)
                    return false;
                if (owner->isDead() && !owner->HasPlayerFlag(PLAYER_FLAGS_GHOST) && !owner->isResurrectRequested())
                {
                    if (!me->IsWithinDistInMap(owner, 25.0f) || !me->IsWithinLOSInMap(owner))
                    {
                        Approach(owner, 10.0f, owner->GetAbsoluteAngle(me), 3.0f);
                        return true;
                    }
                    StandUp();
                    if (TryCast(*res, owner))
                        return true;
                }
                for (auto const& [guid, pos] : DeadMembers(owner))
                {
                    if (me->GetExactDist(&pos) > 25.0f)
                    {
                        MoveTo(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(), 3.0f);
                        return true;
                    }
                    StandUp();
                    Halt();
                    me->SetFacingTo(me->GetAbsoluteAngle(&pos));
                    me->HandleEmoteCommand(EMOTE_STATE_SPELL_PRECAST);
                    if (SpellInfo const* info = sSpellMgr->GetSpellInfo(RankOf(*res)))
                    {
                        int32 const cost = info->CalcPowerCost(me, info->GetSchoolMask());
                        me->ModifyPower(POWER_MANA, -std::min<int32>(cost, int32(me->GetPower(POWER_MANA))));
                    }
                    _resGuid = guid;
                    _resMs = 2500;
                    return true;
                }
                return false;
            }

            Archetype _a;
            bool _configured = false;
            uint32 _index = 0;
            ObjectGuid _owner;
            ObjectGuid _target, _resGuid;
            Position _dest;
            bool _moving = false;
            uint32 _think = 0, _gcd = 0, _sinceCast = 0, _idleMs = 0, _avoidMs = 0, _resMs = 0, _dodgeMs = 0;
            float _regenAcc = 0, _bigHit = 0;
            bool _sitting = false, _holding = false, _returning = false;
            std::map<uint32, uint32> _cd;
            std::map<std::pair<uint32, uint8>, uint32> _ranks;
            std::string _why = "new", _lastFail;
            uint32 _lastCast = 0, _clock = 0;
            std::map<ObjectGuid, uint32> _shieldAt;
            std::vector<Fire> _fires;
        };
    }

    class npc_ef_companion : public CreatureScript
    {
    public:
        npc_ef_companion() : CreatureScript("npc_ef_companion") { }

        CreatureAI* GetAI(Creature* creature) const override { return new CompanionAI(creature); }
    };
}

void AddEmberfallCompanionAIScripts()
{
    new Emberfall::Companions::npc_ef_companion();
}
