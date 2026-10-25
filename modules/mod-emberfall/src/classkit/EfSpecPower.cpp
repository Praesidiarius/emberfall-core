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

// Emberfall class kits: a spec's own power (Emberfall docs/design/classes/druid-stormbringer.md §3, the Stormbringer's
// Static; the Mage's Pyromancer's Heat with its tiers and Overheat's hold), the end bonuses and the charges. EfClassKit.h says what
// each function does; the arithmetic is EfClassKitLogic.h (tests/classkit_tests.cpp).

#include "EfClassKit.h"
#include "EfClassKitLogic.h"
#include "EfTalents.h"

#include "DBCStores.h"
#include "DataMap.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Player.h"
#include "SharedDefines.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace Emberfall::ClassKit
{
    namespace
    {
        std::map<uint32, SpecPower> s_specPowers;   // by spec
        std::map<uint8, ClassPowerRule> s_classPowers;   // by class (the Hunter's Focus)
        std::set<uint32> s_fightMarkers;            // kit helper auras that last until cancelled
        constexpr char const* kStateKey = "ef_spec_power";

        // Per player: the spec power in force (none: the class's own) and the drain's clock.
        struct PowerState : public DataMap::Base
        {
            SpecPower const* power = nullptr;
            uint32 outOfCombatMs = 0;
            int64 carry = 0;
            bool inCombat = false;
            int32 holds = 0;                        // Overheat: nothing drains the power while > 0
            int64 regenCarry = 0;   // the class power's regeneration (millionths of a point)
            bool markerFight = false;               // in a fight as the fight markers saw it (every kit player)
            uint32 markerOutMs = 0;                 // out of that fight so long (the spec power's markers delay)
            int64 specRegenCarry = 0;   // a spec power's regeneration (millionths of a stored unit)
            int32 keptUnits = 0;    // a kept spec power (Openings) as it stood on the last tick (a level up empties it)
        };

        bool TableExists(char const* name)
        {
            QueryResult r = WorldDatabase.Query("SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() "
                                                "AND TABLE_NAME = '{}'", name);
            return r && r->Fetch()[0].Get<uint64>() == 1;
        }

        bool SpecPowerTableExists() { return TableExists("ef_class_kit_spec_power"); }

        // Every tier aura of every spec power of the player's class off the player (its power is the class's again).
        void RemoveTierAuras(Player* player)
        {
            for (auto const& [spec, p] : s_specPowers)
                if (p.classId == player->getClass())
                    for (SpecPower::Tier const& t : p.tiers)
                        player->RemoveAurasDueToSpell(t.aura);
        }

        // The tier auras follow the power (Blazing, Inferno, Maximum).
        void UpdateTiers(Player* player, SpecPower const& power)
        {
            if (power.tiers.empty())
                return;
            Powers const slot = Powers(power.power);
            int32 const stored = player->getPowerType() == slot ? int32(player->GetPower(slot)) : 0;
            for (SpecPower::Tier const& t : power.tiers)
            {
                bool const want = t.at > 0 && stored >= int32(t.at) && (!t.requiredSpell || player->HasSpell(t.requiredSpell));
                bool const has = player->HasAura(t.aura);
                if (want && !has)
                    player->AddAura(t.aura, player);
                else if (!want && has)
                    player->RemoveAurasDueToSpell(t.aura);
            }
        }

        // The player's class power rule when its power is that slot (no spec power in force), else nullptr.
        ClassPowerRule const* ActiveClassPower(Player const* player)
        {
            ClassPowerRule const* rule = ClassPowerOf(player->getClass());
            return rule && player->getPowerType() == Powers(rule->power) ? rule : nullptr;
        }

        Powers ClassPower(Player const* player)
        {
            ChrClassesEntry const* cls = sChrClassesStore.LookupEntry(player->getClass());
            return cls && cls->powerType < MAX_POWERS ? Powers(cls->powerType) : POWER_MANA;
        }
    }

    void ClearFightMarkers() { s_fightMarkers.clear(); }

    void AddFightMarker(uint32 spell) { s_fightMarkers.insert(spell); }

    void LoadSpecPowers()
    {
        s_specPowers.clear();
        if (!SpecPowerTableExists())
        {
            LOG_INFO("server.loading", ">> Emberfall class kits: no ef_class_kit_spec_power table (no spec has a power of its own)");
            return;
        }
        if (QueryResult r = WorldDatabase.Query("SELECT `spec`, `class`, `power`, `max_power`, `drain_delay_ms`, `drain_per_second` "
                                                "FROM `ef_class_kit_spec_power`"))
        {
            do
            {
                Field* f = r->Fetch();
                SpecPower p;
                p.spec = f[0].Get<uint32>();
                p.classId = f[1].Get<uint8>();
                p.power = f[2].Get<uint8>();
                p.max = f[3].Get<uint32>();
                p.drainDelayMs = f[4].Get<uint32>();
                p.drainPerSecond = f[5].Get<uint32>();
                if (p.power >= MAX_POWERS || p.power == POWER_MANA || p.power == POWER_RUNE)
                {
                    LOG_ERROR("server.loading", ">> Emberfall class kits: spec {}: power {} cannot be a spec's own power", p.spec, p.power);
                    continue;
                }
                s_specPowers[p.spec] = p;
                LOG_INFO("server.loading", ">> Emberfall class kits: spec {} (class {}) uses power {} (max {}, drains {} a second after {} ms)",
                         p.spec, p.classId, p.power, p.max, p.drainPerSecond, p.drainDelayMs);
            } while (r->NextRow());
        }
        // the regeneration, start and fight markers' delay (the Corsair's Energy)
        if (TableExists("ef_class_kit_spec_power_regen"))
            if (QueryResult r = WorldDatabase.Query("SELECT `spec`, `regen_per_second`, `start_full`, `markers_delay_ms` "
                                                    "FROM `ef_class_kit_spec_power_regen`"))
            {
                do
                {
                    Field* f = r->Fetch();
                    auto it = s_specPowers.find(f[0].Get<uint32>());
                    if (it == s_specPowers.end())
                    {
                        LOG_ERROR("server.loading", ">> Emberfall class kits: spec power regen of spec {}: no such spec power",
                                  f[0].Get<uint32>());
                        continue;
                    }
                    it->second.regenPerSecond = f[1].Get<uint32>();
                    // 0 empty, 1 full, 2 kept (the Executioner's Openings)
                    it->second.startFull = f[2].Get<uint8>() == 1;
                    it->second.keep = f[2].Get<uint8>() == 2;
                    it->second.markersDelayMs = f[3].Get<uint32>();
                    LOG_INFO("server.loading", ">> Emberfall class kits: spec {} regenerates {} a second{}, fight markers after {} ms",
                             it->first, it->second.regenPerSecond,
                             it->second.startFull ? ", starts full" : it->second.keep ? ", kept" : "",
                             it->second.markersDelayMs);
                } while (r->NextRow());
            }
        if (!TableExists("ef_class_kit_spec_power_tier"))
            return;
        if (QueryResult r = WorldDatabase.Query("SELECT `spec`, `at_power`, `aura`, `requires_spell` FROM `ef_class_kit_spec_power_tier` "
                                                "ORDER BY `spec`, `at_power`"))
        {
            uint32 count = 0;
            do
            {
                Field* f = r->Fetch();
                auto it = s_specPowers.find(f[0].Get<uint32>());
                SpecPower::Tier t;
                t.at = f[1].Get<uint32>();
                t.aura = f[2].Get<uint32>();
                t.requiredSpell = f[3].Get<uint32>();
                if (it == s_specPowers.end() || !sSpellMgr->GetSpellInfo(t.aura))
                {
                    LOG_ERROR("server.loading", ">> Emberfall class kits: spec power tier of spec {}: no such spec power or aura {}",
                              f[0].Get<uint32>(), t.aura);
                    continue;
                }
                it->second.tiers.push_back(t);
                // a tier aura lasts until the power falls below it, not until the fight ends
                s_fightMarkers.erase(t.aura);
                ++count;
            } while (r->NextRow());
            LOG_INFO("server.loading", ">> Emberfall class kits: {} spec power tiers", count);
        }
    }

    void LoadClassPowers()
    {
        s_classPowers.clear();
        if (!TableExists("ef_class_kit_class_power"))
        {
            LOG_INFO("server.loading", ">> Emberfall class kits: no ef_class_kit_class_power table (no class has a power of its own)");
            return;
        }
        if (QueryResult r = WorldDatabase.Query("SELECT `class`, `power`, `max_power`, `regen_per_second`, `regen_out_of_combat` "
                                                "FROM `ef_class_kit_class_power`"))
        {
            do
            {
                Field* f = r->Fetch();
                ClassPowerRule p;
                p.classId = f[0].Get<uint8>();
                p.power = f[1].Get<uint8>();
                p.max = f[2].Get<uint32>();
                p.regen = f[3].Get<float>();
                p.regenOutOfCombat = f[4].Get<float>();
                if (p.power >= MAX_POWERS || p.power == POWER_MANA || p.power == POWER_RUNE || p.power == POWER_HAPPINESS)
                {
                    LOG_ERROR("server.loading", ">> Emberfall class kits: class {}: power {} cannot be a class power", p.classId, p.power);
                    continue;
                }
                s_classPowers[p.classId] = p;
                LOG_INFO("server.loading", ">> Emberfall class kits: class {} uses power {} (max {}, {} a second in a fight, {} out of one)",
                         p.classId, p.power, p.max, p.regen, p.regenOutOfCombat);
            } while (r->NextRow());
        }
    }

    int32 SpecPowerMaxDelta(Player const* player, uint8 power)
    {
        if (!player || player->getPowerType() != Powers(power))
            return 0;
        SpecPower const* p = SpecPowerOf(Emberfall::Talents::ActiveSpec(const_cast<Player*>(player)));
        if (!p || p->classId != player->getClass() || p->power != power)
            return 0;
        return Logic::SpecPowerMaxDelta(int32(p->max), int32(player->GetCreatePowers(Powers(power))));
    }

    ClassPowerRule const* ClassPowerOf(uint8 classId)
    {
        auto it = s_classPowers.find(classId);
        return it != s_classPowers.end() ? &it->second : nullptr;
    }

    uint32 ClassPowerBase(Player const* player, uint8 power)
    {
        if (!player)
            return 0;
        ClassPowerRule const* rule = ClassPowerOf(player->getClass());
        return rule && rule->power == power ? rule->max : 0;
    }

    SpecPower const* SpecPowerOf(uint32 spec)
    {
        auto it = s_specPowers.find(spec);
        return it != s_specPowers.end() ? &it->second : nullptr;
    }

    int32 PowerScale(uint8 power)
    {
        return power == POWER_RAGE || power == POWER_RUNIC_POWER ? 10 : 1;
    }

    void ApplySpecPower(Player* player, bool login)
    {
        if (!player || !KitOf(player))
            return;
        SpecPower const* power = SpecPowerOf(Emberfall::Talents::ActiveSpec(player));
        if (power && power->classId != player->getClass())
            power = nullptr;
        PowerState* st = player->CustomData.GetDefault<PowerState>(kStateKey);
        if (power)
        {
            Powers const slot = Powers(power->power);
            bool const changed = player->getPowerType() != slot || st->power != power;
            if (player->getPowerType() != slot)
                player->setPowerType(slot);
            // the slot's own maximum (runic power: 1000 stored, the kit's `max: 100`) plus the max-power auras of the
            // slot (SPELL_AURA_MOD_INCREASE_ENERGY, misc the power: Brooding Sky +500 / +1000), the way every aura change
            // and stat update computes it, so neither undoes the other
            player->UpdateMaxPower(slot);
            if (changed)
            {
                // empty (Static, Heat) or full (the Corsair's Energy) at login and the spec choice; a kept power (the
                // Executioner's Openings) stays at login as the core loaded it (the slot is saved with the character)
                if (!(login && power->keep))
                    player->SetPower(slot, power->startFull ? player->GetMaxPower(slot) : 0);
                st->specRegenCarry = 0;
            }
            else if (power->keep)
            {
                // a level up: the core emptied the runic power slot (Player::InitStatsForLevel); Openings come back
                int32 const now = int32(player->GetPower(slot));
                int32 const kept = Logic::KeptPower(now, st->keptUnits, true);
                if (kept != now)
                    player->SetPower(slot, uint32(kept));
            }
            st->keptUnits = int32(player->GetPower(slot));
            st->power = power;
            return;
        }
        st->power = nullptr;
        RemoveTierAuras(player);
        Powers const own = ClassPower(player);
        if (player->getPowerType() != own)
        {
            player->setPowerType(own);
            player->UpdateMaxPower(own);
        }
        // a class power (the Hunter's Focus): its maximum (the core's create power is 0 for a player; the hook after
        // UpdateMaxPower adds the rule's) and full, at login, level up and spec change (the core sets Focus to 0 at all
        // three: Player::InitStatsForLevel, GiveLevel, setPowerType)
        if (ClassPowerRule const* rule = ClassPowerOf(player->getClass()); rule && Powers(rule->power) == own)
        {
            player->UpdateMaxPower(own);
            player->SetPower(own, player->GetMaxPower(own));
            st->regenCarry = 0;
        }
    }

    void UpdateSpecPower(Player* player, uint32 diff)
    {
        // a class power (the Hunter's Focus): regenerated every tick, in a fight at its rate times the haste factor and the
        // slot's regen-percent auras (Held Breath), out of one at its out-of-combat rate (every pull starts full)
        if (ClassPowerRule const* rule = ActiveClassPower(player); rule && player->IsAlive())
        {
            PowerState* cs = player->CustomData.GetDefault<PowerState>(kStateKey);
            Powers const slot = Powers(rule->power);
            if (player->GetPower(slot) < player->GetMaxPower(slot))
            {
                bool const fight = player->IsInCombat();
                float multiplier = 1.0f;
                if (fight)
                    multiplier = Logic::HasteFactor(player->GetFloatValue(UNIT_MOD_CAST_SPEED))
                        * player->GetTotalAuraMultiplierByMiscValue(SPELL_AURA_MOD_POWER_REGEN_PERCENT, slot);
                int32 const gain = Logic::Regen(fight ? rule->regen : rule->regenOutOfCombat, multiplier, diff, cs->regenCarry);
                if (gain > 0)
                    player->ModifyPower(slot, gain);
            }
            else
                cs->regenCarry = 0;
        }
        bool const fight = player->IsInCombat();
        // the fight markers end with the fight for every kit player, whatever its power (the Sunwarden's Dawnstrike combo
        // on mana; First Flash may double the next fight's first build again)
        if (!s_fightMarkers.empty())
        {
            PowerState* ms = player->CustomData.GetDefault<PowerState>(kStateKey);
            if (fight)
            {
                ms->markerFight = true;
                ms->markerOutMs = 0;
            }
            else if (ms->markerFight)
            {
                // at once, or after the spec power's markers delay (the Corsair's Flow: 10 s)
                ms->markerOutMs += diff;
                if (ms->markerOutMs >= (ms->power ? ms->power->markersDelayMs : 0))
                {
                    ms->markerFight = false;
                    for (uint32 marker : s_fightMarkers)
                        player->RemoveAurasDueToSpell(marker);
                }
            }
        }
        PowerState* st = player->CustomData.Get<PowerState>(kStateKey);
        if (!st || !st->power)
            return;
        UpdateTiers(player, *st->power);
        if (st->power->keep && player->getPowerType() == Powers(st->power->power))
            st->keptUnits = int32(player->GetPower(Powers(st->power->power)));
        // a spec power that regenerates (the Corsair's Energy): in and out of a fight, its rate times the slot's
        // regen-percent auras (Momentum +50 %), no haste
        if (st->power->regenPerSecond && player->IsAlive())
        {
            Powers const slot = Powers(st->power->power);
            if (player->getPowerType() == slot && player->GetPower(slot) < player->GetMaxPower(slot))
            {
                float const multiplier = player->GetTotalAuraMultiplierByMiscValue(SPELL_AURA_MOD_POWER_REGEN_PERCENT, slot);
                int32 const gain = Logic::Regen(float(st->power->regenPerSecond), multiplier, diff, st->specRegenCarry);
                if (gain > 0)
                    player->ModifyPower(slot, gain);
            }
            else
                st->specRegenCarry = 0;
        }
        if (fight)
        {
            st->inCombat = true;
            st->outOfCombatMs = 0;
            st->carry = 0;
            return;
        }
        if (st->inCombat)
            st->inCombat = false;
        st->outOfCombatMs += diff;
        if (st->holds > 0)
            return;
        Powers const slot = Powers(st->power->power);
        if (player->getPowerType() != slot || !player->GetPower(slot))
            return;
        float const multiplier = player->GetTotalAuraMultiplierByMiscValue(SPELL_AURA_MOD_POWER_REGEN_PERCENT, slot);
        // a larger pool (Brooding Sky) drains in proportion: as long to empty from full as the spec's own maximum
        uint32 const perSecond = Logic::DrainRate(st->power->drainPerSecond, player->GetMaxPower(slot), st->power->max);
        int32 const drain = Logic::Drain(perSecond, multiplier, diff, st->outOfCombatMs, st->power->drainDelayMs, st->carry);
        if (drain > 0)
            player->ModifyPower(slot, -drain);
    }

    int32 GainSpecPower(Unit* caster, int32 points, uint32 spellId)
    {
        Player* player = caster ? caster->ToPlayer() : nullptr;
        if (!player || points <= 0)
            return 0;
        PowerState* st = player->CustomData.Get<PowerState>(kStateKey);
        if (!st || !st->power || player->getPowerType() != Powers(st->power->power))
            return 0;
        int32 const units = points * PowerScale(st->power->power);
        player->EnergizeBySpell(player, spellId, units, Powers(st->power->power));
        return units;
    }

    void HoldSpecPower(Unit* owner, bool on)
    {
        Player* player = owner ? owner->ToPlayer() : nullptr;
        if (!player)
            return;
        PowerState* st = player->CustomData.GetDefault<PowerState>(kStateKey);
        st->holds = std::max(0, st->holds + (on ? 1 : -1));
    }

    void SetSpecPowerPoints(Unit* owner, int32 points)
    {
        Player* player = owner ? owner->ToPlayer() : nullptr;
        if (!player)
            return;
        PowerState* st = player->CustomData.Get<PowerState>(kStateKey);
        if (!st || !st->power || player->getPowerType() != Powers(st->power->power))
            return;
        player->SetPower(Powers(st->power->power), uint32(std::max(0, points) * PowerScale(st->power->power)));
    }

    int32 SpecPowerPoints(Unit* owner)
    {
        Player* player = owner ? owner->ToPlayer() : nullptr;
        if (!player)
            return 0;
        PowerState* st = player->CustomData.Get<PowerState>(kStateKey);
        if (!st || !st->power || player->getPowerType() != Powers(st->power->power))
            return 0;
        return int32(player->GetPower(Powers(st->power->power))) / PowerScale(st->power->power);
    }

    void EndBonuses(Unit* caster, Unit* owner, uint32 auraSpell)
    {
        if (!caster || !owner || !auraSpell)
            return;
        struct Bonus
        {
            uint32 trigger;
            int32 amount;
        };
        std::vector<Bonus> bonuses;
        for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
        {
            uint32 const trigger = eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell;
            if (uint32(eff->GetMiscValue()) == auraSpell && trigger && eff->GetBase()->IsPassive())
                bonuses.push_back({ trigger, eff->GetAmount() });
        }
        for (Bonus const& b : bonuses)
        {
            Unit* target = TargetsCaster(b.trigger) ? caster : owner;
            if (!target->IsAlive())
                continue;
            if (b.amount)
                caster->CastCustomSpell(target, b.trigger, &b.amount, nullptr, nullptr, true);
            else
                caster->CastSpell(target, b.trigger, true);
        }
    }

    namespace
    {
        // The recharge time of a charged spell: its cooldown with the caster's spell mods (Falling Air).
        int32 RechargeMs(Player* player, uint32 spell)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
            if (!info)
                return 0;
            int32 ms = int32(std::max(info->RecoveryTime, info->CategoryRecoveryTime));
            player->ApplySpellMod(spell, SPELLMOD_COOLDOWN, ms);
            return std::max(0, ms);
        }

        // The spell a charge aura recharges: its effect 1's misc value.
        uint32 ChargedSpell(uint32 chargeAura)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(chargeAura);
            return info ? uint32(info->Effects[EFFECT_0].MiscValue) : 0;
        }
    }

    void SpendCharge(Player* player, uint32 spell, uint32 chargeAura, uint32 max)
    {
        if (!player || !spell || !chargeAura || max < 2)
            return;
        int32 const recharge = RechargeMs(player, spell);
        Aura* aura = player->GetAura(chargeAura, player->GetGUID());
        uint32 const before = aura ? aura->GetStackAmount() : 0;
        Logic::ChargeSpend const spend = Logic::SpendCharge(before, max);
        if (!aura)
        {
            aura = player->AddAura(chargeAura, player);
            if (!aura)
                return;
            aura->SetMaxDuration(recharge);
            aura->SetDuration(recharge);
        }
        if (aura->GetStackAmount() != spend.recharging)
            aura->SetStackAmount(uint8(spend.recharging));
        if (spend.left)
        {
            // a charge is left: the spell can be cast again now
            player->RemoveSpellCooldown(spell, true);
            return;
        }
        // none left: the cooldown ends with the next charge (the charge aura's time left)
        int32 const left = aura->GetDuration();
        int32 const delta = left - recharge;
        if (delta < 0)
            player->ModifySpellCooldown(spell, delta);
    }

    void ChargeBack(Unit* owner, uint32 chargeAura, uint32 stacksBefore)
    {
        Player* player = owner ? owner->ToPlayer() : nullptr;
        if (!player || stacksBefore <= 1)
            return;
        // more were recharging: the next starts now
        uint32 const spell = ChargedSpell(chargeAura);
        int32 const recharge = RechargeMs(player, spell);
        ObjectGuid const guid = player->GetGUID();
        player->m_Events.AddEventAtOffset([player, guid, chargeAura, stacksBefore, recharge]()
        {
            if (!player->IsInWorld() || player->GetGUID() != guid)
                return;
            Aura* aura = player->GetAura(chargeAura, guid);
            if (!aura)
                aura = player->AddAura(chargeAura, player);
            if (!aura)
                return;
            aura->SetStackAmount(uint8(stacksBefore - 1));
            aura->SetMaxDuration(recharge);
            aura->SetDuration(recharge);
        }, Milliseconds(1));
    }
}
