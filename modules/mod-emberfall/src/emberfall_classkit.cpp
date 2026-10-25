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
//                                    the targets it hits (the mage's Frost Stacks, the Executioner's Openings; EfClassKit.h
//                                    AddStacks), then the hit bonuses (those on the caster once a cast)
//   spell_ef_kit_shatter             effect 1 damages the enemies around that carry the stacks; effect 2 (dummy): value =
//                                    stacks shattered per enemy (0: all), misc = the stacking aura, misc B 1 = damage per
//                                    stack; effect 3 (dummy): value = shell stacks per unit, misc B = stacks shattered per
//                                    unit (summed over every enemy, rounded down), trigger = the stacked shell aura
//                                    (EfClassKit.h AddShellStacks; its duration: this spell's, with spell mods). Every
//                                    stack shattered refunds 1 % of max mana and runs the stack-cooldown passives
//                                    (Icebreak, Glacial Rupture)
//   spell_ef_kit_shell               the stacked shell aura (effect 1 school absorb; effect 2 dummy: percent of max health
//                                    per stack; StackAmount: the most stacks): its stacks follow the pool as blows wear it
//                                    down (EfClassKit.h FollowShell; Frost Shell)
//   spell_ef_kit_stack_cooldown      a passive: effect 1 (dummy): misc = the stacking aura, value = ms off the cooldown of
//                                    its trigger spell per stack shattered; effect 2 (dummy): value = at most that many
//                                    seconds off per cooldown (EfClassKit.h CooldownByStacks; Winter's Embrace on Hoarstorm)
//   spell_ef_kit_absorb_pct_health   the school absorb's amount is its value in percent of max health (Barrier)
//   spell_ef_kit_line_targets        the area targets are only those on a line ahead, effect 3's value yards to each side
//                                    (Frostbound Orb)
//   spell_ef_kit_interrupt_followup  after a successful interrupt, with the talent of effect 2's misc known, effect 2's
//                                    trigger spell hits the target (Unweave + Improved Unweave)
//   spell_ef_kit_rest_regen          a periodic dummy aura: out of combat, its value in percent of max mana per tick (the
//                                    mage's mana rule)
//   spell_ef_kit_conditional         a passive whose effect 1 is a periodic dummy (the check, every period): misc = an aura
//                                    the owner must have (0: none), misc B 1 = only out of combat; effects 2 and 3 count only
//                                    while the condition holds, else their amount is 0 (Light Step out of combat; Iceplate
//                                    and Cryo Ward while a Frost Shell is up)
//   spell_ef_kit_ward_stacks         effect 1 is a school absorb that takes every hit of its schools in full while it lasts;
//                                    effect 2 (dummy): every attacker it absorbed a hit from gains value stacks of its trigger
//                                    aura (Glacial Bulwark: physical immunity that keeps threat, attackers gain Frost Stacks)
//   spell_ef_kit_cheat_death         a passive school absorb that only acts on a hit that would kill (after the other
//                                    shields): effect 2 (dummy): trigger = the spell then cast on oneself (its absorb takes
//                                    from that hit), misc = the cooldown marker aura cast with it (Last Ward: Barrier, 3 min)
//   spell_ef_kit_interrupt_reset     after a successful interrupt, with the talent of effect 3's misc known, the cooldown of
//                                    effect 3's trigger spell is reset (Unweave + Spellweaver: Flicker)
//   spell_ef_kit_stack_freeze        an area spell: effect 1 (dummy): misc = the stacking aura, misc B = the stacks needed,
//                                    trigger = cast at every enemy with that many (not at bosses: the freeze); effect 2
//                                    (dummy): trigger = cast at every enemy with that many, bosses too (the mark); effect 3
//                                    (dummy): with the talent of its misc known, enemies below the threshold gain value
//                                    stacks of its trigger (Stillpoint + Deepening Still). The stacks stay.
//
// Built for the Paladin's first kit (Emberfall docs/design/classes/paladin-wrathbringer.md: Sunblades, stacks of a self
// aura; since 2026-10-21 Crown of Blades raises its swords with spell_ef_kit_self_stacks):
//
//   spell_ef_kit_self_stacks         every dummy effect on the caster with a trigger raises its value in stacks of that
//                                    self aura (misc B > 0: one per enemy struck, at most misc B, plus value - 1; nothing if
//                                    the enemy effects struck nobody), up to the aura's cap (EfClassKit.h AddSelfStacks)
//   spell_ef_kit_delayed_strike      effect 1 (dummy at an enemy): value ms later the trigger is cast where the enemy stands
//                                    then, or on its last spot if it is gone (Heavenfall, Heaven's Answer)
//   spell_ef_kit_not_bosses          never on bosses: the cast at one is refused, bosses leave the area (Kneel, Noonday)
//   spell_ef_kit_talent_buff         an aura whose dummy effects with misc = a talent the owner has keep their trigger on the
//                                    owner while it lasts (Unbowed + Unbowed Grace)
//   spell_ef_kit_extend_aura         effect 1 (dummy): misc = a caster aura, value = ms longer, misc B = at most that many
//                                    seconds beyond its duration (Endless Crown)
//   spell_ef_kit_hit_bonuses         the caster's hit bonuses for this spell on every enemy it strikes (Heavenfall's strike)
//
// The Druid's (a spec's own power, Static; end bonuses, charges, limited targets) are in src/emberfall_classkit_power.cpp,
// the Warlock's in src/emberfall_classkit_heal.cpp, the Pyromancer's Heat in src/emberfall_classkit_heat.cpp, the Wrathbringer's cards (2026-10-21: splash, Crown of Blades'
// launches, the Blessing's main stat and its empowerment, the execute, cooldown cuts) in src/emberfall_classkit_wrath.cpp.
// spell_ef_kit_spend_stacks (the Sunblades' spenders) went with the Sunblades on 2026-10-21 (in git history).
//
// Hit bonuses (EfClassKit.h HitBonuses) need no script of their own: spell_ef_kit_add_stacks and spell_ef_kit_shatter run
// the caster's talents that name the spell that hit (Splintering Cold, Driving Snow, Whiteout, Frozen Grip, Improved
// Glacial Rupture); spell_ef_kit_shatter also lengthens a running storm (Hoarstorm: a periodic trigger aura with a dummy
// effect, misc = the stacking aura, value = ms longer per stack shattered, misc B = at most that many seconds longer;
// EfClassKit.h ExtendByStacks, no script of its own).

#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "EfClassKit.h"
#include "EfClassKitLogic.h"
#include "ObjectAccessor.h"
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

    // `.efkit` (dump), `.efkit purge` and `.efkit auras`: the selected player's class kit, the purge run now, the kit auras
    // on the selected unit.
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
                { "auras", HandleAuras, SEC_GAMEMASTER, Console::No },
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

        // `.efkit auras`: the class kit auras on the selected unit (or oneself), for the live checks.
        static bool HandleAuras(ChatHandler* handler)
        {
            Unit* target = handler->getSelectedUnit();
            if (!target)
                target = handler->GetSession()->GetPlayer();
            std::vector<std::string> lines;
            Emberfall::ClassKit::DumpAuras(target, handler->GetSession()->GetPlayer(), lines);
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

    bool _selfDone = false;

    void HandleDummy(SpellEffIndex effIndex)
    {
        SpellEffectInfo const& eff = GetSpellInfo()->Effects[effIndex];
        if (!eff.TriggerSpell || !GetHitUnit())
            return;
        uint32 const stacks = Emberfall::ClassKit::AddStacks(GetCaster(), GetHitUnit(), eff.TriggerSpell, std::max(1, GetEffectValue()));
        // the bonuses on the caster once a cast, with the first enemy (an area spell rolls Perfect Execution once)
        Emberfall::ClassKit::HitBonuses(GetCaster(), GetHitUnit(), GetSpellInfo()->Id, stacks, !_selfDone);
        _selfDone = true;
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
    uint32 _had = 0;

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
        Aura const* aura = target->GetAura(StackSpell(), GetCaster()->GetGUID());
        uint32 const had = aura ? aura->GetStackAmount() : 0;
        uint32 const taken = Emberfall::ClassKit::TakeStacks(GetCaster(), target, StackSpell(), per > 0 ? uint32(per) : 0);
        if (GetSpellInfo()->Effects[EFFECT_1].MiscValueB == 1 && taken > 1)
            SetHitDamage(GetHitDamage() * int32(taken));
        _shattered += taken;
        _had = had;
    }

    // the talents that name this spell, at the stacks the enemy carried (Improved Glacial Rupture), once the damage is in
    void AfterHitTarget()
    {
        if (Unit* target = GetHitUnit())
            Emberfall::ClassKit::HitBonuses(GetCaster(), target, GetSpellInfo()->Id, _had);
    }

    void Finish()
    {
        Unit* caster = GetCaster();
        if (!caster || !_shattered)
            return;
        // a running storm lasts longer (Hoarstorm)
        Emberfall::ClassKit::ExtendByStacks(caster, StackSpell(), _shattered);
        // the refund: 1 % of max mana per stack
        int32 const mana = int32(caster->GetMaxPower(POWER_MANA) * _shattered / 100);
        if (mana > 0)
            caster->EnergizeBySpell(caster, GetSpellInfo()->Id, uint32(mana), POWER_MANA);
        // cooldowns shortened per stack (Winter's Embrace)
        Emberfall::ClassKit::CooldownByStacks(caster, StackSpell(), _shattered);
        // the shell: value stacks per misc B stacks shattered, summed over every enemy
        SpellEffectInfo const& shell = GetSpellInfo()->Effects[EFFECT_2];
        int32 const gain = Emberfall::ClassKit::Logic::ShellGain(_shattered, std::max(1, shell.MiscValueB), shell.CalcValue(caster));
        if (gain <= 0)
            return;
        int32 duration = GetSpellInfo()->GetDuration();
        if (Player* player = caster->ToPlayer())
            player->ApplySpellMod(GetSpellInfo()->Id, SPELLMOD_DURATION, duration);
        Emberfall::ClassKit::AddShellStacks(caster, shell.TriggerSpell, gain, duration);
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_ef_kit_shatter::FilterTargets, EFFECT_0, TARGET_UNIT_SRC_AREA_ENEMY);
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_ef_kit_shatter::FilterTargets, EFFECT_1, TARGET_UNIT_SRC_AREA_ENEMY);
        OnHit += SpellHitFn(spell_ef_kit_shatter::HandleHit);
        AfterHit += SpellHitFn(spell_ef_kit_shatter::AfterHitTarget);
        AfterCast += SpellCastFn(spell_ef_kit_shatter::Finish);
    }
};

// A school absorb of its value in percent of its owner's max health (the value already carries the caster's spell mods;
// the owner is the caster for every self shield; the Oathkeeper's Sunwall shields each party member by its own health).
class spell_ef_kit_absorb_pct_health : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_absorb_pct_health);

    void CalcAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& canBeRecalculated)
    {
        canBeRecalculated = false;
        if (Unit* owner = GetUnitOwner() ? GetUnitOwner() : GetCaster())
            amount = int32(owner->CountPctFromMaxHealth(amount));
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

    // AzerothCore interrupts in the launch phase (EffectInterruptCast), before the hit: the target's cast is seen here
    void Cast()
    {
        Unit* target = GetExplTargetUnit();
        _wasCasting = target && target->IsNonMeleeSpellCast(false, false, true);
    }

    void Before(SpellMissInfo missInfo)
    {
        if (missInfo != SPELL_MISS_NONE)
            _wasCasting = false;
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
        OnCast += SpellCastFn(spell_ef_kit_interrupt_followup::Cast);
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

// Effects 2 and 3 count only while effect 1's condition holds (an aura on the owner, out of combat).
class spell_ef_kit_conditional : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_conditional);

    bool Holds()
    {
        Unit* owner = GetUnitOwner();
        SpellEffectInfo const& check = GetSpellInfo()->Effects[EFFECT_0];
        if (!owner)
            return false;
        if (check.MiscValueB == 1 && owner->IsInCombat())
            return false;
        if (check.MiscValue && !owner->HasAura(uint32(check.MiscValue)))
            return false;
        return true;
    }

    void CalcAmount(AuraEffect const* aurEff, int32& amount, bool& canBeRecalculated)
    {
        canBeRecalculated = true;
        if (aurEff->GetEffIndex() != EFFECT_0 && !Holds())
            amount = 0;
    }

    void Tick(AuraEffect const* /*aurEff*/)
    {
        bool const holds = Holds();
        for (uint8 i = EFFECT_1; i < MAX_SPELL_EFFECTS; ++i)
            if (AuraEffect* eff = GetAura()->GetEffect(i))
                if ((eff->GetAmount() != 0) != holds)
                    eff->RecalculateAmount(GetUnitOwner());
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_ef_kit_conditional::CalcAmount, EFFECT_ALL, SPELL_AURA_ANY);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_ef_kit_conditional::Tick, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// The stacked shell: its stacks follow the pool as blows wear it down (the gains: spell_ef_kit_shatter).
class spell_ef_kit_shell : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_shell);

    // the pool is set by AddShellStacks, never recalculated from the spell
    void CalcAmount(AuraEffect const* /*aurEff*/, int32& /*amount*/, bool& canBeRecalculated)
    {
        canBeRecalculated = false;
    }

    void AfterAbsorb(AuraEffect* /*aurEff*/, DamageInfo& /*dmgInfo*/, uint32& absorbAmount)
    {
        Emberfall::ClassKit::FollowShell(GetAura(), int32(absorbAmount));
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_ef_kit_shell::CalcAmount, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
        AfterEffectAbsorb += AuraEffectAbsorbFn(spell_ef_kit_shell::AfterAbsorb, EFFECT_0);
    }
};

// A stack-cooldown passive: its numbers are read by EfClassKit.h CooldownByStacks (called by spell_ef_kit_shatter); the
// script only names it (Load finds the passives by it).
class spell_ef_kit_stack_cooldown : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_stack_cooldown);

    bool Validate(SpellInfo const* spellInfo) override
    {
        return spellInfo->Effects[EFFECT_0].MiscValue && ValidateSpellInfo({ spellInfo->Effects[EFFECT_0].TriggerSpell });
    }

    void Register() override { }
};

// Every hit of the absorb's schools is taken in full; the attacker gains stacks.
class spell_ef_kit_ward_stacks : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_ward_stacks);

    void CalcAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        amount = -1; // unlimited: Absorb decides
    }

    void Absorb(AuraEffect* /*aurEff*/, DamageInfo& dmgInfo, uint32& absorbAmount)
    {
        absorbAmount = dmgInfo.GetDamage();
        Unit* owner = GetUnitOwner();
        Unit* attacker = dmgInfo.GetAttacker();
        SpellEffectInfo const& stacks = GetSpellInfo()->Effects[EFFECT_1];
        if (owner && attacker && attacker != owner && attacker->IsAlive() && stacks.TriggerSpell && owner->IsValidAttackTarget(attacker))
            Emberfall::ClassKit::AddStacks(owner, attacker, stacks.TriggerSpell, std::max(1, stacks.CalcValue(owner)));
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_ef_kit_ward_stacks::CalcAmount, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
        OnEffectAbsorb += AuraEffectAbsorbFn(spell_ef_kit_ward_stacks::Absorb, EFFECT_0);
    }
};

// On a hit that would kill (after the other shields), once per cooldown: the trigger spell (a shield) on oneself, and
// that shield takes from the hit.
class spell_ef_kit_cheat_death : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_cheat_death);

    void CalcAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        amount = -1; // unlimited: Absorb decides
    }

    void Absorb(AuraEffect* aurEff, DamageInfo& dmgInfo, uint32& absorbAmount)
    {
        absorbAmount = 0;
        Unit* owner = GetUnitOwner();
        SpellEffectInfo const& eff = GetSpellInfo()->Effects[EFFECT_1];
        uint32 const marker = uint32(eff.MiscValue);
        if (!owner || !eff.TriggerSpell || (marker && owner->HasAura(marker)))
            return;
        // what the other shields can still take of this hit
        uint32 others = 0;
        for (AuraEffect const* shield : owner->GetAuraEffectsByType(SPELL_AURA_SCHOOL_ABSORB))
            if (shield != aurEff && shield->GetAmount() > 0 && (shield->GetMiscValue() & dmgInfo.GetSchoolMask()))
                others += uint32(shield->GetAmount());
        uint32 const damage = dmgInfo.GetDamage();
        if (damage <= others || damage - others < owner->GetHealth())
            return;
        owner->CastSpell(owner, eff.TriggerSpell, true);
        if (marker)
            owner->CastSpell(owner, marker, true);
        // the new shield is not in this hit's list of absorbs: it takes its part here
        Aura* shield = owner->GetAura(eff.TriggerSpell, owner->GetGUID());
        if (!shield)
            return;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            AuraEffect* s = shield->GetEffect(i);
            if (!s || s->GetAuraType() != SPELL_AURA_SCHOOL_ABSORB || s->GetAmount() <= 0)
                continue;
            uint32 const take = std::min(uint32(s->GetAmount()), damage);
            absorbAmount = take;
            if (uint32(s->GetAmount()) > take)
                s->ChangeAmount(s->GetAmount() - int32(take));
            else
                shield->Remove(AURA_REMOVE_BY_ENEMY_SPELL);
            break;
        }
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_ef_kit_cheat_death::CalcAmount, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
        OnEffectAbsorb += AuraEffectAbsorbFn(spell_ef_kit_cheat_death::Absorb, EFFECT_0);
    }
};

// After an interrupt that stopped a cast, with the enabling talent known: another spell's cooldown is reset.
class spell_ef_kit_interrupt_reset : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_interrupt_reset);

    bool _wasCasting = false;

    // AzerothCore interrupts in the launch phase (EffectInterruptCast), before the hit: the target's cast is seen here
    void Cast()
    {
        Unit* target = GetExplTargetUnit();
        _wasCasting = target && target->IsNonMeleeSpellCast(false, false, true);
    }

    void Before(SpellMissInfo missInfo)
    {
        if (missInfo != SPELL_MISS_NONE)
            _wasCasting = false;
    }

    void After()
    {
        Player* caster = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        SpellEffectInfo const& eff = GetSpellInfo()->Effects[EFFECT_2];
        if (_wasCasting && caster && eff.TriggerSpell && caster->HasAura(uint32(eff.MiscValue)) && caster->HasSpellCooldown(eff.TriggerSpell))
            caster->RemoveSpellCooldown(eff.TriggerSpell, true);
    }

    void Register() override
    {
        OnCast += SpellCastFn(spell_ef_kit_interrupt_reset::Cast);
        BeforeHit += BeforeSpellHitFn(spell_ef_kit_interrupt_reset::Before);
        AfterHit += SpellHitFn(spell_ef_kit_interrupt_reset::After);
    }
};

// The enemies around with enough stacks are frozen (not bosses) and marked (bosses too); with the talent, the others
// gain stacks. The stacks stay.
class spell_ef_kit_stack_freeze : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_stack_freeze);

    bool Enough(Unit* target)
    {
        SpellEffectInfo const& eff = GetSpellInfo()->Effects[EFFECT_0];
        Aura const* aura = target->GetAura(uint32(eff.MiscValue), GetCaster()->GetGUID());
        return aura && aura->GetStackAmount() >= uint32(std::max(1, eff.MiscValueB));
    }

    void HandleEffect(SpellEffIndex effIndex)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        SpellEffectInfo const& eff = GetSpellInfo()->Effects[effIndex];
        if (!caster || !target || !eff.TriggerSpell)
            return;
        switch (effIndex)
        {
            case EFFECT_0: // the freeze
                if (Enough(target) && !Emberfall::ClassKit::IsBoss(target))
                    caster->CastSpell(target, eff.TriggerSpell, true);
                break;
            case EFFECT_1: // the mark
                if (Enough(target))
                    caster->CastSpell(target, eff.TriggerSpell, true);
                break;
            default: // below the threshold, with the talent: stacks
                if (!Enough(target) && eff.MiscValue && caster->HasAura(uint32(eff.MiscValue)))
                    Emberfall::ClassKit::AddStacks(caster, target, eff.TriggerSpell, std::max(1, GetEffectValue()));
                break;
        }
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_ef_kit_stack_freeze::HandleEffect, EFFECT_ALL, SPELL_EFFECT_DUMMY);
    }
};

// A builder of self stacks (the Paladin's Sunblades): every dummy effect on the caster with a trigger raises stacks of
// that self aura once the cast is done: its value (misc B above 0: one per enemy struck, at most misc B, plus value - 1);
// nothing when the spell's enemy effects struck nobody. The caster's hit bonuses for this spell run on every enemy
// struck (Twin Brand).
class spell_ef_kit_self_stacks : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_self_stacks);

    uint32 _hits = 0;
    bool _first = true;

    bool NeedsHit()
    {
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            SpellEffectInfo const& eff = GetSpellInfo()->Effects[i];
            if (eff.Effect && eff.TargetA.GetTarget() != TARGET_UNIT_CASTER)
                return true;
        }
        return false;
    }

    void HandleHit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || target == caster)
            return;
        ++_hits;
    }

    void AfterHitTarget()
    {
        // a shot a summoned helper fires for its caster (Thornspitter's thorn) is the caster's: its bonuses too
        Unit* caster = GetOriginalCaster() ? GetOriginalCaster() : GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || target == caster)
            return;
        Emberfall::ClassKit::HitBonuses(caster, target, GetSpellInfo()->Id, 0, _first);
        _first = false;
    }

    void Finish()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;
        bool const needsHit = NeedsHit();
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            SpellEffectInfo const& eff = GetSpellInfo()->Effects[i];
            if (eff.Effect != SPELL_EFFECT_DUMMY || !eff.TriggerSpell || eff.TargetA.GetTarget() != TARGET_UNIT_CASTER)
                continue;
            int32 const gain = Emberfall::ClassKit::Logic::BuilderGain(eff.CalcValue(caster), eff.MiscValueB, _hits, needsHit);
            if (gain > 0)
                Emberfall::ClassKit::AddSelfStacks(caster, eff.TriggerSpell, gain);
        }
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_ef_kit_self_stacks::HandleHit);
        AfterHit += SpellHitFn(spell_ef_kit_self_stacks::AfterHitTarget);
        AfterCast += SpellCastFn(spell_ef_kit_self_stacks::Finish);
    }
};

// A delayed strike (Heavenfall): effect 1 (dummy at an enemy): value ms after the hit the caster casts the trigger at the
// enemy's spot when it falls (a ground strike: dest_dest + an area); its last spot if the enemy died or left.
class spell_ef_kit_delayed_strike : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_delayed_strike);

    bool Validate(SpellInfo const* spellInfo) override
    {
        return ValidateSpellInfo({ spellInfo->Effects[EFFECT_0].TriggerSpell });
    }

    void HandleDummy(SpellEffIndex effIndex)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        uint32 const strike = GetSpellInfo()->Effects[effIndex].TriggerSpell;
        if (!caster || !target || !strike)
            return;
        // The strike follows the enemy (Nathanael, 2026-10-23: "heavenfall should follow the target not stand still"): it
        // lands where the enemy stands when it falls; on the spot it last stood on if it died, left or changed map.
        float const x = target->GetPositionX(), y = target->GetPositionY(), z = target->GetPositionZ();
        ObjectGuid const targetGuid = target->GetGUID();
        int32 const delay = std::max(0, GetEffectValue());
        // the event lives in the caster's own queue: it goes with the caster
        caster->m_Events.AddEventAtOffset([caster, targetGuid, x, y, z, strike]()
        {
            if (!caster->IsInWorld())
                return;
            if (Unit* now = ObjectAccessor::GetUnit(*caster, targetGuid);
                now && now->IsInWorld() && now->IsAlive() && now->GetMapId() == caster->GetMapId())
                caster->CastSpell(now->GetPositionX(), now->GetPositionY(), now->GetPositionZ(), strike, true);
            else
                caster->CastSpell(x, y, z, strike, true);
        }, Milliseconds(delay));
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_ef_kit_delayed_strike::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

// Never on bosses (Kneel, Noonday): a boss as the explicit target refuses the cast; bosses leave the area targets.
class spell_ef_kit_not_bosses : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_not_bosses);

    SpellCastResult CheckCast()
    {
        Unit* target = GetExplTargetUnit();
        if (target && Emberfall::ClassKit::IsBoss(target))
            return SPELL_FAILED_IMMUNE;
        return SPELL_CAST_OK;
    }

    void FilterTargets(std::list<WorldObject*>& targets)
    {
        targets.remove_if([](WorldObject* object) { return Emberfall::ClassKit::IsBoss(object->ToUnit()); });
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_ef_kit_not_bosses::CheckCast);
        // The area filter only on spells that have an enemy area (Noonday); a single-target hold (Kneel) has only the
        // cast check, else the core logs that the hook matches no effect.
        // An area round the caster (Noonday) or round the target (the Druid's Stormpeal: dest area).
        bool area = false, destArea = false;
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(m_scriptSpellId))
            for (SpellEffectInfo const& effect : info->Effects)
            {
                area = area || effect.TargetA.GetTarget() == TARGET_UNIT_SRC_AREA_ENEMY ||
                       effect.TargetB.GetTarget() == TARGET_UNIT_SRC_AREA_ENEMY;
                destArea = destArea || effect.TargetA.GetTarget() == TARGET_UNIT_DEST_AREA_ENEMY ||
                           effect.TargetB.GetTarget() == TARGET_UNIT_DEST_AREA_ENEMY;
            }
        if (area)
            OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_ef_kit_not_bosses::FilterTargets, EFFECT_ALL, TARGET_UNIT_SRC_AREA_ENEMY);
        if (destArea)
            OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_ef_kit_not_bosses::FilterTargets, EFFECT_ALL, TARGET_UNIT_DEST_AREA_ENEMY);
    }
};

// While the aura lasts, every dummy effect of it with misc = a talent the owner has and a trigger keeps that trigger
// (a buff) on the owner (Unbowed + Unbowed Grace: the heal while it holds).
class spell_ef_kit_talent_buff : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_talent_buff);

    void Apply(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
    {
        Unit* owner = GetUnitOwner();
        SpellEffectInfo const& eff = GetSpellInfo()->Effects[aurEff->GetEffIndex()];
        if (owner && eff.TriggerSpell && eff.MiscValue && owner->HasAura(uint32(eff.MiscValue)))
            owner->CastSpell(owner, eff.TriggerSpell, true);
    }

    void Remove(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
    {
        Unit* owner = GetUnitOwner();
        SpellEffectInfo const& eff = GetSpellInfo()->Effects[aurEff->GetEffIndex()];
        if (owner && eff.TriggerSpell && eff.MiscValue)
            owner->RemoveAurasDueToSpell(eff.TriggerSpell, owner->GetGUID());
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_ef_kit_talent_buff::Apply, EFFECT_ALL, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_ef_kit_talent_buff::Remove, EFFECT_ALL, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Lengthens one of the caster's own auras (Endless Crown on Crown of Blades): effect 1 (dummy on the caster): misc = the
// aura, value = ms longer, misc B = at most that many seconds beyond its duration (EfClassKit.h Lengthen).
class spell_ef_kit_extend_aura : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_extend_aura);

    void HandleDummy(SpellEffIndex effIndex)
    {
        Unit* caster = GetCaster();
        SpellEffectInfo const& eff = GetSpellInfo()->Effects[effIndex];
        if (caster && eff.MiscValue)
            Emberfall::ClassKit::Lengthen(caster, uint32(eff.MiscValue), GetEffectValue(), eff.MiscValueB);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_ef_kit_extend_aura::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

// The caster's hit bonuses for this spell on every enemy it strikes, for a spell with no stack script of its own
// (Heavenfall's strike: Marked by Heaven's mark).
class spell_ef_kit_hit_bonuses : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_hit_bonuses);

    bool _first = true;

    void AfterHitTarget()
    {
        // a shot a summoned helper fires for its caster (Thornspitter's thorn) is the caster's: its bonuses too
        Unit* caster = GetOriginalCaster() ? GetOriginalCaster() : GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || target == caster)
            return;
        Emberfall::ClassKit::HitBonuses(caster, target, GetSpellInfo()->Id, 0, _first);
        _first = false;
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_ef_kit_hit_bonuses::AfterHitTarget);
    }
};

void AddEmberfallClassKitPowerScripts();
void AddEmberfallClassKitHealScripts();
void AddEmberfallClassKitWrathScripts();
void AddEmberfallClassKitHeatScripts();
void AddEmberfallClassKitOathScripts();
void AddEmberfallClassKitHuntScripts();
void AddEmberfallClassKitExecScripts();
void AddEmberfallClassKitBloomScripts();
void AddEmberfallClassKitSunScripts();
void AddEmberfallClassKitFlowScripts();
void AddEmberfallClassKitSealScripts();

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
    RegisterSpellScript(spell_ef_kit_conditional);
    RegisterSpellScript(spell_ef_kit_shell);
    RegisterSpellScript(spell_ef_kit_stack_cooldown);
    RegisterSpellScript(spell_ef_kit_ward_stacks);
    RegisterSpellScript(spell_ef_kit_cheat_death);
    RegisterSpellScript(spell_ef_kit_interrupt_reset);
    RegisterSpellScript(spell_ef_kit_stack_freeze);
    RegisterSpellScript(spell_ef_kit_self_stacks);
    RegisterSpellScript(spell_ef_kit_delayed_strike);
    RegisterSpellScript(spell_ef_kit_not_bosses);
    RegisterSpellScript(spell_ef_kit_talent_buff);
    RegisterSpellScript(spell_ef_kit_extend_aura);
    RegisterSpellScript(spell_ef_kit_hit_bonuses);
    AddEmberfallClassKitPowerScripts(); // the Druid's: src/emberfall_classkit_power.cpp
    AddEmberfallClassKitHealScripts();  // the Warlock's: src/emberfall_classkit_heal.cpp
    AddEmberfallClassKitWrathScripts(); // the Wrathbringer's cards: src/emberfall_classkit_wrath.cpp
    AddEmberfallClassKitHeatScripts();  // the Pyromancer's Heat: src/emberfall_classkit_heat.cpp
    AddEmberfallClassKitOathScripts();  // the Oathkeeper's chain and shields: src/emberfall_classkit_oath.cpp
    AddEmberfallClassKitHuntScripts();  // the Hunter's: src/emberfall_classkit_hunt.cpp
    AddEmberfallClassKitExecScripts();  // the Warrior's Executioner: src/emberfall_classkit_exec.cpp
    AddEmberfallClassKitBloomScripts(); // the Druid's Bloomtender: src/emberfall_classkit_bloom.cpp
    AddEmberfallClassKitSunScripts();   // the Paladin's Sunwarden: src/emberfall_classkit_sun.cpp
    AddEmberfallClassKitFlowScripts();  // the Warrior's Corsair: src/emberfall_classkit_flow.cpp
    AddEmberfallClassKitSealScripts();  // the Warlock's Damnator: src/emberfall_classkit_seal.cpp
}
