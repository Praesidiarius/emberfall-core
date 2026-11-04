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
// from Seed to Sprout to Bloom, a tree that stores healing, a thorn plant that shoots on its own; ankarna's handoff of
// 2026-11-03, §14 there). Generic scripts, their numbers in the spells' own effects (Emberfall's classkit/model.py
// SCRIPTS checks the contracts); the arithmetic is EfClassKitLogic.h (tests/classkit_tests.cpp).
//
// A growth chain is a row of stage auras ending in a hit: each stage's effect 1 is a periodic heal (an ally's plant) or
// periodic damage (Withering Bloom's plant on an enemy) and its effect 2 a dummy whose trigger is the next stage (Seed ->
// Sprout) or, for the last stage, the spell cast when it ends (Sprout -> Bloom: a heal, or the withering burst). The first
// stage's effect 2 value is the cycle in ms (18 s): the chain runs on one clock, so pushing a stage does not reset it (a
// stage's effect 2 amount, set by the script, is what was left of the cycle when the stage began).
//
//   spell_ef_kit_growth         effect 1 a dummy whose trigger is the first stage: at its unit target (an ally), or with
//                               misc B > 0 at every party member within misc B yards (EfClassKit.h PartyUnits: the
//                               caster, its group or raid subgroup and their companions, no pets; line of sight to each)
//                               no stage of the caster's yet -> the first stage (its cycle + the effect's value); a stage
//                               -> the next one at once (Lifesprout pushed: Seed -> Sprout, Sprout -> Bloom) (Lifesprout,
//                               Seeds of Renewal). Caster passives naming the spell: a dummy without a trigger adds its
//                               value in percent to the Blooms it pushes (Abundant Renewal); one whose trigger is the
//                               chain's first stage plants a fresh one where it pushed a Bloom (Evergreen Renewal); one
//                               whose trigger is a damaging chain's first stage lets the spell go on the hostile unit the
//                               caster has selected when the cast comes at the caster (the client aims a helpful spell at
//                               its caster when an enemy is selected): that chain grows there (Withering Bloom)
//   spell_ef_kit_growth_stage   a stage aura: when it runs out the next stage starts with what is left of the cycle, the
//                               last one casts its spell at the owner. Caster passives: a dummy naming the last stage with
//                               misc B > 0 and no trigger sends a heal on an owner at full health to the most injured
//                               party member within misc B yards of it (Wandering Bloom); a dummy naming the first stage
//                               with no trigger, misc B a health percent and value a lockout in seconds blooms the plant
//                               at once when a blow leaves its owner alive below that percent, once per lockout per owner
//                               (Nature's Intervention; the unit script below)
//   spell_ef_kit_plant_gift     every dummy effect at an ally whose trigger is a growth chain's first stage plants it when
//                               the ally has no stage of that chain from the caster (misc: a spell the caster must hold:
//                               Cocoon's Gift on Verdant Cocoon); no charge, no push
//   spell_ef_kit_heal_echo      a heal (effect 1); the caster's passive dummies naming it: no trigger and misc B > 0, its
//                               value in percent more healing on an ally below misc B percent health; a trigger that heals
//                               and misc B > 0, value percent of the heal cast as the trigger at up to misc B other injured
//                               party members, most injured first, within the trigger's effect 2 value in yards of the
//                               ally (Shared Vitality); then the hit bonuses (EfClassKit.h HitBonuses, self triggers once:
//                               Rising Sap's mana on every Bloom) (Nature's Embrace, Lifesprout's Bloom)
//   spell_ef_kit_mana_back      effect 1 (energize of mana on the caster): its value in tenths of a percent of the
//                               caster's max mana (Rising Sap: 20)
//   spell_ef_kit_sentry         an aura on the caster: effect 1 a periodic dummy with misc = the creature, misc B = its
//                               reach in yards, trigger = the shot; effect 2 a dummy with value = yards beside the caster
//                               where it stands and trigger = its look aura. While the aura holds the creature stands
//                               there (untargetable, unkillable, the caster's level and faction) and every tick shoots
//                               the trigger at a random enemy within its reach and sight that is already fighting the
//                               caster's group, never one held by crowd control that damage breaks; the shot is the
//                               caster's (spell power, threat, the combat log). A cast while one stands (a second charge:
//                               Thornstock) raises a second one on the other side, each lasting the aura's duration from
//                               its own summon; they go with the aura (Thornspitter)
//   spell_ef_kit_stored_heal    (a spell and its own aura) effect 1 a dummy aura on the caster: value = the tenths of a
//                               percent of the caster's raw healing it stores (overhealing included; spell mods: Ancient
//                               Reservoir), misc B = the creature, trigger = the release heal (its effect 2 value: the
//                               release radius); effect 3 a dummy at an ally: where the creature stands, trigger = its look
//                               aura. A cast while the aura holds releases instead of summoning. The release (pressed,
//                               cancelled or the aura's end) heals the injured party members within the radius of the
//                               creature, lowest health percent first, each what it misses until the store is spent,
//                               ignoring line of sight; the cooldown counts from the summon. Caster passives naming the
//                               spell: with a trigger, cast at every ally the release heals (Ancient's Shelter); without
//                               one and misc B 0, the plants (growth stages that heal) of the caster's on the party within
//                               the radius tick value times at once at the summon (Ancient Awakening); without one and
//                               misc B > 0, they tick once more every misc B ms while the creature stands (Ancient
//                               Communion) (Verdant Ancient)
//   spell_ef_kit_field_echo     after a hit: every caster dummy naming the spell with a trigger whose TargetAuraSpell (a
//                               field's aura) the target carries from the caster casts the trigger with value percent of
//                               the hit's damage at every enemy carrying that aura (Lashing Vines on Thornbolt)
//   spell_ef_kit_field_end      a persistent area spell: every caster dummy naming it with a trigger casts the trigger at
//                               the field's place when the field runs out (Final Bloom on Thornbloom)
//   spell_ef_kit_clear_dots     a hold that damage breaks: when it lands, the caster's own periodic damage on the target
//                               goes, so the caster's poisons never break it (Becalm and Poisonous Spines)
//   spell_ef_kit_fine_pct       a marker on a passive: each dummy effect, its value in tenths of a percent more of the
//                               owner's heals and periodic damage its class mask covers, on the final amount (Flourishing
//                               Roots' and Deep Restoration's +7.5 / +15 %)
//
// A unit script feeds the stores (every heal of a kit spell of the druid family while the caster's store is up: direct
// heals and, since 2026-11-04, every plant's tick on any ally), adds the half percents and runs Nature's Intervention (a
// blow that leaves an ally with a plant below the capstone's percent).

#include "EfClassKit.h"
#include "EfClassKitLogic.h"
#include "EfCompanions.h"

#include "CellImpl.h"
#include "Creature.h"
#include "DataMap.h"
#include "DynamicObject.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
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
#include <list>
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

    // A stage: effect 1 a periodic heal (an ally's plant) or periodic damage (an enemy's: Withering Bloom), effect 2 a dummy
    // whose trigger is what comes next.
    bool IsStage(SpellInfo const* info)
    {
        if (!info)
            return false;
        AuraType const ticks = info->Effects[EFFECT_0].ApplyAuraName;
        return (ticks == SPELL_AURA_PERIODIC_HEAL || ticks == SPELL_AURA_PERIODIC_DAMAGE)
            && info->Effects[EFFECT_1].ApplyAuraName == SPELL_AURA_DUMMY && info->Effects[EFFECT_1].TriggerSpell;
    }

    bool IsDamagingStage(SpellInfo const* info)
    {
        return IsStage(info) && info->Effects[EFFECT_0].ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE;
    }

    // A chain's first stage: its effect 2 value is the cycle (the stages after it start at 0).
    bool IsFirstStage(uint32 spell)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
        return IsStage(info) && info->Effects[EFFECT_1].CalcValue() > 0;
    }

    // The stage auras of the chain that starts at `first`, in order (the spell that ends it is not in the list).
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

    uint32 TriggerOf(AuraEffect const* eff)
    {
        return eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell;
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
    void Plant(Unit* caster, Unit* unit, uint32 first, int32 extraMs)
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
            clock->ChangeAmount(cycle, false);
        // the first stage never outlasts the cycle (a cycle shorter than the Seed: it blooms at the cycle's end)
        if (aura->GetDuration() > cycle)
        {
            aura->SetMaxDuration(cycle);
            aura->SetDuration(cycle);
        }
    }

    // A Bloom pushed by Seeds of Renewal is cast with Abundant Renewal's percent more: the heal's own script reads it while
    // the (triggered, instant) cast runs.
    struct PendingBonus
    {
        uint32 spell = 0;
        int32 pct = 0;
    };
    PendingBonus s_bloomBonus;

    // The last stage ended or was pushed: the spell it carries (the Bloom; Withering Bloom's burst) at the owner, or
    // (Wandering Bloom, heals only) at the most injured party member near an owner at full health.
    void Bloom(Unit* caster, Unit* owner, SpellInfo const* last, int32 bonusPct)
    {
        uint32 const hit = last->Effects[EFFECT_1].TriggerSpell;
        SpellInfo const* hitInfo = sSpellMgr->GetSpellInfo(hit);
        if (!hitInfo || !owner->IsInWorld())
            return;
        Unit* target = owner;
        if (hitInfo->Effects[EFFECT_0].Effect == SPELL_EFFECT_HEAL && owner->IsFullHealth())
        {
            int32 yards = 0;
            for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
                if (uint32(eff->GetMiscValue()) == last->Id && !HasTrigger(eff) && eff->GetMiscValueB() > 0)
                    yards = std::max(yards, eff->GetMiscValueB());
            if (yards > 0)
                if (Unit* other = MostInjuredNear(caster, owner, float(yards)))
                    target = other;
        }
        if (!target->IsAlive())
            return;
        s_bloomBonus = { hit, std::max(0, bonusPct) };
        caster->CastSpell(target, hit, true);
        s_bloomBonus = {};
    }

    // The stage `aura` on `owner` moves on: the next stage with what is left of the cycle, or the Bloom.
    void Advance(Unit* caster, Unit* owner, Aura* aura, bool natural, int32 bonusPct = 0)
    {
        SpellInfo const* info = aura->GetSpellInfo();
        AuraEffect const* clock = aura->GetEffect(EFFECT_1);
        int32 const amount = clock ? std::abs(clock->GetAmount()) : 0;
        int32 const elapsed = natural ? aura->GetMaxDuration() : std::max(0, aura->GetMaxDuration() - aura->GetDuration());
        int32 const left = Logic::GrowthLeft(amount, elapsed);
        uint32 const next = info->Effects[EFFECT_1].TriggerSpell;
        SpellInfo const* nextInfo = sSpellMgr->GetSpellInfo(next);
        if (!natural)
            owner->RemoveAura(aura, AURA_REMOVE_BY_DEFAULT);
        if (!IsStage(nextInfo))
        {
            Bloom(caster, owner, info, bonusPct);
            return;
        }
        SpellInfo const* afterNext = sSpellMgr->GetSpellInfo(nextInfo->Effects[EFFECT_1].TriggerSpell);
        bool const last = !IsStage(afterNext);
        int32 const ms = Logic::StageMs(left, nextInfo->GetMaxDuration(), last);
        if (ms <= 0 || !owner->IsAlive())
        {
            // no time left for the next stage: it blooms at once (its spell, if it is the last)
            if (last && owner->IsAlive())
                Bloom(caster, owner, nextInfo, bonusPct);
            return;
        }
        caster->CastSpell(owner, next, true);
        Aura* nextAura = owner->GetAura(next, caster->GetGUID());
        if (!nextAura)
            return;
        nextAura->SetMaxDuration(ms);
        nextAura->SetDuration(ms);
        if (AuraEffect* nextClock = nextAura->GetEffect(EFFECT_1))
            nextClock->ChangeAmount(left, false);
    }

    // Nature's Intervention: the plant of the chain starting at `first` on `unit` blooms now, at full strength.
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
            Bloom(caster, unit, last, 0);
    }

    // One more tick of every healing plant of the caster's on the party within `radius` of `center` (Ancient Awakening,
    // Ancient Communion): the plant's own heal, its growth and clock untouched.
    void ExtraTicks(Unit* caster, WorldObject const* center, float radius, int32 times)
    {
        if (times <= 0)
            return;
        for (Unit* u : Emberfall::ClassKit::PartyUnits(caster, center, radius, true))
        {
            std::vector<AuraEffect*> plants;
            for (AuraEffect* eff : u->GetAuraEffectsByType(SPELL_AURA_PERIODIC_HEAL))
                if (eff->GetEffIndex() == EFFECT_0 && eff->GetCasterGUID() == caster->GetGUID() && IsStage(eff->GetSpellInfo()))
                    plants.push_back(eff);
            for (AuraEffect* eff : plants)
                if (AuraApplication* app = eff->GetBase()->GetApplicationOfTarget(u->GetGUID()))
                    for (int32 i = 0; i < times && u->IsAlive(); ++i)
                        eff->PeriodicTick(app, caster);
        }
    }

    // ---- the stored heal (Verdant Ancient) ----------------------------------------------------------------------------

    constexpr char kStoreKey[] = "EfStoredHeal";

    struct Store : DataMap::Base
    {
        uint32 spell = 0;          // the spell whose aura holds the store
        uint32 release = 0;        // the release heal (never stored itself)
        ObjectGuid creature;       // the Ancient standing in the world
        uint64 hundredths = 0;     // what it holds, in hundredths of a point
        int32 tenths = 0;          // the share of every raw heal it stores, in tenths of a percent
        uint64 startMs = 0;        // the summon (also the token of its Communion ticks)
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

    float ReleaseRadius(Unit* caster, Store const& store)
    {
        SpellInfo const* release = sSpellMgr->GetSpellInfo(store.release);
        return release ? float(std::max(1, release->Effects[EFFECT_1].CalcValue(caster))) : 40.0f;
    }

    WorldObject const* StoreCenter(Unit* caster, Store const& store)
    {
        Creature* ancient = ObjectAccessor::GetCreature(*caster, store.creature);
        return ancient ? static_cast<WorldObject const*>(ancient) : caster;
    }

    // Ancient Communion: while the store stands (the summon `token`), every plant in its reach ticks once more every `ms`.
    void CommunionAfter(Unit* caster, uint64 token, int32 delayMs, int32 everyMs)
    {
        caster->m_Events.AddEventAtOffset([caster, token, everyMs]()
        {
            Store* store = StoreOf(caster, false);
            if (!caster->IsInWorld() || !store || !store->active || store->startMs != token)
                return;
            ExtraTicks(caster, StoreCenter(caster, *store), ReleaseRadius(caster, *store), 1);
            CommunionAfter(caster, token, everyMs, everyMs);
        }, Milliseconds(std::max(100, delayMs)));
    }

    void Release(Unit* caster, Store& store)
    {
        if (!store.active)
            return;
        store.active = false;
        Creature* ancient = ObjectAccessor::GetCreature(*caster, store.creature);
        WorldObject const* center = ancient ? static_cast<WorldObject const*>(ancient) : caster;
        float const radius = ReleaseRadius(caster, store);
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
        // Ancient's Shelter: a caster dummy naming the store's spell with a trigger: cast at every ally the release heals
        std::vector<uint32> shelters;
        for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
            if (uint32(eff->GetMiscValue()) == store.spell && HasTrigger(eff))
                shelters.push_back(TriggerOf(eff));
        if (sSpellMgr->GetSpellInfo(store.release))
            for (std::size_t i = 0; i < injured.size(); ++i)
                if (out[i] > 0)
                {
                    int32 const bp = int32(out[i]);
                    caster->CastCustomSpell(injured[i], store.release, &bp, nullptr, nullptr, true);
                    for (uint32 shelter : shelters)
                        if (injured[i]->IsAlive())
                            caster->CastSpell(injured[i], shelter, true);
                }
        if (ancient)
            ancient->DespawnOrUnsummon();
        store.creature.Clear();
        if (Player* player = caster->ToPlayer())
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(store.spell))
                SetCooldown(player, store.spell, Logic::CooldownFromStart(int32(info->RecoveryTime),
                                                                           int64(NowMs() - store.startMs)));
    }

    // ---- Nature's Intervention's lockout, per bearer --------------------------------------------------------------------

    constexpr char kLockoutKey[] = "EfPlantLockout";

    struct Lockout : DataMap::Base
    {
        std::map<ObjectGuid, uint64> last;   // caster -> when its plant last bloomed by itself for this bearer
    };
}

// ---- Lifesprout, Seeds of Renewal --------------------------------------------------------------------------------------

class spell_ef_kit_growth : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_growth);

    ObjectGuid _enemy;          // Withering Bloom: the enemy the plant goes on instead
    uint32 _enemyFirst = 0;     // its damaging chain

    // Withering Bloom: a caster passive naming this spell whose trigger is a damaging chain's first stage, or 0.
    uint32 WitheringFirst(Unit* caster)
    {
        for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
            if (uint32(eff->GetMiscValue()) == GetSpellInfo()->Id && HasTrigger(eff)
                && IsDamagingStage(sSpellMgr->GetSpellInfo(TriggerOf(eff))))
                return TriggerOf(eff);
        return 0;
    }

    // Seeds of Renewal's Abundant Renewal (no trigger: percent more on the Blooms it pushes).
    int32 BloomBonus(Unit* caster)
    {
        int32 pct = 0;
        for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
            if (uint32(eff->GetMiscValue()) == GetSpellInfo()->Id && !HasTrigger(eff) && eff->GetMiscValueB() == 0)
                pct += eff->GetAmount();
        return pct;
    }

    // Evergreen Renewal (the chain's first stage as the trigger): plants it again where the spell pushed a Bloom.
    bool Replants(Unit* caster, uint32 first)
    {
        for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
            if (uint32(eff->GetMiscValue()) == GetSpellInfo()->Id && TriggerOf(eff) == first)
                return true;
        return false;
    }

    void Grow(Unit* caster, Unit* unit, uint32 first, int32 extraMs, bool party)
    {
        std::vector<uint32> const chain = Chain(first);
        if (chain.empty() || !unit->IsAlive())
            return;
        Aura* aura = StageOn(unit, caster, chain);
        if (!aura)
        {
            Plant(caster, unit, first, extraMs);
            return;
        }
        bool const blooms = !IsStage(sSpellMgr->GetSpellInfo(aura->GetSpellInfo()->Effects[EFFECT_1].TriggerSpell));
        Advance(caster, unit, aura, false, party ? BloomBonus(caster) : 0);
        if (party && blooms && unit->IsAlive() && !StageOn(unit, caster, chain) && Replants(caster, first))
            Plant(caster, unit, first, extraMs);
    }

    // The client aims a helpful spell at its caster while an enemy is selected; with Withering Bloom the plant goes on that
    // enemy instead (in range and sight, or the cast is refused).
    SpellCastResult Check()
    {
        _enemy.Clear();
        _enemyFirst = 0;
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!player || GetSpellInfo()->Effects[EFFECT_0].MiscValueB > 0)
            return SPELL_CAST_OK;
        Unit* aimed = GetExplTargetUnit();
        if (aimed && aimed != player)
            return SPELL_CAST_OK;
        uint32 const first = WitheringFirst(player);
        if (!first)
            return SPELL_CAST_OK;
        Unit* selected = ObjectAccessor::GetUnit(*player, player->GetTarget());
        if (!selected || selected == player || !selected->IsAlive() || !player->IsValidAttackTarget(selected))
            return SPELL_CAST_OK;
        float const range = std::max(GetSpellInfo()->GetMaxRange(false), GetSpellInfo()->GetMaxRange(true));
        if (!player->IsWithinCombatRange(selected, range))
            return SPELL_FAILED_OUT_OF_RANGE;
        if (!player->IsWithinLOSInMap(selected))
            return SPELL_FAILED_LINE_OF_SIGHT;
        _enemy = selected->GetGUID();
        _enemyFirst = first;
        return SPELL_CAST_OK;
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
        if (_enemyFirst)
        {
            Unit* enemy = ObjectAccessor::GetUnit(*caster, _enemy);
            if (enemy && enemy->IsAlive() && caster->IsValidAttackTarget(enemy))
                Grow(caster, enemy, _enemyFirst, 0, false);
            return;
        }
        if (eff.MiscValueB <= 0)
        {
            Grow(caster, target, first, extra, false);
            return;
        }
        // the party round the caster (Seeds of Renewal): each in sight, no pets
        for (Unit* u : Emberfall::ClassKit::PartyUnits(caster, caster, float(eff.MiscValueB), false))
            if (u == caster || caster->IsWithinLOSInMap(u))
                Grow(caster, u, first, extra, true);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_ef_kit_growth::Check);
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

// ---- Cocoon's Gift: a plant where there is none ------------------------------------------------------------------------

class spell_ef_kit_plant_gift : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_plant_gift);

    void HandleDummy(SpellEffIndex effIndex)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        SpellEffectInfo const& eff = GetSpellInfo()->Effects[effIndex];
        if (!caster || !target || !IsFirstStage(eff.TriggerSpell) || !target->IsAlive())
            return;
        if (eff.MiscValue > 0 && !caster->HasAura(uint32(eff.MiscValue)))
            return;
        if (StageOn(target, caster, Chain(eff.TriggerSpell)))
            return;
        Plant(caster, target, eff.TriggerSpell, 0);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_ef_kit_plant_gift::HandleDummy, EFFECT_ALL, SPELL_EFFECT_DUMMY);
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
        if (s_bloomBonus.spell == GetSpellInfo()->Id)
            pct += s_bloomBonus.pct;   // Abundant Renewal on a Bloom of Seeds of Renewal
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
            uint32 const trigger = TriggerOf(eff);
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

    std::vector<ObjectGuid> _sentries;

    void Raise(AuraEffect const* aurEff)
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->IsInWorld())
            return;
        uint32 const entry = uint32(aurEff->GetMiscValue());
        SpellEffectInfo const& place = GetSpellInfo()->Effects[EFFECT_1];
        float const side = float(std::max(1, place.CalcValue(caster)));
        // the first on the left, a second (Thornstock's charge) on the right
        float const angle = (_sentries.size() % 2 ? -1.0f : 1.0f) * float(M_PI) / 2.0f;
        Position const pos = caster->GetNearPosition(side, angle);
        int32 const lasts = std::max(1000, GetAura()->GetMaxDuration());
        TempSummon* sentry = caster->SummonCreature(entry, pos, TEMPSUMMON_TIMED_DESPAWN, uint32(lasts + 500));
        if (!sentry)
            return;
        Configure(sentry, caster);
        sentry->SetFacingTo(caster->GetOrientation());
        _sentries.push_back(sentry->GetGUID());
        if (place.TriggerSpell)
            sentry->CastSpell(sentry, place.TriggerSpell, true);
    }

    void Apply(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
    {
        Raise(aurEff);
    }

    // cast again while the aura holds (a second charge): another plant beside the first
    void Again(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
    {
        Raise(aurEff);
    }

    void Tick(AuraEffect const* aurEff)
    {
        Unit* caster = GetCaster();
        Player* player = caster ? caster->ToPlayer() : nullptr;
        uint32 const shot = GetSpellInfo()->Effects[aurEff->GetEffIndex()].TriggerSpell;
        if (!player || !shot)
            return;
        float const reach = float(std::max(1, aurEff->GetMiscValueB()));
        std::vector<Unit*> const engaged = Emberfall::Companions::EngagedEnemies(player);
        for (ObjectGuid const& guid : _sentries)
        {
            Creature* sentry = ObjectAccessor::GetCreature(*caster, guid);
            if (!sentry || !sentry->IsAlive())
                continue;  // its time is up (each lasts the duration from its own summon)
            std::vector<Unit*> targets;
            for (Unit* mob : engaged)
                if (sentry->IsWithinDistInMap(mob, reach) && sentry->IsWithinLOSInMap(mob) && caster->IsValidAttackTarget(mob)
                    && !Emberfall::Companions::IsCrowdControlled(mob) && !mob->HasBreakableByDamageCrowdControlAura())
                    targets.push_back(mob);
            if (targets.empty())
                continue;  // nobody to shoot: it waits (its time runs on)
            Unit* target = targets[urand(0, uint32(targets.size() - 1))];
            sentry->SetFacingToObject(target);
            sentry->CastSpell(target, shot, TRIGGERED_FULL_MASK, nullptr, aurEff, caster->GetGUID());
        }
    }

    void Remove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Unit* owner = GetUnitOwner())
            for (ObjectGuid const& guid : _sentries)
                if (Creature* sentry = ObjectAccessor::GetCreature(*owner, guid))
                    sentry->DespawnOrUnsummon();
        _sentries.clear();
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_ef_kit_sentry::Apply, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectApply += AuraEffectApplyFn(spell_ef_kit_sentry::Again, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAPPLY);
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
        store->tenths = std::max(0, hold.CalcValue(caster));
        store->hundredths = 0;
        store->startMs = NowMs();
        store->active = true;
        store->creature = ObjectGuid::Empty;
        if (ancient)
        {
            Configure(ancient, caster);
            ancient->SetFacingToObject(caster);
            store->creature = ancient->GetGUID();
            if (uint32 const look = GetSpellInfo()->Effects[EFFECT_2].TriggerSpell)
                ancient->CastSpell(ancient, look, true);
        }
        // Ancient Awakening (misc B 0: ticks at the summon) and Ancient Communion (misc B: one more tick every so many ms)
        int32 awaken = 0;
        int32 every = 0;
        for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
        {
            if (uint32(eff->GetMiscValue()) != store->spell || HasTrigger(eff))
                continue;
            if (eff->GetMiscValueB() > 0)
                every = every ? std::min(every, eff->GetMiscValueB()) : eff->GetMiscValueB();
            else
                awaken += eff->GetAmount();
        }
        ExtraTicks(caster, StoreCenter(caster, *store), ReleaseRadius(caster, *store), awaken);
        if (every > 0)
            CommunionAfter(caster, store->startMs, every / 2, every);
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

// ---- Lashing Vines: a hit on an enemy in the caster's field strikes everyone in it -------------------------------------

class spell_ef_kit_field_echo : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_field_echo);

    void AfterHitTarget()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        int32 const damage = GetHitDamage();
        if (!caster || !target || damage <= 0)
            return;
        struct Echo
        {
            uint32 strike;
            uint32 field;
            int32 pct;
        };
        std::vector<Echo> echoes;
        for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
        {
            if (uint32(eff->GetMiscValue()) != GetSpellInfo()->Id || !HasTrigger(eff))
                continue;
            SpellInfo const* strike = sSpellMgr->GetSpellInfo(TriggerOf(eff));
            if (strike && strike->TargetAuraSpell && target->HasAura(strike->TargetAuraSpell, caster->GetGUID()))
                echoes.push_back({ strike->Id, strike->TargetAuraSpell, eff->GetAmount() });
        }
        if (echoes.empty())
            return;
        float constexpr reach = 40.0f;  // wider than any field: who stands in it is told by its aura
        std::list<Unit*> near;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(target, caster, reach);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(target, near, check);
        Cell::VisitObjects(target, searcher, reach);
        if (std::find(near.begin(), near.end(), target) == near.end())
            near.push_back(target);
        for (Echo const& e : echoes)
        {
            int32 const amount = Logic::PctOf(damage, e.pct);
            if (amount <= 0)
                continue;
            for (Unit* u : near)
                if (u->IsAlive() && u->HasAura(e.field, caster->GetGUID()) && caster->IsValidAttackTarget(u))
                    caster->CastCustomSpell(u, e.strike, &amount, nullptr, nullptr, true);
        }
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_ef_kit_field_echo::AfterHitTarget);
    }
};

// ---- Final Bloom: the field's last burst ---------------------------------------------------------------------------------

class spell_ef_kit_field_end : public SpellScript
{
    PrepareSpellScript(spell_ef_kit_field_end);

    void After()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;
        uint32 const id = GetSpellInfo()->Id;
        std::vector<uint32> bursts;
        for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
            if (uint32(eff->GetMiscValue()) == id && HasTrigger(eff))
                bursts.push_back(TriggerOf(eff));
        DynamicObject* field = caster->GetDynObject(id);
        if (bursts.empty() || !field)
            return;
        ObjectGuid const fieldGuid = field->GetGUID();
        // a moment before it goes, while it still stands where it was placed
        int32 const delay = std::max(100, field->GetDuration() - 100);
        caster->m_Events.AddEventAtOffset([caster, id, fieldGuid, bursts]()
        {
            DynamicObject* now = caster->IsInWorld() ? caster->GetDynObject(id) : nullptr;
            if (!now || now->GetGUID() != fieldGuid)
                return;
            for (uint32 burst : bursts)
                caster->CastSpell(now->GetPositionX(), now->GetPositionY(), now->GetPositionZ(), burst, true);
        }, Milliseconds(delay));
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_ef_kit_field_end::After);
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

// ---- half percents: Flourishing Roots, Deep Restoration ---------------------------------------------------------------

// A marker on a passive (ankarna's handoff of 2026-11-03 asks +7.5 / +15 %, which a spell mod's whole percent cannot
// hold): every dummy effect of it makes the owner's heals and periodic damage of the spells its class mask covers its
// value in tenths of a percent stronger, on the final amount (spell power, crits and the target's bonuses in). The bloom
// unit script below applies it.
class spell_ef_kit_fine_pct : public AuraScript
{
    PrepareAuraScript(spell_ef_kit_fine_pct);

    void Register() override { }
};

namespace
{
    constexpr char kFinePct[] = "spell_ef_kit_fine_pct";

    void FineBonus(Unit* owner, uint32& amount, SpellInfo const* info)
    {
        if (!owner || !info || !amount || info->SpellFamilyName != kDruidFamily)
            return;
        int64 tenths = 0;
        for (AuraEffect const* eff : owner->GetAuraEffectsByType(SPELL_AURA_DUMMY))
            if (eff->GetCasterGUID() == owner->GetGUID() && eff->IsAffectedOnSpell(info) && eff->GetBase()->GetScriptByName(kFinePct))
                tenths += eff->GetAmount();
        if (tenths > 0)
            amount = uint32(std::min<int64>(int64(amount) + int64(amount) * tenths / 1000, INT32_MAX));
    }

    bool TicksHealing(SpellInfo const* info)
    {
        for (SpellEffectInfo const& eff : info->Effects)
            if (eff.ApplyAuraName == SPELL_AURA_PERIODIC_HEAL)
                return true;
        return false;
    }

    // A periodic heal's tick calls the heal hook right after the periodic hook, with the healed unit first
    // (SpellAuraEffects.cpp HandlePeriodicHealAurasTick); a direct heal only the heal hook, the healer first
    // (Unit::HealBySpell). The tick is handled in the periodic hook (the caster is known there), its heal hook skipped.
    bool s_tickHandled = false;
}

// ---- the stores and Nature's Intervention ----------------------------------------------------------------------------

class EmberfallBloomUnitScript : public UnitScript
{
public:
    EmberfallBloomUnitScript() : UnitScript("EmberfallBloomUnitScript", true,
        { UNITHOOK_MODIFY_HEAL_RECEIVED, UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK, UNITHOOK_ON_DAMAGE }) { }

    // A heal of the druid's: the half percents, then the store (until 2026-11-04 a plant's tick on another ally fed no
    // store: the tick's heal hook names the healed unit first)
    static void Healed(Unit* healer, uint32& heal, SpellInfo const* spellInfo)
    {
        if (!healer || !spellInfo || spellInfo->SpellFamilyName != kDruidFamily || !heal)
            return;
        FineBonus(healer, heal, spellInfo);
        Store* store = StoreOf(healer, false);
        if (!store || !store->active || spellInfo->Id == store->release)
            return;
        store->hundredths += Logic::StoreHundredths(heal, store->tenths);
    }

    void ModifyPeriodicDamageAurasTick(Unit* /*target*/, Unit* attacker, uint32& amount, SpellInfo const* spellInfo) override
    {
        if (!spellInfo)
            return;
        if (TicksHealing(spellInfo))
        {
            s_tickHandled = true;
            Healed(attacker, amount, spellInfo);
            return;
        }
        FineBonus(attacker, amount, spellInfo);   // Withering Bloom's Seed and Sprout
    }

    // Unit::HealBySpell calls this with the healer first (the unit it is called on) and the healed unit second
    // (Unit.cpp: sScriptMgr->ModifyHealReceived(this, healInfo.GetTarget(), ...)), whatever the hook's parameter names say.
    void ModifyHealReceived(Unit* healer, Unit* /*healed*/, uint32& heal, SpellInfo const* spellInfo) override
    {
        if (s_tickHandled)
        {
            s_tickHandled = false;   // a periodic tick's second call: handled above
            return;
        }
        Healed(healer, heal, spellInfo);
    }

    // Nature's Intervention: a blow that leaves an ally with a plant alive below the capstone's percent blooms it at once
    // (a caster dummy naming the chain's first stage, no trigger, misc B the percent, value the lockout per ally in s).
    void OnDamage(Unit* /*attacker*/, Unit* victim, uint32& damage) override
    {
        if (!victim || !damage || !victim->IsAlive() || !victim->IsInWorld())
            return;
        std::vector<AuraEffect*> plants;
        for (AuraEffect* eff : victim->GetAuraEffectsByType(SPELL_AURA_PERIODIC_HEAL))
            if (eff->GetEffIndex() == EFFECT_0 && IsStage(eff->GetSpellInfo()))
                plants.push_back(eff);
        for (AuraEffect* plant : plants)
        {
            Unit* caster = plant->GetCaster();
            if (!caster || !caster->IsPlayer() || !caster->IsInWorld() || caster->GetMap() != victim->GetMap())
                continue;
            for (AuraEffect const* rule : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
            {
                uint32 const first = uint32(rule->GetMiscValue());
                int32 const belowPct = rule->GetMiscValueB();
                if (HasTrigger(rule) || belowPct <= 0 || belowPct >= 100 || !IsFirstStage(first) || !ChainHas(first, plant->GetId()))
                    continue;
                if (!Logic::LowAfterBlow(victim->GetHealth(), damage, victim->GetMaxHealth(), belowPct))
                    continue;
                Lockout* lock = victim->CustomData.GetDefault<Lockout>(kLockoutKey);
                uint64 const now = NowMs();
                auto it = lock->last.find(caster->GetGUID());
                if (!Logic::LockoutOver(it != lock->last.end() ? it->second : 0, uint64(std::max(0, rule->GetAmount())) * 1000, now))
                    continue;
                lock->last[caster->GetGUID()] = now;
                ObjectGuid const bearer = victim->GetGUID();
                // after the blow: never heal inside the damage calculation
                caster->m_Events.AddEventAtOffset([caster, bearer, first]()
                {
                    Unit* ally = ObjectAccessor::GetUnit(*caster, bearer);
                    if (ally && ally->IsAlive())
                        BloomNow(caster, ally, first);
                }, Milliseconds(1));
                return;  // one per blow
            }
        }
    }
};

void AddEmberfallClassKitBloomScripts()
{
    RegisterSpellScript(spell_ef_kit_growth);
    RegisterSpellScript(spell_ef_kit_growth_stage);
    RegisterSpellScript(spell_ef_kit_plant_gift);
    RegisterSpellScript(spell_ef_kit_heal_echo);
    RegisterSpellScript(spell_ef_kit_mana_back);
    RegisterSpellScript(spell_ef_kit_sentry);
    RegisterSpellAndAuraScriptPair(spell_ef_kit_stored_heal, spell_ef_kit_stored_heal_aura);
    RegisterSpellScript(spell_ef_kit_field_echo);
    RegisterSpellScript(spell_ef_kit_field_end);
    RegisterSpellScript(spell_ef_kit_clear_dots);
    RegisterSpellScript(spell_ef_kit_fine_pct);
    new EmberfallBloomUnitScript();
}
