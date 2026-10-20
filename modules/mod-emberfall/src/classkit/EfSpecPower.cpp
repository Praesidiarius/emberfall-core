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
// Static; the Mage's Pyromancer's Heat is meant to use it too), the end bonuses and the charges. EfClassKit.h says what
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

#include <map>
#include <set>
#include <vector>

namespace Emberfall::ClassKit
{
    namespace
    {
        std::map<uint32, SpecPower> s_specPowers;   // by spec
        std::set<uint32> s_fightMarkers;            // kit helper auras that last until cancelled
        constexpr char const* kStateKey = "ef_spec_power";

        // Per player: the spec power in force (none: the class's own) and the drain's clock.
        struct PowerState : public DataMap::Base
        {
            SpecPower const* power = nullptr;
            uint32 outOfCombatMs = 0;
            int64 carry = 0;
            bool inCombat = false;
        };

        bool SpecPowerTableExists()
        {
            QueryResult r = WorldDatabase.Query("SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() "
                                                "AND TABLE_NAME = 'ef_class_kit_spec_power'");
            return r && r->Fetch()[0].Get<uint64>() == 1;
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

    void ApplySpecPower(Player* player)
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
            player->SetMaxPower(slot, power->max);
            if (changed)
                player->SetPower(slot, 0);
            st->power = power;
            return;
        }
        st->power = nullptr;
        Powers const own = ClassPower(player);
        if (player->getPowerType() != own)
        {
            player->setPowerType(own);
            player->UpdateMaxPower(own);
        }
    }

    void UpdateSpecPower(Player* player, uint32 diff)
    {
        PowerState* st = player->CustomData.Get<PowerState>(kStateKey);
        if (!st || !st->power)
            return;
        bool const fight = player->IsInCombat();
        if (fight)
        {
            st->inCombat = true;
            st->outOfCombatMs = 0;
            st->carry = 0;
            return;
        }
        if (st->inCombat)
        {
            // the fight ended: its markers go (First Flash may double the next fight's first build again)
            st->inCombat = false;
            for (uint32 marker : s_fightMarkers)
                player->RemoveAurasDueToSpell(marker);
        }
        st->outOfCombatMs += diff;
        Powers const slot = Powers(st->power->power);
        if (player->getPowerType() != slot || !player->GetPower(slot))
            return;
        float const multiplier = player->GetTotalAuraMultiplierByMiscValue(SPELL_AURA_MOD_POWER_REGEN_PERCENT, slot);
        int32 const drain = Logic::Drain(st->power->drainPerSecond, multiplier, diff, st->outOfCombatMs, st->power->drainDelayMs,
                                         st->carry);
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
