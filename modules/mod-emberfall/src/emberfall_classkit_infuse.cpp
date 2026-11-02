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

// Emberfall class kits: the Mage's Infusionist (Emberfall docs/design/classes/mage-infusionist.md, ankarna's approved
// v1.0 gameplay handoff of 2026-11-02). Generic scripts, their numbers in the spells' own effects (Emberfall's
// classkit/scripts_infuse.py checks the contracts); the arithmetic is classkit/EfInfuseLogic.h
// (tests/classkit_infuse_tests.cpp).
//
//   spell_ef_kit_potions          the potion rule (a spec passive): effect 1 value = the vials, misc B = how many recharge
//                                 at once; effect 2 value = ms a vial recharges (never haste); effect 3 misc = the marker
//                                 aura kept while no potion can be spent (a spell_ef_kit_cast_override helper: Restorative
//                                 Flask's button is Arcane Restoration). The player's potions live in the module (full at
//                                 the rule's arrival, gone with it); every player tick runs the recharges, out of combat
//                                 the vials are full at once. The client is told in EFP messages (EfInfuseLogic.h): at
//                                 every change and every 5 s
//   spell_ef_kit_potion_cost      a spender: a dummy on the caster with misc = the potion rule, value = the potions. Refused
//                                 (SPELL_FAILED_NO_POWER) when they are not ready, taken when the cast goes (a triggered cast
//                                 pays nothing). Perfect Formula (rule _perfect_formula naming the potion rule: every
//                                 value-th potion spent is free) counts every potion; Efficient Infusion (rule
//                                 _potion_refund naming the spell: value % to give misc B of the vials taken back)
//   spell_ef_kit_potion_rush      Rapid Synthesis' aura: effect 1 value = ms a vial recharges while it holds, effect 2 value
//                                 = more vials at once (Expanded Production), effect 3 value = vials restored when it comes
//                                 (Immediate Preparation; each advances Perfect Formula's count, never past the free one).
//                                 A running recharge keeps its share done when the rate changes [proposal]
//   spell_ef_kit_split_stacks     a damage over time whose every application is a stack with its own timer (Volatile
//                                 Flask): the tick deals the amount x the stacks, a stack goes when its time is up, the aura
//                                 when the last one does. When the enemy dies under it, the caster's _potion_on_kill rules
//                                 naming it restore their value in potions, once a death (Reclaimed Essence)
//   spell_ef_kit_infuse_rules     the caster's rules for the ability, once a cast: on its own target struck, an enemy:
//                                 _mana_back (value % of base mana), _potion_on_crit (a crit: value potions restored),
//                                 _chain_hits (a buff's rule: trigger at up to value other enemies within misc B yards of
//                                 the target, the buff spent: Chain Reaction); an ally healed: _twin_heal (trigger from the
//                                 target at the most injured other ally within misc B yards of it, the caster included: Twin
//                                 Infusion); after the cast: _impact_field (trigger at the cast's destination: Lingering
//                                 Reaction)
//   spell_ef_kit_vapor            a cloud on the ground (effect 1 a persistent area aura with a periodic dummy on allies,
//                                 a dummy's trigger the tick heal): one per caster, a new cast removes the old; each tick
//                                 heals every ally inside; _field_start rules (Restorative Surge) cast their trigger at every
//                                 ally inside when it is placed, _field_end rules (Vapor Detonation) when it ends (the last
//                                 tick) or is replaced
//   spell_ef_kit_chain_heal       a chain heal: after the first target, a dummy on the caster (value = allies in all, spell
//                                 mods in: Extended Remedy; misc B = yards a jump; trigger = the jump) jumps from each to the
//                                 most injured ally within reach not healed yet; _chain_momentum (value % more each jump),
//                                 _chain_rebound (back to the first once, the next jump's percent) [proposal]
//   spell_ef_kit_chain_heal_jump  the jump: effect 2's value (the chain casts it with) is the percent of its heal
//   spell_ef_kit_rule_*           the rules' own aura scripts: markers

#include "EfClassKit.h"
#include "EfInfuseLogic.h"

#include "CellImpl.h"
#include "Chat.h"
#include "DynamicObject.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Random.h"
#include "Group.h"
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
#include "WorldPacket.h"

#include <algorithm>
#include <list>
#include <vector>

namespace IL = Emberfall::ClassKit::Infuse;

namespace
{
    constexpr char const* kPotionsKey = "ef_infuse_potions";
    constexpr char const* POTIONS = "spell_ef_kit_potions";
    constexpr char const* RUSH = "spell_ef_kit_potion_rush";
    constexpr uint32 kResendMs = 5000;

    struct PotionState : public DataMap::Base
    {
        IL::Potions potions;
        uint32 rule = 0;          // the potion rule's spell
        uint32 marker = 0;        // the marker aura while depleted
        int32 rateMs = 0;         // the rate the running recharges run at
        std::string shape;        // what the client was told last
        uint32 sinceSentMs = 0;
    };

    PotionState* StateOf(Unit* unit)
    {
        Player* player = unit ? unit->ToPlayer() : nullptr;
        return player ? player->CustomData.Get<PotionState>(kPotionsKey) : nullptr;
    }

    // The aura effects of `unit`'s auras bound to `script` (the potion rule, Rapid Synthesis), `ignore` left out.
    AuraEffect const* ScriptAura(Unit* unit, char const* script, uint8 effIndex, Aura const* ignore = nullptr)
    {
        for (AuraEffect const* eff : unit->GetAuraEffectsByType(SPELL_AURA_DUMMY))
            if (eff->GetEffIndex() == effIndex && eff->GetBase() != ignore && eff->GetCasterGUID() == unit->GetGUID()
                && eff->GetBase()->GetScriptByName(script))
                return eff;
        return nullptr;
    }

    // Perfect Formula's period (0: not known).
    uint32 FormulaEvery(Unit* unit, uint32 rule)
    {
        int32 every = 0;
        for (AuraEffect const* eff : Emberfall::ClassKit::Rules(unit, "spell_ef_kit_rule_perfect_formula", rule))
            every = std::max(every, eff->GetAmount());
        return every >= 2 ? uint32(every) : 0;
    }

    // The rate and the vials at once now: the rule's, or Rapid Synthesis' while it holds (`ignore`: the aura going).
    std::pair<int32, uint32> RateAndLimit(Unit* unit, Aura const* ignore = nullptr)
    {
        AuraEffect const* vials = ScriptAura(unit, POTIONS, EFFECT_0);
        AuraEffect const* rate = ScriptAura(unit, POTIONS, EFFECT_1);
        int32 ms = rate ? std::max(100, rate->GetAmount()) : 8000;
        uint32 limit = vials ? uint32(std::max(1, vials->GetMiscValueB())) : 3;
        if (AuraEffect const* rush = ScriptAura(unit, RUSH, EFFECT_0, ignore))
        {
            ms = std::max(100, rush->GetAmount());
            if (AuraEffect const* more = rush->GetBase()->GetEffect(EFFECT_1))
                limit += uint32(std::max(0, more->GetAmount()));
        }
        return { ms, limit };
    }

    void Send(Player* player, std::string const& body)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, std::string(IL::kPrefix) + body);
        player->SendDirectMessage(&data);
    }

    // The marker follows the potions, the client is told what changed.
    void Sync(Player* player, PotionState& st, bool force = false)
    {
        uint32 const every = FormulaEvery(player, st.rule);
        if (st.marker)
        {
            bool const depleted = IL::Depleted(st.potions, every);
            bool const has = player->HasAura(st.marker, player->GetGUID());
            if (depleted && !has)
                player->AddAura(st.marker, player);
            else if (!depleted && has)
                player->RemoveAurasDueToSpell(st.marker, player->GetGUID());
        }
        std::string const shape = IL::Shape(st.potions, every);
        if (force || shape != st.shape || st.sinceSentMs >= kResendMs)
        {
            st.shape = shape;
            st.sinceSentMs = 0;
            Send(player, IL::Message(st.potions, every));
        }
    }

    // A rate or limit change (Rapid Synthesis comes or goes): the running recharges keep their share done.
    void Retime(Player* player, PotionState& st, Aura const* ignore = nullptr)
    {
        auto const [rate, limit] = RateAndLimit(player, ignore);
        if (rate != st.rateMs)
        {
            IL::Rescale(st.potions, rate);
            st.rateMs = rate;
        }
        IL::StartQueued(st.potions, limit, rate);
    }

    void Update(Player* player, uint32 diff)
    {
        PotionState* st = player->CustomData.Get<PotionState>(kPotionsKey);
        if (!st)
            return;
        st->sinceSentMs += diff;
        if (!player->IsInCombat())
        {
            if (!IL::IsFull(st->potions))
                IL::Fill(st->potions);   // out of combat the vials are full at once [proposal]
        }
        else
        {
            Retime(player, *st);
            auto const [rate, limit] = RateAndLimit(player);
            IL::Advance(st->potions, int32(diff), limit, rate);
        }
        Sync(player, *st);
    }

    // Spent vials ready again at once (Arcane Recovery, Reclaimed Essence, Efficient Infusion, Immediate Preparation).
    uint32 RestorePotions(Unit* unit, uint32 count)
    {
        Player* player = unit ? unit->ToPlayer() : nullptr;
        PotionState* st = StateOf(player);
        if (!st || !count)
            return 0;
        auto const [rate, limit] = RateAndLimit(player);
        uint32 const done = IL::Restore(st->potions, count, limit, rate);
        Sync(player, *st);
        return done;
    }

    // An ally of the caster's heals: the caster, its group or raid and what any of them controls (the companions).
    bool HealAlly(Unit* caster, Unit* unit)
    {
        if (!caster || !unit || !unit->IsAlive() || !unit->IsInWorld() || unit->GetMap() != caster->GetMap())
            return false;
        if (unit == caster)
            return true;
        if (!caster->IsFriendlyTo(unit))
            return false;
        Player* me = caster->GetCharmerOrOwnerPlayerOrPlayerItself();
        Player* them = unit->GetCharmerOrOwnerPlayerOrPlayerItself();
        if (!me || !them)
            return unit->GetCharmerOrOwnerOrSelf() == caster;
        return me == them || me->IsInSameRaidWith(them);
    }

    // The most injured ally (by the share of health missing) within `range` of `center`, those in `skip` left out.
    Unit* MostInjuredNear(Unit* caster, WorldObject* center, float range, std::vector<Unit*> const& skip)
    {
        Unit* best = nullptr;
        for (Unit* unit : Emberfall::ClassKit::PartyUnits(caster, center, range, true))
        {
            if (std::find(skip.begin(), skip.end(), unit) != skip.end() || unit->IsFullHealth() || !HealAlly(caster, unit))
                continue;
            if (!center->IsWithinLOSInMap(unit))
                continue;
            if (!best || unit->GetHealthPct() < best->GetHealthPct())
                best = unit;
        }
        return best;
    }

    bool HitCrit(Spell* spell, Unit* target)
    {
        if (!spell || !target)
            return false;
        for (TargetInfo const& info : *spell->GetUniqueTargetInfo())
            if (info.targetGUID == target->GetGUID())
                return info.crit;
        return false;
    }

    uint32 TriggerOf(AuraEffect const* eff)
    {
        return eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell;
    }
}

// The potion rule: the player's potions come with it and go with it.
class spell_ef_kit_potions : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_potions);

    void Arrive(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget() ? GetTarget()->ToPlayer() : nullptr;
        if (!player)
            return;
        PotionState* st = player->CustomData.GetDefault<PotionState>(kPotionsKey);
        st->potions = IL::Full(uint32(std::clamp(aurEff->GetAmount(), 1, 20)));
        st->rule = GetId();
        st->marker = uint32(std::max(0, GetSpellInfo()->Effects[EFFECT_2].MiscValue));
        st->rateMs = RateAndLimit(player).first;
        st->shape.clear();
        Sync(player, *st, true);
    }

    void Leave(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget() ? GetTarget()->ToPlayer() : nullptr;
        PotionState* st = StateOf(player);
        if (!st)
            return;
        if (st->marker)
            player->RemoveAurasDueToSpell(st->marker);
        player->CustomData.Erase(kPotionsKey);
        Send(player, IL::OffMessage());
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_ef_kit_potions::Arrive, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_ef_kit_potions::Leave, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

class spell_ef_kit_potion_cost : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_potion_cost);

    // the potions the cast costs (the dummy on the caster naming the potion rule)
    uint32 Cost(Unit* caster, uint32 rule)
    {
        for (SpellEffectInfo const& eff : GetSpellInfo()->Effects)
            if (eff.Effect == SPELL_EFFECT_DUMMY && eff.TargetA.GetTarget() == TARGET_UNIT_CASTER && rule
                && uint32(eff.MiscValue) == rule)
                return uint32(std::max(0, eff.CalcValue(caster)));
        return 0;
    }

    SpellCastResult Check()
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->IsPlayer() || GetSpell()->IsTriggered())
            return SPELL_CAST_OK;
        PotionState* st = StateOf(caster);
        if (!st)
            return SPELL_FAILED_NO_POWER;   // no potions without the potion rule (another spec)
        uint32 const cost = Cost(caster, st->rule);
        return IL::CanPay(st->potions, cost, FormulaEvery(caster, st->rule)) ? SPELL_CAST_OK : SPELL_FAILED_NO_POWER;
    }

    void Pay()
    {
        Unit* caster = GetCaster();
        Player* player = caster ? caster->ToPlayer() : nullptr;
        PotionState* st = StateOf(player);
        if (!st || GetSpell()->IsTriggered())
            return;
        uint32 const cost = Cost(player, st->rule);
        if (!cost)
            return;
        Retime(player, *st);
        auto const [rate, limit] = RateAndLimit(player);
        uint32 const paid = IL::Spend(st->potions, cost, FormulaEvery(player, st->rule), limit, rate);
        // Efficient Infusion: a chance to give a vial taken back (a free potion took none: nothing to give back)
        if (paid)
            for (AuraEffect const* eff : Emberfall::ClassKit::Rules(player, "spell_ef_kit_rule_potion_refund", GetSpellInfo()->Id))
                if (roll_chance_i(eff->GetAmount()))
                    IL::Restore(st->potions, std::min<uint32>(paid, uint32(std::max(1, eff->GetMiscValueB()))), limit, rate);
        Sync(player, *st);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_ef_kit_potion_cost::Check);
        OnCast += SpellCastFn(spell_ef_kit_potion_cost::Pay);
    }
};

// Rapid Synthesis: the faster rate while it holds, more at once, vials restored as it comes.
class spell_ef_kit_potion_rush : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_potion_rush);

    void Arrive(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget() ? GetTarget()->ToPlayer() : nullptr;
        PotionState* st = StateOf(player);
        if (!st)
            return;
        Retime(player, *st);
        if (AuraEffect const* restore = GetAura()->GetEffect(EFFECT_2); restore && restore->GetAmount() > 0)
        {
            auto const [rate, limit] = RateAndLimit(player);
            uint32 const back = IL::Restore(st->potions, uint32(restore->GetAmount()), limit, rate);
            st->potions.pf = IL::AdvanceFormula(st->potions.pf, back, FormulaEvery(player, st->rule));
        }
        Sync(player, *st);
    }

    void Leave(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget() ? GetTarget()->ToPlayer() : nullptr;
        PotionState* st = StateOf(player);
        if (!st)
            return;
        Retime(player, *st, GetAura());
        Sync(player, *st);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_ef_kit_potion_rush::Arrive, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_ef_kit_potion_rush::Leave, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Volatile Flask's aura: every application a stack with a timer of its own.
class spell_ef_kit_split_stacks_aura : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_split_stacks_aura);

    std::vector<uint64> _ends;
    bool _own = false;   // our own stack change (not an application)

    void Follow(uint32 left)
    {
        uint32 const cap = std::max<uint32>(1, GetSpellInfo()->StackAmount);
        uint8 const stacks = uint8(std::clamp<uint32>(left, 1, std::min<uint32>(cap, 255)));
        if (GetStackAmount() != stacks)
        {
            _own = true;
            GetAura()->SetStackAmount(stacks);
            _own = false;
        }
    }

public:
    // A new application (the flask struck): its own timer, the aura's duration its end.
    void AddApplication()
    {
        uint64 const now = GameTime::GetGameTimeMS().count();
        _ends.push_back(now + uint64(std::max(0, GetAura()->GetMaxDuration())));
        uint32 const cap = std::max<uint32>(1, GetSpellInfo()->StackAmount);
        while (_ends.size() > cap)
            _ends.erase(std::min_element(_ends.begin(), _ends.end()));
        Follow(uint32(_ends.size()));
        GetAura()->SetDuration(IL::LongestLeft(_ends, now));
    }

private:
    void Tick(AuraEffect const* /*aurEff*/)
    {
        if (_ends.empty())
            return;   // not one of ours yet (applied some other way): as it is
        uint64 const now = GameTime::GetGameTimeMS().count();
        uint32 const left = IL::Prune(_ends, now);
        if (!left)
        {
            PreventDefaultAction();
            Remove(AURA_REMOVE_BY_EXPIRE);
            return;
        }
        Follow(left);
    }

    // Reclaimed Essence: the enemy died under it
    void Gone(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_DEATH)
            return;
        Unit* caster = GetCaster();
        if (!caster)
            return;
        int32 potions = 0;
        for (AuraEffect const* eff : Emberfall::ClassKit::Rules(caster, "spell_ef_kit_rule_potion_on_kill", GetId()))
            potions += std::max(0, eff->GetAmount());
        if (potions > 0)
            RestorePotions(caster, uint32(potions));
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_ef_kit_split_stacks_aura::Tick, EFFECT_ALL, SPELL_AURA_PERIODIC_DAMAGE);
        AfterEffectRemove += AuraEffectRemoveFn(spell_ef_kit_split_stacks_aura::Gone, EFFECT_ALL, SPELL_AURA_PERIODIC_DAMAGE,
            AURA_EFFECT_HANDLE_REAL);
    }
};

class spell_ef_kit_split_stacks : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_split_stacks);

    void AfterHitTarget()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target)
            return;
        if (Aura* aura = target->GetAura(GetSpellInfo()->Id, caster->GetGUID()))
            if (auto* script = aura->GetScript<spell_ef_kit_split_stacks_aura>("spell_ef_kit_split_stacks"))
                script->AddApplication();
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_ef_kit_split_stacks::AfterHitTarget);
    }
};

class spell_ef_kit_infuse_rules : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_infuse_rules);

    bool _done = false;

    void Enemy(Unit* caster, Unit* target)
    {
        using namespace Emberfall::ClassKit;
        uint32 const id = GetSpellInfo()->Id;
        // Mana Reclamation: a share of base mana on a hit
        for (AuraEffect const* eff : Rules(caster, "spell_ef_kit_rule_mana_back", id))
            if (int32 const mana = int32(caster->GetCreateMana()) * eff->GetAmount() / 100; mana > 0)
                caster->EnergizeBySpell(caster, id, uint32(mana), POWER_MANA);
        // Arcane Recovery: a crit restores potions
        if (HitCrit(GetSpell(), target))
            for (AuraEffect const* eff : Rules(caster, "spell_ef_kit_rule_potion_on_crit", id))
                RestorePotions(caster, uint32(std::max(0, eff->GetAmount())));
        // Chain Reaction: a buff's rule, spent by this hit
        struct Chain { uint32 trigger; int32 count; float yards; uint32 aura; };
        std::vector<Chain> chains;
        for (AuraEffect const* eff : Rules(caster, "spell_ef_kit_rule_chain_hits", id))
            if (uint32 const trigger = TriggerOf(eff); trigger && !eff->GetBase()->IsPassive())
                chains.push_back({ trigger, eff->GetAmount(), float(std::max(1, eff->GetMiscValueB())), eff->GetId() });
        for (Chain const& c : chains)
        {
            caster->RemoveAurasDueToSpell(c.aura, caster->GetGUID());
            std::list<Unit*> near;
            Acore::AnyUnfriendlyUnitInObjectRangeCheck check(target, caster, c.yards);
            Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(target, near, check);
            Cell::VisitObjects(target, searcher, c.yards);
            std::vector<Unit*> others;
            for (Unit* unit : near)
                if (unit != target && unit->IsAlive() && caster->IsValidAttackTarget(unit) && target->IsWithinLOSInMap(unit))
                    others.push_back(unit);
            std::sort(others.begin(), others.end(), [target](Unit* a, Unit* b) { return target->GetExactDist(a) < target->GetExactDist(b); });
            for (int32 i = 0; i < c.count && i < int32(others.size()); ++i)
                caster->CastSpell(others[i], c.trigger, true);
        }
    }

    void Ally(Unit* caster, Unit* target)
    {
        // Twin Infusion: from the target to the most injured other ally near it
        for (AuraEffect const* eff : Emberfall::ClassKit::Rules(caster, "spell_ef_kit_rule_twin_heal", GetSpellInfo()->Id))
            if (uint32 const trigger = TriggerOf(eff))
                if (Unit* other = MostInjuredNear(caster, target, float(std::max(1, eff->GetMiscValueB())), { target }))
                    target->CastSpell(other, trigger, true, nullptr, nullptr, caster->GetGUID());
    }

    void AfterHitTarget()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (_done || !caster || !target || target != GetExplTargetUnit())
            return;
        _done = true;
        if (target != caster && !caster->IsFriendlyTo(target))
            Enemy(caster, target);
        else
            Ally(caster, target);
    }

    // Lingering Reaction: a field at the cast's destination
    void Finish()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;
        auto const rules = Emberfall::ClassKit::Rules(caster, "spell_ef_kit_rule_impact_field", GetSpellInfo()->Id);
        if (rules.empty())
            return;
        Position pos;
        if (WorldLocation const* dest = GetExplTargetDest())
            pos.Relocate(dest->GetPositionX(), dest->GetPositionY(), dest->GetPositionZ());
        else if (Unit* target = GetExplTargetUnit())
            pos.Relocate(target);
        else
            return;
        for (AuraEffect const* eff : rules)
            if (uint32 const trigger = TriggerOf(eff))
                caster->CastSpell(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(), trigger, true);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_ef_kit_infuse_rules::AfterHitTarget);
        AfterCast += SpellCastFn(spell_ef_kit_infuse_rules::Finish);
    }
};

namespace
{
    // The cloud's rules (`script`: _field_start, _field_end) naming `spell`: their triggers at every ally of `units`.
    void FieldRules(Unit* caster, uint32 spell, char const* script, std::vector<Unit*> const& units)
    {
        for (AuraEffect const* eff : Emberfall::ClassKit::Rules(caster, script, spell))
            if (uint32 const trigger = TriggerOf(eff))
                for (Unit* unit : units)
                    if (unit->IsAlive())
                        caster->CastSpell(unit, trigger, true);
    }

    // The allies of the caster within the cloud `dyn` (`inside`: those its aura is on now).
    std::vector<Unit*> CloudAllies(Unit* caster, DynamicObject* dyn, bool inside)
    {
        std::vector<Unit*> out;
        for (Unit* unit : Emberfall::ClassKit::PartyUnits(caster, dyn, dyn->GetRadius(), true))
            if (HealAlly(caster, unit) && (!inside || unit->HasAura(dyn->GetSpellId(), caster->GetGUID())))
                out.push_back(unit);
        return out;
    }
}

class spell_ef_kit_vapor : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_vapor);

    // one cloud per caster: the old one bursts (Vapor Detonation) and goes before the new one is placed
    void Replace()
    {
        Unit* caster = GetCaster();
        uint32 const id = GetSpellInfo()->Id;
        if (!caster)
            return;
        while (DynamicObject* old = caster->GetDynObject(id))
        {
            FieldRules(caster, id, "spell_ef_kit_rule_field_end", CloudAllies(caster, old, true));
            if (!caster->RemoveDynObject(id))
                break;
        }
    }

    // Restorative Surge: every ally inside the new cloud at once
    void Placed()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;
        if (DynamicObject* cloud = caster->GetDynObject(GetSpellInfo()->Id))
            FieldRules(caster, GetSpellInfo()->Id, "spell_ef_kit_rule_field_start", CloudAllies(caster, cloud, false));
    }

    void Register() override
    {
        BeforeCast += SpellCastFn(spell_ef_kit_vapor::Replace);
        AfterCast += SpellCastFn(spell_ef_kit_vapor::Placed);
    }
};

class spell_ef_kit_vapor_aura : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_vapor_aura);

    bool CheckTarget(Unit* target)
    {
        return HealAlly(GetCaster(), target);
    }

    void Tick(AuraEffect const* aurEff)
    {
        Unit* caster = GetCaster();
        Unit* target = GetTarget();
        if (!caster || !target || !target->IsAlive())
            return;
        for (SpellEffectInfo const& eff : GetSpellInfo()->Effects)
            if (eff.Effect == SPELL_EFFECT_DUMMY && eff.TriggerSpell)
            {
                caster->CastSpell(target, eff.TriggerSpell, true);
                break;
            }
        // the cloud's end (its last tick): Vapor Detonation
        if (aurEff->GetTotalTicks() > 0 && int32(aurEff->GetTickNumber()) >= aurEff->GetTotalTicks())
            FieldRules(caster, GetId(), "spell_ef_kit_rule_field_end", { target });
    }

    void Register() override
    {
        DoCheckAreaTarget += AuraCheckAreaTargetFn(spell_ef_kit_vapor_aura::CheckTarget);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_ef_kit_vapor_aura::Tick, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

class spell_ef_kit_chain_heal : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_chain_heal);

    bool _done = false;

    void AfterHitTarget()
    {
        Unit* caster = GetCaster();
        Unit* first = GetHitUnit();
        if (_done || !caster || !first || first != GetExplTargetUnit() || !HealAlly(caster, first))
            return;
        _done = true;
        SpellEffectInfo const* chain = nullptr;
        for (SpellEffectInfo const& eff : GetSpellInfo()->Effects)
            if (eff.Effect == SPELL_EFFECT_DUMMY && eff.TriggerSpell && eff.TargetA.GetTarget() == TARGET_UNIT_CASTER)
                chain = &eff;
        if (!chain)
            return;
        uint32 const id = GetSpellInfo()->Id;
        int32 const total = std::max(1, chain->CalcValue(caster));
        float const yards = float(std::max(1, chain->MiscValueB));
        int32 momentum = 0;
        for (AuraEffect const* eff : Emberfall::ClassKit::Rules(caster, "spell_ef_kit_rule_chain_momentum", id))
            momentum += eff->GetAmount();
        bool const rebound = !Emberfall::ClassKit::Rules(caster, "spell_ef_kit_rule_chain_rebound", id).empty();
        std::vector<Unit*> healed = { first };
        Unit* prev = first;
        uint32 jump = 1;
        for (; int32(jump) < total; ++jump)
        {
            Unit* next = MostInjuredNear(caster, prev, yards, healed);
            if (!next)
                break;
            int32 const pct = IL::JumpPct(jump, momentum);
            prev->CastCustomSpell(next, chain->TriggerSpell, nullptr, &pct, nullptr, true, nullptr, nullptr, caster->GetGUID());
            healed.push_back(next);
            prev = next;
        }
        // Rebounding Remedy: from the last back to the first, once (an exception to "each ally once")
        if (rebound && prev != first && first->IsAlive() && prev->IsWithinDistInMap(first, std::max(yards, 40.0f)))
        {
            int32 const pct = IL::JumpPct(jump, momentum);
            prev->CastCustomSpell(first, chain->TriggerSpell, nullptr, &pct, nullptr, true, nullptr, nullptr, caster->GetGUID());
        }
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_ef_kit_chain_heal::AfterHitTarget);
    }
};

class spell_ef_kit_chain_heal_jump : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_chain_heal_jump);

    int32 _pct = 100;

    void Percent(SpellEffIndex /*effIndex*/)
    {
        _pct = GetEffectValue();
    }

    void Scale()
    {
        if (_pct != 100 && _pct > 0)
            SetHitHeal(int32(int64(GetHitHeal()) * _pct / 100));
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_ef_kit_chain_heal_jump::Percent, EFFECT_1, SPELL_EFFECT_DUMMY);
        OnHit += SpellHitFn(spell_ef_kit_chain_heal_jump::Scale);
    }
};

// A rule's own script: a marker the rules are found by (EfClassKit.h Rules); nothing happens in it.
class spell_ef_kit_infuse_rule : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_infuse_rule);

    void Register() override { }
};

// Every player tick: the recharges run, out of combat the vials fill, the client is told.
class emberfall_classkit_infuse_player : public PlayerScript
{
public:
    emberfall_classkit_infuse_player() : PlayerScript("emberfall_classkit_infuse_player", { PLAYERHOOK_ON_UPDATE }) { }

    void OnPlayerUpdate(Player* player, uint32 diff) override { Update(player, diff); }
};

void AddEmberfallClassKitInfuseScripts()
{
    RegisterSpellScript(spell_ef_kit_potions);
    RegisterSpellScript(spell_ef_kit_potion_cost);
    RegisterSpellScript(spell_ef_kit_potion_rush);
    RegisterSpellAndAuraScriptPairWithArgs(spell_ef_kit_split_stacks, spell_ef_kit_split_stacks_aura, "spell_ef_kit_split_stacks");
    RegisterSpellScript(spell_ef_kit_infuse_rules);
    RegisterSpellAndAuraScriptPairWithArgs(spell_ef_kit_vapor, spell_ef_kit_vapor_aura, "spell_ef_kit_vapor");
    RegisterSpellScript(spell_ef_kit_chain_heal);
    RegisterSpellScript(spell_ef_kit_chain_heal_jump);
    for (char const* name : { "spell_ef_kit_rule_potion_refund", "spell_ef_kit_rule_potion_on_crit",
                              "spell_ef_kit_rule_potion_on_kill", "spell_ef_kit_rule_perfect_formula",
                              "spell_ef_kit_rule_mana_back", "spell_ef_kit_rule_chain_hits", "spell_ef_kit_rule_impact_field",
                              "spell_ef_kit_rule_twin_heal", "spell_ef_kit_rule_chain_momentum",
                              "spell_ef_kit_rule_chain_rebound", "spell_ef_kit_rule_field_start", "spell_ef_kit_rule_field_end" })
        RegisterSpellScriptWithArgs(spell_ef_kit_infuse_rule, name);
    new emberfall_classkit_infuse_player();
}
