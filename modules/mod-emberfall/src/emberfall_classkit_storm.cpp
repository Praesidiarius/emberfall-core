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

// Emberfall class kits, the Druid Stormbringer's storm loop (Emberfall docs/design/classes/druid-stormbringer.md §13,
// 2026-10-27). Generic like the scripts of src/emberfall_classkit.cpp: each reads its numbers from the spell's own
// effects (Emberfall's classkit/scripts_storm.py checks the contracts):
//
//   spell_ef_kit_stacking_field   a ground field (effect 1 a persistent area aura with periodic damage) whose fields stack:
//                                 the engine lets one field's aura tick on a unit at a time, so each tick strikes as often
//                                 as the caster's fields of that spell (alive, by their duration with spell mods) cover the
//                                 unit (Hailwind)
//   spell_ef_kit_lengthen_marks   a dummy effect on the caster: misc = an aura of the caster's on enemies, value = ms, misc B
//                                 = yards: every enemy within misc B yards of the caster carrying it has it that much
//                                 longer (Thunderhead lengthens every Stormbrand)
//   spell_ef_kit_cast_while_moving an aura: while it holds, its owner casts spells with a cast time while moving (the core
//                                 asks, EmberfallCastWhileMoving.h, PATCHES.md #25; Windborne's buff while Windstride holds)
//                                 and keeps a channel going on the move; with a class mask on its first aura effect
//                                 (`affects:`) only the spells it names (the Corsair's Mobile Tempest: Whirlwind Assault)

#include "EfClassKit.h"
#include "EfClassKitLogic.h"

#include "CellImpl.h"
#include "DataMap.h"
#include "EmberfallCastWhileMoving.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "Unit.h"

#include <algorithm>
#include <list>
#include <vector>

namespace
{
    namespace Logic = Emberfall::ClassKit::Logic;

    uint64 NowMs()
    {
        return uint64(GameTime::GetGameTimeMS().count());
    }

    // ---- stacking fields ------------------------------------------------------------------------------------------------

    struct StormField
    {
        uint32 spell = 0;
        uint32 map = 0;
        uint32 instance = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f, radius = 0.0f;
        uint64 endMs = 0;
    };

    // Per caster: its fields still on the ground.
    struct StormFields : public DataMap::Base
    {
        std::vector<StormField> list;
    };

    constexpr char const* kFieldsKey = "ef_storm_fields";

    void Forget(StormFields* fields, uint64 now)
    {
        fields->list.erase(std::remove_if(fields->list.begin(), fields->list.end(),
            [now](StormField const& f) { return f.endMs <= now; }), fields->list.end());
    }

    uint32 Covering(Unit* caster, Unit const* target, uint32 spell)
    {
        StormFields* fields = caster->CustomData.Get<StormFields>(kFieldsKey);
        if (!fields || fields->list.empty())
            return 0;
        uint64 const now = NowMs();
        Forget(fields, now);
        uint32 n = 0;
        for (StormField const& f : fields->list)
            if (f.spell == spell && f.map == target->GetMapId() && f.instance == target->GetInstanceId()
                && Logic::InField(target->GetPositionX() - f.x, target->GetPositionY() - f.y, target->GetPositionZ() - f.z,
                                  f.radius + target->GetCombatReach()))
                ++n;
        return n;
    }

    // ---- casting on the move ----------------------------------------------------------------------------------------------

    // The holds of the owner's auras: `holds` for every spell (Windborne), `masked` for the spells of a class mask only (the
    // Corsair's Mobile Tempest: Whirlwind Assault), one entry per aura.
    struct MovingCasts : public DataMap::Base
    {
        struct Masked
        {
            uint32 family = 0;
            flag96 mask;
        };
        uint32 holds = 0;
        std::vector<Masked> masked;
    };

    constexpr char const* kMovingKey = "ef_cast_while_moving";

    bool CanCastMoving(Unit const* caster, SpellInfo const* spell)
    {
        if (!caster)
            return false;
        MovingCasts const* m = caster->CustomData.Get<MovingCasts>(kMovingKey);
        if (!m)
            return false;
        if (m->holds > 0)
            return true;
        if (!spell)
            return false;
        for (MovingCasts::Masked const& entry : m->masked)
            if (spell->SpellFamilyName == entry.family && (spell->SpellFamilyFlags & entry.mask))
                return true;
        return false;
    }
}

class spell_ef_kit_stacking_field : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_stacking_field);

    void Record()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;
        Spell* spell = GetSpell();
        Position pos;
        if (spell->m_targets.HasDst())
            pos = spell->m_targets.GetDstPos()->GetPosition();
        else if (Unit* target = GetExplTargetUnit())
            pos = target->GetPosition();
        else
            return;
        SpellInfo const* info = GetSpellInfo();
        int32 duration = info->GetMaxDuration();
        if (Player* modOwner = caster->GetSpellModOwner())
            modOwner->ApplySpellMod(info->Id, SPELLMOD_DURATION, duration);
        if (duration <= 0)
            return;
        StormFields* fields = caster->CustomData.GetDefault<StormFields>(kFieldsKey);
        uint64 const now = NowMs();
        Forget(fields, now);
        StormField f;
        f.spell = info->Id;
        f.map = caster->GetMapId();
        f.instance = caster->GetInstanceId();
        f.x = pos.GetPositionX();
        f.y = pos.GetPositionY();
        f.z = pos.GetPositionZ();
        f.radius = info->Effects[EFFECT_0].CalcRadius(caster, spell);
        // the dynamic object lives a little past its last tick
        f.endMs = now + uint64(duration) + 250;
        fields->list.push_back(f);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_ef_kit_stacking_field::Record);
    }
};

class spell_ef_kit_lengthen_marks : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_lengthen_marks);

    void Lengthen()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;
        for (SpellEffectInfo const& eff : GetSpellInfo()->Effects)
        {
            if (eff.Effect != SPELL_EFFECT_DUMMY || eff.TargetA.GetTarget() != TARGET_UNIT_CASTER || eff.MiscValue <= 0)
                continue;
            uint32 const mark = uint32(eff.MiscValue);
            int32 const addMs = eff.CalcValue(caster);
            float const radius = float(std::max(1, eff.MiscValueB));
            if (addMs <= 0)
                continue;
            std::list<Unit*> near;
            Acore::AnyUnfriendlyUnitInObjectRangeCheck check(caster, caster, radius);
            Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(caster, near, check);
            Cell::VisitObjects(caster, searcher, radius);
            for (Unit* unit : near)
            {
                Aura* aura = unit->IsAlive() ? unit->GetAura(mark, caster->GetGUID()) : nullptr;
                if (!aura || aura->GetDuration() <= 0)
                    continue;
                int32 const total = aura->GetDuration() + addMs;
                aura->SetMaxDuration(std::max(aura->GetMaxDuration(), total));
                aura->SetDuration(total);
            }
        }
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_ef_kit_lengthen_marks::Lengthen);
    }
};

class spell_ef_kit_cast_while_moving : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_cast_while_moving);

    // the effect's class mask (`affects:`) limits it to those spells (Mobile Tempest); none: every spell (Windborne)
    void Apply(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
    {
        Unit* owner = GetUnitOwner();
        if (!owner)
            return;
        MovingCasts* m = owner->CustomData.GetDefault<MovingCasts>(kMovingKey);
        SpellEffectInfo const& eff = GetSpellInfo()->Effects[aurEff->GetEffIndex()];
        if (eff.SpellClassMask)
            m->masked.push_back({ GetSpellInfo()->SpellFamilyName, eff.SpellClassMask });
        else
            ++m->holds;
    }

    void Remove(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
    {
        Unit* owner = GetUnitOwner();
        MovingCasts* m = owner ? owner->CustomData.Get<MovingCasts>(kMovingKey) : nullptr;
        if (!m)
            return;
        SpellEffectInfo const& eff = GetSpellInfo()->Effects[aurEff->GetEffIndex()];
        if (eff.SpellClassMask)
        {
            uint32 const family = GetSpellInfo()->SpellFamilyName;
            auto it = std::find_if(m->masked.begin(), m->masked.end(), [&](MovingCasts::Masked const& e)
            {
                return e.family == family && e.mask == eff.SpellClassMask;
            });
            if (it != m->masked.end())
                m->masked.erase(it);
        }
        else if (m->holds > 0)
            --m->holds;
    }

    void Register() override
    {
        // once per aura: on its first aura effect
        uint8 first = EFFECT_0;
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(m_scriptSpellId))
            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                if (info->Effects[i].IsAura())
                {
                    first = i;
                    break;
                }
        AfterEffectApply += AuraEffectApplyFn(spell_ef_kit_cast_while_moving::Apply, first, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_ef_kit_cast_while_moving::Remove, first, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

class EmberfallStormUnitScript : public UnitScript
{
public:
    EmberfallStormUnitScript() : UnitScript("EmberfallStormUnitScript", true, { UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK }) { }

    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage, SpellInfo const* spellInfo) override
    {
        if (!target || !attacker || !spellInfo || !damage)
            return;
        uint32 const n = Covering(attacker, target, spellInfo->Id);
        if (n > 1)
            damage = Logic::StackedFieldDamage(damage, n);
    }
};

void AddEmberfallClassKitStormScripts()
{
    Emberfall::CastWhileMoving::SetCheck(&CanCastMoving);
    RegisterSpellScript(spell_ef_kit_stacking_field);
    RegisterSpellScript(spell_ef_kit_lengthen_marks);
    RegisterSpellScript(spell_ef_kit_cast_while_moving);
    new EmberfallStormUnitScript();
}
