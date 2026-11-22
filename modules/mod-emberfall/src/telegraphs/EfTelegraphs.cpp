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

// Telegraphed casts on the server: see EfTelegraphs.h.

#include "EfTelegraphs.h"

#include "DBCFileLoader.h"
#include "Log.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Unit.h"
#include "World.h"

#include <mutex>
#include <unordered_map>

namespace Emberfall::Telegraphs
{
    namespace
    {
        struct Entry
        {
            Marker marker;
            bool directDamage = false;
        };

        std::once_flag s_once;
        std::unordered_map<uint32, Entry> s_rows;
        uint32 s_avoidable = 0;

        // EfSpellTelegraph.dbc: id, shape, angle (f), width (f), style, flags, color (Emberfall dbc/tables.py).
        void Load()
        {
            std::string const path = sWorld->GetDataPath() + "dbc/EfSpellTelegraph.dbc";
            DBCFileLoader dbc;
            if (!dbc.Load(path.c_str(), "niffiii"))
            {
                LOG_INFO("server.loading", ">> Emberfall telegraphs: no {} (stock data): no telegraphed spell", path);
                return;
            }
            for (uint32 i = 0; i < dbc.GetNumRows(); ++i)
            {
                DBCFileLoader::Record row = dbc.getRecord(i);
                Entry e;
                e.marker.shape = row.getUInt(1);
                e.marker.angle = row.getFloat(2);
                e.marker.width = row.getFloat(3);
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(row.getUInt(0)))
                    for (SpellEffectInfo const& eff : info->Effects)
                        if (eff.Effect == SPELL_EFFECT_SCHOOL_DAMAGE)
                            e.directDamage = true;
                s_avoidable += e.directDamage ? 1 : 0;
                s_rows.emplace(row.getUInt(0), e);
            }
            LOG_INFO("server.loading", ">> Emberfall telegraphs: {} telegraphed spells, {} with direct damage", s_rows.size(), s_avoidable);
        }

        Entry const* Find(uint32 spellId)
        {
            std::call_once(s_once, Load);
            auto it = s_rows.find(spellId);
            return it == s_rows.end() ? nullptr : &it->second;
        }
    }

    Marker const* MarkerOf(uint32 spellId)
    {
        Entry const* e = Find(spellId);
        return e ? &e->marker : nullptr;
    }

    uint32 AvoidableSpells()
    {
        std::call_once(s_once, Load);
        return s_avoidable;
    }

    bool TelegraphedWithDamage(uint32 spellId)
    {
        Entry const* e = Find(spellId);
        return e && e->directDamage;
    }

    CastFacts FactsOf(Spell const* spell)
    {
        CastFacts c;
        if (!spell || spell->getState() != SPELL_STATE_PREPARING)
            return c;
        Unit const* caster = spell->GetCaster() ? spell->GetCaster()->ToUnit() : nullptr;
        c.byCreature = caster && caster->IsCreature();
        c.castMs = uint32(std::max(spell->GetCastTime(), 0));
        c.triggered = spell->IsTriggered();
        if (!c.byCreature || c.castMs < kMinWindupMs || c.triggered)
            return c;  // the cheap facts first: most casts end here
        Entry const* e = Find(spell->GetSpellInfo()->Id);
        c.telegraph = e != nullptr;
        c.directDamage = e && e->directDamage;
        return c;
    }

    bool DangerOf(Unit const* caster, Spell const* spell, float victimReach, float maxRadius, Danger& d)
    {
        SpellInfo const* info = spell->GetSpellInfo();
        Marker const* marker = MarkerOf(info->Id);
        for (SpellEffectInfo const& eff : info->Effects)
        {
            if (!eff.Effect)
                continue;
            float const radius = eff.CalcRadius(const_cast<Unit*>(caster));
            if (radius <= 0.0f || radius > maxRadius)
                continue;
            uint32 const a = eff.TargetA.GetTarget(), b = eff.TargetB.GetTarget();
            auto is = [&](uint32 t) { return a == t || b == t; };
            if (is(TARGET_UNIT_CONE_ENEMY_104) || is(TARGET_UNIT_CONE_ENEMY_54) || is(TARGET_UNIT_CONE_ENEMY_24))
            {
                d.origin = Vec2{ caster->GetPositionX(), caster->GetPositionY() };
                d.radius = radius + caster->GetCombatReach() + victimReach;
                // The caster faces its explicit target from the cast's start (mob-abilities.md §3).
                Unit const* t = spell->m_targets.GetUnitTarget();
                d.facing = t && t != caster ? caster->GetAbsoluteAngle(t) : caster->GetOrientation();
                if (info->HasAttribute(SPELL_ATTR0_CU_CONE_LINE) || (marker && marker->shape == 4))
                {
                    // a line (mob-abilities.md §2): the core's Position::HasInLine, the caster's and our size sideways
                    d.shape = DangerShape::Line;
                    d.width = std::max(marker ? marker->width * 0.5f : 0.0f, caster->GetObjectSize()) + victimReach;
                    return true;
                }
                d.shape = DangerShape::Cone;
                float deg = is(TARGET_UNIT_CONE_ENEMY_24) ? 24.0f : is(TARGET_UNIT_CONE_ENEMY_54) ? 54.0f : 104.0f;
                if (SpellCone const* sc = sSpellMgr->GetSpellCone(info->Id))
                    deg = float(sc->cone_degrees);
                d.angle = deg * float(M_PI) / 180.0f;
                return true;
            }
            if (is(TARGET_UNIT_SRC_AREA_ENEMY))
            {
                d.shape = DangerShape::Circle;
                d.origin = Vec2{ caster->GetPositionX(), caster->GetPositionY() };
                d.radius = radius + victimReach;
                return true;
            }
            if (is(TARGET_UNIT_DEST_AREA_ENEMY) || (eff.Effect == SPELL_EFFECT_PERSISTENT_AREA_AURA && spell->m_targets.HasDst()))
            {
                d.shape = DangerShape::Circle;
                if (spell->m_targets.HasDst())
                    d.origin = Vec2{ spell->m_targets.GetDstPos()->GetPositionX(), spell->m_targets.GetDstPos()->GetPositionY() };
                else if (Unit const* t = spell->m_targets.GetUnitTarget())
                    d.origin = Vec2{ t->GetPositionX(), t->GetPositionY() };
                else
                    continue;
                d.radius = radius + victimReach;
                return true;
            }
        }
        return false;
    }
}
