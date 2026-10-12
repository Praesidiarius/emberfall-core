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

// Emberfall class kits (Emberfall docs/design/class-kit-pipeline.md): the hooks and the GM command of
// src/classkit/EfClassKit.h, and the spell scripts class kits bind by name.
//
// Spell scripts: a kit ability names them with `script:` in Emberfall's kit file; the compiler writes the
// spell_script_names rows. Each script is generic and reads its numbers from the spell's own effects, so the data stays
// the place where an ability is tuned. Contracts (Emberfall's classkit/model.py SCRIPTS checks the effects):
//
//   spell_ef_kit_add_stacks          every `dummy` effect with a trigger spell adds its value in stacks of that aura to
//                                    the targets it hits (the mage's Frost Stacks; EfClassKit.h AddStacks)
//   spell_ef_kit_shatter             effect 1 damages the enemies around that carry the stacks; effect 2 (dummy): value =
//                                    stacks shattered per enemy (0: all), misc = the stacking aura, misc B 1 = damage per
//                                    stack; effect 3 (dummy): value = shell per unit, misc = percent of intellect, misc B =
//                                    stacks per unit, trigger = the shell aura (its duration: this spell's, with spell
//                                    mods). Every stack shattered refunds 1 % of max mana. (Icebreak; later Glacial Rupture)
//   spell_ef_kit_absorb_pct_health   the school absorb's amount is its value in percent of max health (Barrier)
//   spell_ef_kit_line_targets        the area targets are only those on a line ahead, effect 3's value yards to each side
//                                    (Frostbound Orb)
//   spell_ef_kit_interrupt_followup  after a successful interrupt, with the talent of effect 2's misc known, effect 2's
//                                    trigger spell hits the target (Unweave + Improved Unweave)
//   spell_ef_kit_rest_regen          a periodic dummy aura: out of combat, its value in percent of max mana per tick (the
//                                    mage's mana rule)

#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "EfClassKit.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "WorldScript.h"

#include <cmath>
#include <map>

using namespace Acore::ChatCommands;

namespace
{
    class emberfall_classkit_world : public WorldScript
    {
    public:
        emberfall_classkit_world() : WorldScript("emberfall_classkit_world",
            { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED }) { }

        void OnAfterConfigLoad(bool reload) override { Emberfall::ClassKit::ReadConfig(reload); }

        void OnBeforeWorldInitialized() override { Emberfall::ClassKit::Load(); }
    };

    class emberfall_classkit_player : public PlayerScript
    {
    public:
        emberfall_classkit_player() : PlayerScript("emberfall_classkit_player", {
            PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LEVEL_CHANGED }) { }

        void OnPlayerLogin(Player* player) override { Emberfall::ClassKit::OnLogin(player); }

        void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override { Emberfall::ClassKit::OnLevelChanged(player); }
    };

    // `.efkit` (dump) and `.efkit purge`: the selected player's class kit, and the purge run now.
    class emberfall_classkit_commands : public CommandScript
    {
    public:
        emberfall_classkit_commands() : CommandScript("emberfall_classkit_commands") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable kitTable =
            {
                { "dump",  HandleDump,  SEC_GAMEMASTER, Console::No },
                { "purge", HandlePurge, SEC_GAMEMASTER, Console::No },
                { "",      HandleDump,  SEC_GAMEMASTER, Console::No },
            };
            static ChatCommandTable commandTable =
            {
                { "efkit", kitTable },
            };
            return commandTable;
        }

        static Player* Target(ChatHandler* handler)
        {
            Player* target = handler->getSelectedPlayerOrSelf();
            if (!target)
            {
                handler->SendSysMessage("No player selected.");
                handler->SetSentErrorMessage(true);
            }
            return target;
        }

        static bool HandleDump(ChatHandler* handler)
        {
            Player* target = Target(handler);
            if (!target)
                return false;
            std::vector<std::string> lines;
            Emberfall::ClassKit::Dump(target, lines);
            for (std::string const& line : lines)
                handler->SendSysMessage(line);
            return true;
        }

        static bool HandlePurge(ChatHandler* handler)
        {
            Player* target = Target(handler);
            if (!target)
                return false;
            std::vector<std::string> removed;
            uint32 const n = Emberfall::ClassKit::Purge(target, &removed);
            handler->PSendSysMessage("Class kit purge: {} taken away.", n);
            for (std::string const& line : removed)
                handler->SendSysMessage("  " + line);
            return true;
        }
    };
}

// Every dummy effect with a trigger spell adds its value in stacks of that aura to each target it hits.
class spell_ef_kit_add_stacks : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_add_stacks);

    void HandleDummy(SpellEffIndex effIndex)
    {
        SpellEffectInfo const& eff = GetSpellInfo()->Effects[effIndex];
        if (!eff.TriggerSpell || !GetHitUnit())
            return;
        Emberfall::ClassKit::AddStacks(GetCaster(), GetHitUnit(), eff.TriggerSpell, std::max(1, GetEffectValue()));
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_ef_kit_add_stacks::HandleDummy, EFFECT_ALL, SPELL_EFFECT_DUMMY);
    }
};

// Shatters the stacks of the enemies around: damage, stacks taken, mana back, the shell.
class spell_ef_kit_shatter : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_shatter);

    uint32 _shattered = 0;

    bool Validate(SpellInfo const* spellInfo) override
    {
        return spellInfo->Effects[EFFECT_1].MiscValue && ValidateSpellInfo({ spellInfo->Effects[EFFECT_2].TriggerSpell });
    }

    uint32 StackSpell() { return uint32(GetSpellInfo()->Effects[EFFECT_1].MiscValue); }
    int32 PerEnemy() { return GetSpellInfo()->Effects[EFFECT_1].CalcValue(GetCaster()); }

    // Only enemies that carry enough stacks (at least the stacks shattered per enemy, or one).
    void FilterTargets(std::list<WorldObject*>& targets)
    {
        Unit* caster = GetCaster();
        uint32 const need = uint32(std::max(1, PerEnemy()));
        targets.remove_if([&](WorldObject* object)
        {
            Unit* unit = object->ToUnit();
            Aura* aura = unit ? unit->GetAura(StackSpell(), caster->GetGUID()) : nullptr;
            return !aura || aura->GetStackAmount() < need;
        });
    }

    void HandleHit()
    {
        Unit* target = GetHitUnit();
        if (!target)
            return;
        int32 const per = PerEnemy();
        uint32 const taken = Emberfall::ClassKit::TakeStacks(GetCaster(), target, StackSpell(), per > 0 ? uint32(per) : 0);
        if (GetSpellInfo()->Effects[EFFECT_1].MiscValueB == 1 && taken > 1)
            SetHitDamage(GetHitDamage() * int32(taken));
        _shattered += taken;
    }

    void Finish()
    {
        Unit* caster = GetCaster();
        if (!caster || !_shattered)
            return;
        // the refund: 1 % of max mana per stack
        int32 const mana = int32(caster->GetMaxPower(POWER_MANA) * _shattered / 100);
        if (mana > 0)
            caster->EnergizeBySpell(caster, GetSpellInfo()->Id, uint32(mana), POWER_MANA);
        // the shell: per unit of stacks, flat + percent of intellect
        SpellEffectInfo const& shell = GetSpellInfo()->Effects[EFFECT_2];
        int32 const perUnit = std::max(1, shell.MiscValueB);
        int32 const units = int32(_shattered) / perUnit;
        if (units <= 0)
            return;
        int32 const add = units * (shell.CalcValue(caster) + int32(caster->GetStat(STAT_INTELLECT) * shell.MiscValue / 100.0f));
        int32 duration = GetSpellInfo()->GetDuration();
        if (Player* player = caster->ToPlayer())
            player->ApplySpellMod(GetSpellInfo()->Id, SPELLMOD_DURATION, duration);
        Emberfall::ClassKit::AddToShell(caster, shell.TriggerSpell, add, duration);
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_ef_kit_shatter::FilterTargets, EFFECT_0, TARGET_UNIT_SRC_AREA_ENEMY);
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_ef_kit_shatter::FilterTargets, EFFECT_1, TARGET_UNIT_SRC_AREA_ENEMY);
        OnHit += SpellHitFn(spell_ef_kit_shatter::HandleHit);
        AfterCast += SpellCastFn(spell_ef_kit_shatter::Finish);
    }
};

// A school absorb of its value in percent of max health (the value already carries the caster's spell mods).
class spell_ef_kit_absorb_pct_health : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_absorb_pct_health);

    void CalcAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& canBeRecalculated)
    {
        canBeRecalculated = false;
        if (Unit* caster = GetCaster())
            amount = int32(caster->CountPctFromMaxHealth(amount));
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_ef_kit_absorb_pct_health::CalcAmount, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
    }
};

// The area targets on a line straight ahead of the caster only (a projectile flying forward).
class spell_ef_kit_line_targets : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_line_targets);

    void FilterTargets(std::list<WorldObject*>& targets)
    {
        Unit* caster = GetCaster();
        float half = float(GetSpellInfo()->Effects[EFFECT_2].CalcValue(caster));
        if (half <= 0.0f)
            half = 3.0f;
        float const o = caster->GetOrientation();
        float const c = std::cos(o), s = std::sin(o);
        targets.remove_if([&](WorldObject* object)
        {
            float const dx = object->GetPositionX() - caster->GetPositionX();
            float const dy = object->GetPositionY() - caster->GetPositionY();
            float const along = dx * c + dy * s;
            float const across = -dx * s + dy * c;
            float const reach = object->ToUnit() ? object->ToUnit()->GetCombatReach() : 0.0f;
            return along < -reach || std::fabs(across) > half + reach;
        });
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_ef_kit_line_targets::FilterTargets, EFFECT_0, TARGET_UNIT_SRC_AREA_ENEMY);
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_ef_kit_line_targets::FilterTargets, EFFECT_1, TARGET_UNIT_SRC_AREA_ENEMY);
    }
};

// After an interrupt that stopped a cast, with the enabling talent known: the follow-up spell on the target.
class spell_ef_kit_interrupt_followup : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_interrupt_followup);

    bool _wasCasting = false;

    void Before(SpellMissInfo missInfo)
    {
        Unit* target = GetHitUnit();
        _wasCasting = missInfo == SPELL_MISS_NONE && target && target->IsNonMeleeSpellCast(false, false, true);
    }

    void After()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        SpellEffectInfo const& eff = GetSpellInfo()->Effects[EFFECT_1];
        if (_wasCasting && caster && target && eff.TriggerSpell && caster->HasAura(uint32(eff.MiscValue)))
            caster->CastSpell(target, eff.TriggerSpell, true);
    }

    void Register() override
    {
        BeforeHit += BeforeSpellHitFn(spell_ef_kit_interrupt_followup::Before);
        AfterHit += SpellHitFn(spell_ef_kit_interrupt_followup::After);
    }
};

// Out of combat, the aura's value in percent of max mana per tick.
class spell_ef_kit_rest_regen : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_rest_regen);

    void Tick(AuraEffect const* aurEff)
    {
        Unit* target = GetTarget();
        if (!target || target->IsInCombat() || !target->GetMaxPower(POWER_MANA))
            return;
        int32 const gain = int32(target->GetMaxPower(POWER_MANA) * aurEff->GetAmount() / 100);
        if (gain > 0)
            target->ModifyPower(POWER_MANA, gain);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_ef_kit_rest_regen::Tick, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

void AddEmberfallClassKitScripts()
{
    new emberfall_classkit_world();
    new emberfall_classkit_player();
    new emberfall_classkit_commands();
    RegisterSpellScript(spell_ef_kit_add_stacks);
    RegisterSpellScript(spell_ef_kit_shatter);
    RegisterSpellScript(spell_ef_kit_absorb_pct_health);
    RegisterSpellScript(spell_ef_kit_line_targets);
    RegisterSpellScript(spell_ef_kit_interrupt_followup);
    RegisterSpellScript(spell_ef_kit_rest_regen);
}
