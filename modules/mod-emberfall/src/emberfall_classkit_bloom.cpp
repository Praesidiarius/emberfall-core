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

// Emberfall class kits: the Druid's Bloomtender (Emberfall docs/design/classes/druid-bloomtender.md: plants that grow
// from Seed to Sprout to Bloom, a tree that stores healing, a thorn plant that shoots on its own). Generic scripts, their
// numbers in the spells' own effects (Emberfall's classkit/model.py SCRIPTS checks the contracts); the arithmetic is
// EfClassKitLogic.h (tests/classkit_tests.cpp).
//
// A growth chain is a row of stage auras ending in a heal: each stage's effect 1 is a periodic heal and its effect 2 a
// dummy whose trigger is the next stage (Seed -> Sprout) or, for the last stage, the heal cast when it ends (Sprout ->
// Bloom). The first stage's effect 2 value is the cycle in ms (18 s): the chain runs on one clock, so pushing a stage
// does not reset it (a stage's effect 2 amount, set by the script, is what was left of the cycle when the stage began;
// negative marks a replanted Seed, Cycle of Life's, which never replants again).
//
//   spell_ef_kit_growth         effect 1 a dummy whose trigger is the first stage: at its unit target (an ally), or with
//                               misc B > 0 at every party member within misc B yards (EfClassKit.h PartyUnits: the
//                               caster, its group or raid subgroup and their companions, no pets; line of sight to each)
//                               no stage of the caster's yet -> the first stage (its cycle + the effect's value: Lasting
//                               Renewal's mods on Seeds of Renewal); a stage -> the next one at once (Lifesprout pushed:
//                               Seed -> Sprout, Sprout -> Bloom) (Lifesprout, Seeds of Renewal)
//   spell_ef_kit_growth_stage   a stage aura: when it runs out the next stage starts with what is left of the cycle, the
//                               last one casts its heal at the owner. Caster passives: a dummy naming the last stage with
//                               misc B > 0 and no trigger sends a heal on an owner at full health to the most injured
//                               party member within misc B yards of it (Wandering Bloom); a dummy naming the first stage
//                               with no trigger plants a fresh first stage after a natural end (Cycle of Life)
//   spell_ef_kit_heal_echo      a heal (effect 1); the caster's passive dummies naming it: no trigger and misc B > 0, its
//                               value in percent more healing on an ally below misc B percent health (Deep Restoration);
//                               a trigger that heals and misc B > 0, value percent of the heal cast as the trigger at up to
//                               misc B other injured party members, most injured first, within the trigger's effect 2
//                               value in yards of the ally (Radiant Blossom, Shared Vitality); then the hit bonuses
//                               (EfClassKit.h HitBonuses, self triggers once: Rising Sap's mana on every Bloom)
//                               (Nature's Embrace, Lifesprout's Bloom)
//   spell_ef_kit_mana_back      effect 1 (energize of mana on the caster): its value in tenths of a percent of the
//                               caster's max mana (Rising Sap: 20, Returning Essence 25/30)
//   spell_ef_kit_sentry         an aura on the caster: effect 1 a periodic dummy with misc = the creature, misc B = its
//                               reach in yards, trigger = the shot; effect 2 a dummy with value = yards beside the caster
//                               where it stands and trigger = its look aura. While the aura holds the creature stands
//                               there (untargetable, unkillable, the caster's level and faction) and every tick shoots
//                               the trigger at a random enemy within its reach and sight that is already fighting the
//                               caster's group, never one held by crowd control that damage breaks; the shot is the
//                               caster's (spell power, threat, the combat log). It goes with the aura (Thornspitter)
//   spell_ef_kit_stored_heal    (a spell and its own aura) effect 1 a dummy aura on the caster: value = the percent of the
//                               caster's raw healing it stores (overhealing included; spell mods: Ancient Reservoir), misc
//                               B = the creature, trigger = the release heal (its effect 2 value: the release radius);
//                               effect 3 a dummy at an ally: where the creature stands, trigger = its look aura. A cast
//                               while the aura holds releases instead of summoning. The release (pressed, cancelled or the
//                               aura's end) heals the injured party members within the radius of the creature, lowest
//                               health percent first, each what it misses until the store is spent, ignoring line of
//                               sight; the cooldown counts from the summon. A caster dummy naming the spell with a trigger
//                               (a growth chain's first stage) blooms every plant of that chain in the radius
//                               (Final Flourish) (Verdant Ancient)
//   spell_ef_kit_clear_dots     a hold that damage breaks: when it lands, the caster's own periodic damage on the target
//                               goes, so the caster's poisons never break it (Becalm and Venomous Barrage)
//
// A unit script feeds the stores (every heal of a kit spell of the druid family while the caster's store is up) and
// strikes back for the plants (a caster dummy naming a plant or shield aura with a trigger: when its bearer is struck in
// melee, the caster casts the trigger at the attacker, at most once every misc B ms per bearer: Nature's Retaliation).

#include "EfClassKit.h"
#include "EfClassKitLogic.h"
#include "EfCompanions.h"

#include "Creature.h"
#include "DataMap.h"
#include "GameTime.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "TemporarySummon.h"
#include "Unit.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace
{
    namespace Logic = Emberfall::ClassKit::Logic;

    constexpr uint32 kDruidFamily = 111;   // the Druid kit's SpellFamilyName (Emberfall dbc/tables.py SPELL_FAMILIES)
    constexpr std::size_t kMaxStages = 4;  // a chain longer than this is a data loop

    uint64 NowMs()
    {
        return uint64(GameTime::GetGameTimeMS().count());
    }

    // ---- growth chains ------------------------------------------------------------------------------------------------

    bool IsStage(SpellInfo const* info)
    {
        return info && info->Effects[EFFECT_0].ApplyAuraName == SPELL_AURA_PERIODIC_HEAL
            && info->Effects[EFFECT_1].ApplyAuraName == SPELL_AURA_DUMMY && info->Effects[EFFECT_1].TriggerSpell;
    }

    // The stage auras of the chain that starts at `first`, in order (the heal that ends it is not in the list).
    std::vector<uint32> Chain(uint32 first)
    {
        std::vector<uint32> out;
        for (uint32 s = first; s && out.size() < kMaxStages; )
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(s);
            if (!IsStage(info) || std::find(out.begin(), out.end(), s) != out.end())
                break;
            out.push_back(s);
            s = info->Effects[EFFECT_1].TriggerSpell;
        }
        return out;
    }

    // The first stage of the chain that `stage` belongs to, as some caster passive names it (Cycle of Life), or 0.
    bool ChainHas(uint32 first, uint32 stage)
    {
        std::vector<uint32> const chain = Chain(first);
        return std::find(chain.begin(), chain.end(), stage) != chain.end();
    }

    Aura* StageOn(Unit* unit, Unit* caster, std::vector<uint32> const& chain)
    {
        for (uint32 s : chain)
            if (Aura* a = unit->GetAura(s, caster->GetGUID()))
                return a;
        return nullptr;
    }

    bool HasTrigger(AuraEffect const* eff)
    {
        return eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell != 0;
    }

    // The most injured party member of `caster` within `yards` of `center` (by the share of health missing), or nullptr.
    Unit* MostInjuredNear(Unit* caster, WorldObject const* center, float yards, Unit* except = nullptr)
    {
        Unit* best = nullptr;
        float bestPct = 101.0f;
        for (Unit* u : Emberfall::ClassKit::PartyUnits(caster, center, yards, true))
        {
            if (u == except || u->IsFullHealth())
                continue;
            float const pct = u->GetHealthPct();
            if (pct < bestPct)
            {
                bestPct = pct;
                best = u;
            }
        }
        return best;
    }

    // Plants the first stage of a chain on `unit`: its cycle is the stage's effect 2 value + `extraMs`.
    void Plant(Unit* caster, Unit* unit, uint32 first, int32 extraMs, bool replant)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(first);
        if (!IsStage(info) || !unit->IsAlive())
            return;
        caster->CastSpell(unit, first, true);
        Aura* aura = unit->GetAura(first, caster->GetGUID());
        if (!aura)
            return;
        int32 const cycle = std::max(1, info->Effects[EFFECT_1].CalcValue(caster) + std::max(0, extraMs));
        if (AuraEffect* clock = aura->GetEffect(EFFECT_1))
            clock->ChangeAmount(replant ? -cycle : cycle, false);
        // the first stage never outlasts the cycle (a cycle shorter than the Seed: it blooms at the cycle's end)
        if (aura->GetDuration() > cycle)
        {
            aura->SetMaxDuration(cycle);
            aura->SetDuration(cycle);
        }
    }

    // The last stage ended or was pushed: the heal it carries (the Bloom) at the owner, or (Wandering Bloom) at the most
    // injured party member near an owner at full health; after a natural end Cycle of Life plants the chain again.
    void Bloom(Unit* caster, Unit* owner, SpellInfo const* last, bool natural, bool replanted)
    {
        uint32 const heal = last->Effects[EFFECT_1].TriggerSpell;
        if (!heal || !owner->IsInWorld())
            return;
        Unit* target = owner;
        if (owner->IsFullHealth())
        {
            int32 yards = 0;
            for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
                if (uint32(eff->GetMiscValue()) == last->Id && !HasTrigger(eff) && eff->GetMiscValueB() > 0)
                    yards = std::max(yards, eff->GetMiscValueB());
            if (yards > 0)
                if (Unit* other = MostInjuredNear(caster, owner, float(yards)))
                    target = other;
        }
        if (target->IsAlive())
            caster->CastSpell(target, heal, true);
        if (!natural || replanted || !owner->IsAlive())
            return;
        for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
        {
            uint32 const first = uint32(eff->GetMiscValue());
            if (HasTrigger(eff) || first == last->Id || !ChainHas(first, last->Id))
                continue;
            Plant(caster, owner, first, 0, true);
            break;
        }
    }

    // The stage `aura` on `owner` moves on: the next stage with what is left of the cycle, or the Bloom.
    void Advance(Unit* caster, Unit* owner, Aura* aura, bool natural)
    {
        SpellInfo const* info = aura->GetSpellInfo();
        AuraEffect const* clock = aura->GetEffect(EFFECT_1);
        int32 const amount = clock ? clock->GetAmount() : 0;
        bool const replanted = amount < 0;
        int32 const elapsed = natural ? aura->GetMaxDuration() : std::max(0, aura->GetMaxDuration() - aura->GetDuration());
        int32 const left = Logic::GrowthLeft(std::abs(amount), elapsed);
        uint32 const next = info->Effects[EFFECT_1].TriggerSpell;
        SpellInfo const* nextInfo = sSpellMgr->GetSpellInfo(next);
        if (!natural)
            owner->RemoveAura(aura, AURA_REMOVE_BY_DEFAULT);
        if (!IsStage(nextInfo))
        {
            Bloom(caster, owner, info, natural, replanted);
            return;
        }
        SpellInfo const* afterNext = sSpellMgr->GetSpellInfo(nextInfo->Effects[EFFECT_1].TriggerSpell);
        bool const last = !IsStage(afterNext);
        int32 const ms = Logic::StageMs(left, nextInfo->GetMaxDuration(), last);
        if (ms <= 0 || !owner->IsAlive())
        {
            // no time left for the next stage: it blooms at once (its heal, if it is the last)
            if (last && owner->IsAlive())
                Bloom(caster, owner, nextInfo, natural, replanted);
            return;
        }
        caster->CastSpell(owner, next, true);
        Aura* nextAura = owner->GetAura(next, caster->GetGUID());
        if (!nextAura)
            return;
        nextAura->SetMaxDuration(ms);
        nextAura->SetDuration(ms);
        if (AuraEffect* nextClock = nextAura->GetEffect(EFFECT_1))
            nextClock->ChangeAmount(replanted ? -left : left, false);
    }

    // Final Flourish: every plant of the chain starting at `first` on `unit` blooms now.
    void BloomNow(Unit* caster, Unit* unit, uint32 first)
    {
        std::vector<uint32> const chain = Chain(first);
        if (chain.empty())
            return;
        Aura* aura = StageOn(unit, caster, chain);
        if (!aura)
            return;
        SpellInfo const* last = sSpellMgr->GetSpellInfo(chain.back());
        unit->RemoveAura(aura, AURA_REMOVE_BY_DEFAULT);
        if (last)
            Bloom(caster, unit, last, false, false);
    }

    // ---- the stored heal (Verdant Ancient) ----------------------------------------------------------------------------

    constexpr char kStoreKey[] = "EfStoredHeal";

    struct Store : DataMap::Base
    {
        uint32 spell = 0;          // the spell whose aura holds the store
        uint32 release = 0;        // the release heal (never stored itself)
        ObjectGuid creature;       // the Ancient standing in the world
        uint64 hundredths = 0;     // what it holds, in hundredths of a point
        int32 pct = 0;             // the share of every raw heal it stores
        uint64 startMs = 0;        // the summon
        bool active = false;
    };

    Store* StoreOf(Unit* caster, bool create)
    {
        if (!caster)
            return nullptr;
        return create ? caster->CustomData.GetDefault<Store>(kStoreKey) : caster->CustomData.Get<Store>(kStoreKey);
    }

    void Configure(Creature* c, Unit* caster)
    {
        c->SetLevel(caster->GetLevel());
        c->SetFaction(caster->GetFaction());
        c->SetCreatorGUID(caster->GetGUID());
        c->SetUnitFlag(UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_NON_ATTACKABLE);
        c->SetReactState(REACT_PASSIVE);
        c->SetControlled(true, UNIT_STATE_ROOT);
    }

    // Puts the caster's cooldown of `spell` at `ms` left (the server's and the client's).
    void SetCooldown(Player* player, uint32 spell, int32 ms)
    {
        player->RemoveSpellCooldown(spell, true);
        if (ms <= 0)
            return;
        player->AddSpellCooldown(spell, 0, uint32(ms));
        WorldPacket data(SMSG_SPELL_COOLDOWN, 8 + 1 + 4 + 4);
        data << player->GetGUID();
        data << uint8(0);
        data << uint32(spell);
        data << uint32(ms);
        player->SendDirectMessage(&data);
    }

    void Release(Unit* caster, Store& store)
    {
        if (!store.active)
            return;
        store.active = false;
        Creature* ancient = ObjectAccessor::GetCreature(*caster, store.creature);
        WorldObject const* center = ancient ? static_cast<WorldObject const*>(ancient) : caster;
        SpellInfo const* release = sSpellMgr->GetSpellInfo(store.release);
        float const radius = release ? float(std::max(1, release->Effects[EFFECT_1].CalcValue(caster))) : 40.0f;
        uint64 const pool = store.hundredths / 100;
        store.hundredths = 0;
        // the injured party members in reach of the Ancient, lowest health percent first
        std::vector<Unit*> injured;
        for (Unit* u : Emberfall::ClassKit::PartyUnits(caster, center, radius, true))
            if (!u->IsFullHealth())
                injured.push_back(u);
        std::stable_sort(injured.begin(), injured.end(), [](Unit* a, Unit* b) { return a->GetHealthPct() < b->GetHealthPct(); });
        std::vector<uint32> missing, out(injured.size(), 0);
        for (Unit* u : injured)
            missing.push_back(u->GetMaxHealth() - u->GetHealth());
        Logic::ReleaseSplit(pool, missing.data(), out.data(), injured.size());
        if (release)
            for (std::size_t i = 0; i < injured.size(); ++i)
                if (out[i] > 0)
                {
                    int32 const bp = int32(out[i]);
                    caster->CastCustomSpell(injured[i], store.release, &bp, nullptr, nullptr, true);
                }
        // Final Flourish: a caster dummy naming the store's spell with a growth chain's first stage blooms its plants
        for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
        {
            if (uint32(eff->GetMiscValue()) != store.spell || !HasTrigger(eff))
                continue;
            uint32 const first = eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell;
            for (Unit* u : Emberfall::ClassKit::PartyUnits(caster, center, radius, true))
                BloomNow(caster, u, first);
        }
        if (ancient)
            ancient->DespawnOrUnsummon();
        store.creature.Clear();
        if (Player* player = caster->ToPlayer())
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(store.spell))
                SetCooldown(player, store.spell, Logic::CooldownFromStart(int32(info->RecoveryTime),
                                                                           int64(NowMs() - store.startMs)));
    }

    // ---- Nature's Retaliation's lockout, per bearer --------------------------------------------------------------------

    constexpr char kLashKey[] = "EfPlantLash";

    struct Lash : DataMap::Base
    {
        std::map<ObjectGuid, uint64> last;   // caster -> when its trigger last struck for this bearer
    };
}

// ---- Lifesprout, Seeds of Renewal --------------------------------------------------------------------------------------

class spell_ef_kit_growth : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_growth);

    void Grow(Unit* caster, Unit* unit, uint32 first, int32 extraMs)
    {
        std::vector<uint32> const chain = Chain(first);
        if (chain.empty() || !unit->IsAlive())
            return;
        if (Aura* aura = StageOn(unit, caster, chain))
            Advance(caster, unit, aura, false);
        else
            Plant(caster, unit, first, extraMs, false);
    }

    void HandleDummy(SpellEffIndex effIndex)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target)
            return;
        SpellEffectInfo const& eff = GetSpellInfo()->Effects[effIndex];
        uint32 const first = eff.TriggerSpell;
        int32 const extra = GetEffectValue();
        if (eff.MiscValueB <= 0)
        {
            Grow(caster, target, first, extra);
            return;
        }
        // the party round the caster (Seeds of Renewal): each in sight, no pets
        for (Unit* u : Emberfall::ClassKit::PartyUnits(caster, caster, float(eff.MiscValueB), false))
            if (u == caster || caster->IsWithinLOSInMap(u))
                Grow(caster, u, first, extra);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_ef_kit_growth::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

class spell_ef_kit_growth_stage : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_growth_stage);

    void Ended(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_EXPIRE)
            return;
        Unit* caster = GetCaster();
        Unit* owner = GetUnitOwner();
        if (!caster || !owner || !caster->IsInWorld())
            return;
        Advance(caster, owner, GetAura(), true);
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(spell_ef_kit_growth_stage::Ended, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// ---- Nature's Embrace, the Bloom -------------------------------------------------------------------------------------

class spell_ef_kit_heal_echo : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_heal_echo);

    bool _first = true;
    int32 _heal = 0;

    Unit* Healer() { return GetOriginalCaster() ? GetOriginalCaster() : GetCaster(); }

    void Hit()
    {
        Unit* caster = Healer();
        Unit* target = GetHitUnit();
        if (!caster || !target)
            return;
        int32 heal = GetHitHeal();
        int32 pct = 0;
        for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
            if (uint32(eff->GetMiscValue()) == GetSpellInfo()->Id && !HasTrigger(eff) && eff->GetMiscValueB() > 0
                && target->GetHealthPct() < float(eff->GetMiscValueB()))
                pct += eff->GetAmount();
        if (pct > 0)
        {
            heal += Logic::PctOf(heal, pct);
            SetHitHeal(heal);
        }
        _heal = heal;
    }

    void AfterHitTarget()
    {
        Unit* caster = Healer();
        Unit* target = GetHitUnit();
        if (!caster || !target)
            return;
        struct Echo
        {
            uint32 trigger;
            int32 pct;
            int32 count;
        };
        std::vector<Echo> echoes;
        for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
        {
            if (uint32(eff->GetMiscValue()) != GetSpellInfo()->Id || eff->GetMiscValueB() <= 0 || !HasTrigger(eff))
                continue;
            uint32 const trigger = eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(trigger);
            if (info && info->Effects[EFFECT_0].Effect == SPELL_EFFECT_HEAL)
                echoes.push_back({ trigger, eff->GetAmount(), eff->GetMiscValueB() });
        }
        for (Echo const& e : echoes)
        {
            int32 const amount = Logic::PctOf(_heal, e.pct);
            SpellInfo const* info = sSpellMgr->GetSpellInfo(e.trigger);
            if (amount <= 0 || !info)
                continue;
            float const yards = float(std::max(1, info->Effects[EFFECT_1].CalcValue(caster)));
            std::vector<Unit*> others;
            for (Unit* u : Emberfall::ClassKit::PartyUnits(caster, target, yards, true))
                if (u != target && !u->IsFullHealth())
                    others.push_back(u);
            std::stable_sort(others.begin(), others.end(), [](Unit* a, Unit* b) { return a->GetHealthPct() < b->GetHealthPct(); });
            if (int32(others.size()) > e.count)
                others.resize(std::max(0, e.count));
            for (Unit* u : others)
                caster->CastCustomSpell(u, e.trigger, &amount, nullptr, nullptr, true);
        }
        Emberfall::ClassKit::HitBonuses(caster, target, GetSpellInfo()->Id, 0, _first);
        _first = false;
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_ef_kit_heal_echo::Hit);
        AfterHit += SpellHitFn(spell_ef_kit_heal_echo::AfterHitTarget);
    }
};

// ---- Rising Sap's mana -----------------------------------------------------------------------------------------------

class spell_ef_kit_mana_back : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_mana_back);

    void HandleEnergize(SpellEffIndex /*effIndex*/)
    {
        Unit* target = GetHitUnit();
        if (!target)
            return;
        SetEffectValue(Logic::ManaBack(target->GetMaxPower(POWER_MANA), GetEffectValue()));
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_ef_kit_mana_back::HandleEnergize, EFFECT_0, SPELL_EFFECT_ENERGIZE);
    }
};

// ---- Thornspitter --------------------------------------------------------------------------------------------------------

class spell_ef_kit_sentry : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_sentry);

    ObjectGuid _sentry;

    void Apply(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->IsInWorld())
            return;
        uint32 const entry = uint32(aurEff->GetMiscValue());
        SpellEffectInfo const& place = GetSpellInfo()->Effects[EFFECT_1];
        float const side = float(std::max(1, place.CalcValue(caster)));
        Position const pos = caster->GetNearPosition(side, float(M_PI) / 2.0f);
        TempSummon* sentry = caster->SummonCreature(entry, pos, TEMPSUMMON_TIMED_DESPAWN, uint32(std::max(1000, GetAura()->GetDuration() + 500)));
        if (!sentry)
            return;
        Configure(sentry, caster);
        sentry->SetFacingTo(caster->GetOrientation());
        _sentry = sentry->GetGUID();
        if (place.TriggerSpell)
            sentry->CastSpell(sentry, place.TriggerSpell, true);
    }

    void Tick(AuraEffect const* aurEff)
    {
        Unit* caster = GetCaster();
        Player* player = caster ? caster->ToPlayer() : nullptr;
        Creature* sentry = caster ? ObjectAccessor::GetCreature(*caster, _sentry) : nullptr;
        uint32 const shot = GetSpellInfo()->Effects[aurEff->GetEffIndex()].TriggerSpell;
        if (!player || !sentry || !shot)
            return;
        float const reach = float(std::max(1, aurEff->GetMiscValueB()));
        std::vector<Unit*> targets;
        for (Unit* mob : Emberfall::Companions::EngagedEnemies(player))
            if (sentry->IsWithinDistInMap(mob, reach) && sentry->IsWithinLOSInMap(mob) && caster->IsValidAttackTarget(mob)
                && !Emberfall::Companions::IsCrowdControlled(mob) && !mob->HasBreakableByDamageCrowdControlAura())
                targets.push_back(mob);
        if (targets.empty())
            return;  // nobody to shoot: it waits (its time runs on)
        Unit* target = targets[urand(0, uint32(targets.size() - 1))];
        sentry->SetFacingToObject(target);
        sentry->CastSpell(target, shot, TRIGGERED_FULL_MASK, nullptr, aurEff, caster->GetGUID());
    }

    void Remove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Unit* owner = GetUnitOwner())
            if (Creature* sentry = ObjectAccessor::GetCreature(*owner, _sentry))
                sentry->DespawnOrUnsummon();
        _sentry.Clear();
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_ef_kit_sentry::Apply, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_ef_kit_sentry::Tick, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_ef_kit_sentry::Remove, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// ---- Verdant Ancient -------------------------------------------------------------------------------------------------

class spell_ef_kit_stored_heal : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_stored_heal);

    bool _release = false;

    void Before()
    {
        Unit* caster = GetCaster();
        _release = caster && caster->HasAura(GetSpellInfo()->Id, caster->GetGUID());
    }

    // a press while the store is up releases it: the aura the hit refreshed goes at once (its script releases)
    void KeepAura(SpellEffIndex /*effIndex*/)
    {
        if (_release)
            PreventHitAura();
    }

    void Place(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        Unit* spot = GetHitUnit();
        if (!caster || !spot)
            return;
        if (_release)
            return;  // the aura's hook on the caster releases (KeepAura)
        SpellEffectInfo const& hold = GetSpellInfo()->Effects[EFFECT_0];
        Store* store = StoreOf(caster, true);
        if (store->active)
            Release(caster, *store);  // a store left over (it should have gone with its aura)
        int32 const durationMs = std::max(1000, GetSpellInfo()->GetMaxDuration());
        TempSummon* ancient = caster->SummonCreature(uint32(hold.MiscValueB), spot->GetPosition(), TEMPSUMMON_TIMED_DESPAWN,
                                                     uint32(durationMs + 1000));
        store->spell = GetSpellInfo()->Id;
        store->release = hold.TriggerSpell;
        store->pct = std::max(0, hold.CalcValue(caster));
        store->hundredths = 0;
        store->startMs = NowMs();
        store->active = true;
        store->creature = ObjectGuid::Empty;
        if (!ancient)
            return;
        Configure(ancient, caster);
        ancient->SetFacingToObject(caster);
        store->creature = ancient->GetGUID();
        if (uint32 const look = GetSpellInfo()->Effects[EFFECT_2].TriggerSpell)
            ancient->CastSpell(ancient, look, true);
    }

    // the cooldown counts from the summon and starts at the release: until then the button releases
    void After()
    {
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!player || _release)
            return;
        uint32 const spell = GetSpellInfo()->Id;
        player->RemoveSpellCooldown(spell, true);
        // our client starts the cooldown from the spell's row when the cast goes off, after this clear has arrived: clear
        // it again a moment later, while the store stands (the live check saw the button show 43 s)
        ObjectGuid const guid = player->GetGUID();
        player->m_Events.AddEventAtOffset([player, guid, spell]()
        {
            Store* store = StoreOf(player, false);
            if (player->IsInWorld() && player->GetGUID() == guid && store && store->active)
                player->RemoveSpellCooldown(spell, true);
        }, Milliseconds(300));
    }

    void Register() override
    {
        BeforeCast += SpellCastFn(spell_ef_kit_stored_heal::Before);
        OnEffectHitTarget += SpellEffectFn(spell_ef_kit_stored_heal::KeepAura, EFFECT_0, SPELL_EFFECT_APPLY_AURA);
        OnEffectHitTarget += SpellEffectFn(spell_ef_kit_stored_heal::Place, EFFECT_2, SPELL_EFFECT_DUMMY);
        AfterCast += SpellCastFn(spell_ef_kit_stored_heal::After);
    }
};

class spell_ef_kit_stored_heal_aura : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_stored_heal_aura);

    void Ended(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* owner = GetUnitOwner();
        Store* store = StoreOf(owner, false);
        if (!owner || !store || !store->active)
            return;
        if (GetTargetApplication()->GetRemoveMode() == AURA_REMOVE_BY_DEATH || !owner->IsInWorld())
        {
            // the druid fell (or left): the Ancient goes without a release
            store->active = false;
            if (Creature* ancient = ObjectAccessor::GetCreature(*owner, store->creature))
                ancient->DespawnOrUnsummon();
            store->creature.Clear();
            if (Player* player = owner->ToPlayer())
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(store->spell))
                    SetCooldown(player, store->spell, Logic::CooldownFromStart(int32(info->RecoveryTime),
                                                                               int64(NowMs() - store->startMs)));
            return;
        }
        Release(owner, *store);
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(spell_ef_kit_stored_heal_aura::Ended, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// ---- Becalm: the caster's poisons never break it ---------------------------------------------------------------------

class spell_ef_kit_clear_dots : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_clear_dots);

    void Landed(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Unit* target = GetUnitOwner())
            target->RemoveAurasByType(SPELL_AURA_PERIODIC_DAMAGE, GetCasterGUID());
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_ef_kit_clear_dots::Landed, EFFECT_0, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

// ---- the stores and the plants' lash ---------------------------------------------------------------------------------

class EmberfallBloomUnitScript : public UnitScript
{
public:
    EmberfallBloomUnitScript() : UnitScript("EmberfallBloomUnitScript", true,
        { UNITHOOK_MODIFY_HEAL_RECEIVED, UNITHOOK_MODIFY_MELEE_DAMAGE }) { }

    // Unit::HealBySpell calls this with the healer first (the unit it is called on) and the healed unit second
    // (Unit.cpp: sScriptMgr->ModifyHealReceived(this, healInfo.GetTarget(), ...)), whatever the hook's parameter names say.
    void ModifyHealReceived(Unit* healer, Unit* /*healed*/, uint32& heal, SpellInfo const* spellInfo) override
    {
        if (!healer || !spellInfo || spellInfo->SpellFamilyName != kDruidFamily || !heal)
            return;
        Store* store = StoreOf(healer, false);
        if (!store || !store->active || spellInfo->Id == store->release)
            return;
        store->hundredths += Logic::StoreHundredths(heal, store->pct);
    }

    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) override
    {
        if (!target || !attacker || !damage || attacker == target || !target->IsAlive())
            return;
        // the bearer's plant and shield auras from casters who know a lash naming them
        std::vector<AuraEffect*> held;
        for (AuraType type : { SPELL_AURA_PERIODIC_HEAL, SPELL_AURA_MOD_HEALING_RECEIVED })
            for (AuraEffect* eff : target->GetAuraEffectsByType(type))
                held.push_back(eff);
        for (AuraEffect* eff : held)
        {
            Unit* caster = eff->GetCaster();
            if (!caster || !caster->IsPlayer() || !caster->IsInWorld() || caster->GetMap() != target->GetMap())
                continue;
            uint32 const aura = eff->GetId();
            for (AuraEffect const* lash : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
            {
                if (uint32(lash->GetMiscValue()) != aura || !HasTrigger(lash))
                    continue;
                uint32 const trigger = lash->GetSpellInfo()->Effects[lash->GetEffIndex()].TriggerSpell;
                uint64 const gap = uint64(std::max(0, lash->GetMiscValueB()));
                Lash* lock = target->CustomData.GetDefault<Lash>(kLashKey);
                uint64 const now = NowMs();
                auto it = lock->last.find(caster->GetGUID());
                if (it != lock->last.end() && now < it->second + gap)
                    return;
                lock->last[caster->GetGUID()] = now;
                ObjectGuid const foe = attacker->GetGUID();
                // after the blow: never cast inside the melee calculation
                caster->m_Events.AddEventAtOffset([caster, foe, trigger]()
                {
                    Unit* attackerNow = ObjectAccessor::GetUnit(*caster, foe);
                    if (attackerNow && attackerNow->IsAlive() && caster->IsValidAttackTarget(attackerNow))
                        caster->CastSpell(attackerNow, trigger, true);
                }, Milliseconds(1));
                return;  // one lash per blow
            }
        }
    }
};

void AddEmberfallClassKitBloomScripts()
{
    RegisterSpellScript(spell_ef_kit_growth);
    RegisterSpellScript(spell_ef_kit_growth_stage);
    RegisterSpellScript(spell_ef_kit_heal_echo);
    RegisterSpellScript(spell_ef_kit_mana_back);
    RegisterSpellScript(spell_ef_kit_sentry);
    RegisterSpellAndAuraScriptPair(spell_ef_kit_stored_heal, spell_ef_kit_stored_heal_aura);
    RegisterSpellScript(spell_ef_kit_clear_dots);
    new EmberfallBloomUnitScript();
}
