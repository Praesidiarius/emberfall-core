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

#include "EfClassKit.h"

#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Player.h"
#include "SharedDefines.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"

#include <algorithm>
#include <map>
#include <set>

namespace Emberfall::ClassKit
{
    namespace
    {
        bool s_enabled = false;
        std::map<uint8, Kit> s_kits; // by class

        char const* SourceName(Source s)
        {
            switch (s)
            {
                case Source::Start: return "start";
                case Source::Trainer: return "trainer";
                case Source::Spec: return "spec";
                case Source::Talent: return "talent";
                case Source::Helper: return "helper";
            }
            return "?";
        }

        bool TablesExist()
        {
            QueryResult r = WorldDatabase.Query("SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() "
                                                "AND TABLE_NAME IN ('ef_class_kit', 'ef_class_kit_spell')");
            return r && r->Fetch()[0].Get<uint64>() == 2;
        }
    }

    void ReadConfig(bool reload)
    {
        bool const was = s_enabled;
        s_enabled = sConfigMgr->GetOption<bool>("Emberfall.ClassKit.Enable", false, false);
        if (reload && was != s_enabled)
        {
            LOG_INFO("server.loading", ">> Emberfall class kits: Emberfall.ClassKit.Enable {} -> {} (config reload)", was ? 1 : 0, s_enabled ? 1 : 0);
            Load();
        }
    }

    bool Enabled() { return s_enabled && !s_kits.empty(); }

    bool Load()
    {
        s_kits.clear();
        if (!s_enabled)
        {
            LOG_INFO("server.loading", ">> Emberfall class kits: off (Emberfall.ClassKit.Enable = 0)");
            return false;
        }
        if (!TablesExist())
        {
            LOG_ERROR("server.loading", ">> Emberfall class kits: the ef_class_kit tables are missing (modules/mod-emberfall/data/sql/db-world); "
                                        "the class kits stay off");
            return false;
        }
        if (QueryResult r = WorldDatabase.Query("SELECT `class`, `skill_line`, `stock_family` FROM `ef_class_kit`"))
        {
            do
            {
                Field* f = r->Fetch();
                Kit kit;
                kit.classId = f[0].Get<uint8>();
                kit.skillLine = f[1].Get<uint32>();
                kit.stockFamily = f[2].Get<uint32>();
                s_kits[kit.classId] = kit;
            } while (r->NextRow());
        }
        uint32 problems = 0;
        if (QueryResult r = WorldDatabase.Query("SELECT `class`, `spell`, `level`, `source` FROM `ef_class_kit_spell` ORDER BY `class`, `level`, `spell`"))
        {
            do
            {
                Field* f = r->Fetch();
                uint8 const classId = f[0].Get<uint8>();
                auto kit = s_kits.find(classId);
                KitSpell s{ f[1].Get<uint32>(), f[2].Get<uint8>(), Source(f[3].Get<uint8>()) };
                if (kit == s_kits.end())
                {
                    LOG_ERROR("server.loading", ">> Emberfall class kits: ef_class_kit_spell names class {} without an ef_class_kit row", classId);
                    ++problems;
                    continue;
                }
                SpellInfo const* info = sSpellMgr->GetSpellInfo(s.spell);
                if (!info)
                {
                    LOG_ERROR("server.loading", ">> Emberfall class kits: class {}: spell {} does not exist (spell_dbc)", classId, s.spell);
                    ++problems;
                    continue;
                }
                if (info->SpellFamilyName == kit->second.stockFamily && kit->second.stockFamily != SPELLFAMILY_GENERIC)
                {
                    LOG_ERROR("server.loading", ">> Emberfall class kits: class {}: spell {} is in the stock family {} the kit replaces "
                                                "(it would be purged)", classId, s.spell, kit->second.stockFamily);
                    ++problems;
                }
                kit->second.spells.push_back(s);
            } while (r->NextRow());
        }
        for (auto const& [classId, kit] : s_kits)
        {
            if (kit.skillLine && !sSkillLineStore.LookupEntry(kit.skillLine))
            {
                LOG_ERROR("server.loading", ">> Emberfall class kits: class {}: skill line {} does not exist (skillline_dbc)", classId, kit.skillLine);
                ++problems;
            }
            uint32 counts[6] = {};
            for (KitSpell const& s : kit.spells)
                if (uint8(s.source) < 6)
                    ++counts[uint8(s.source)];
            LOG_INFO("server.loading", ">> Emberfall class kits: class {}: skill line {}, replaces stock family {}, {} spells ({} start, {} trainer, "
                                       "{} spec, {} talent, {} helper)", classId, kit.skillLine, kit.stockFamily, kit.spells.size(),
                     counts[1], counts[2], counts[3], counts[4], counts[5]);
        }
        if (s_kits.empty())
            LOG_INFO("server.loading", ">> Emberfall class kits: on, no kits in ef_class_kit");
        else if (problems)
            LOG_ERROR("server.loading", ">> Emberfall class kits: {} problem(s) in the ef_class_kit tables", problems);
        return !s_kits.empty();
    }

    Kit const* KitOf(Player const* player)
    {
        if (!s_enabled || !player)
            return nullptr;
        auto it = s_kits.find(player->getClass());
        return it != s_kits.end() ? &it->second : nullptr;
    }

    uint32 Purge(Player* player, std::vector<std::string>* removed)
    {
        Kit const* kit = KitOf(player);
        if (!kit)
            return 0;
        uint32 count = 0;

        // 1. Stock class skill lines (category class) other than the kit's: their SkillLineAbility rows would hand
        //    the stock spells back at the next login (Player::learnSkillRewardedSpells).
        //    A class's own line is one with a SkillRaceClassInfo row naming that class alone; class-category lines every
        //    class has (mounts, companions: rows naming many classes) stay.
        for (uint32 i = 0; i < sSkillLineStore.GetNumRows(); ++i)
        {
            SkillLineEntry const* line = sSkillLineStore.LookupEntry(i);
            if (!line || line->categoryId != SKILL_CATEGORY_CLASS || line->id == kit->skillLine || !player->HasSkill(line->id))
                continue;
            SkillRaceClassInfoEntry const* info = GetSkillRaceClassInfo(line->id, player->getRace(), player->getClass());
            if (!info || info->ClassMask != player->getClassMask())
                continue;
            player->SetSkill(line->id, 0, 0, 0);
            ++count;
            if (removed)
                removed->push_back(Acore::StringFormat("skill line {}", line->id));
        }

        // 2. Spells of the stock family (the stock class spells and stock talent spells; the kit's own spells carry
        //    their own family, engine and racial spells are generic).
        std::vector<uint32> spells;
        if (kit->stockFamily != SPELLFAMILY_GENERIC)
            for (auto const& [spellId, ps] : player->GetSpellMap())
            {
                if (ps->State == PLAYERSPELL_REMOVED)
                    continue;
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId))
                    if (info->SpellFamilyName == kit->stockFamily)
                        spells.push_back(spellId);
            }
        for (uint32 spellId : spells)
        {
            player->removeSpell(spellId, SPEC_MASK_ALL, false);
            ++count;
            if (removed)
                removed->push_back(Acore::StringFormat("spell {}", spellId));
        }

        // 3. Auras of the stock family on the character (a stance the stock start casts, playercreateinfo_cast_spell).
        if (kit->stockFamily != SPELLFAMILY_GENERIC)
        {
            std::vector<uint32> auras;
            for (auto const& [auraId, app] : player->GetAppliedAuras())
                if (app->GetBase()->GetSpellInfo()->SpellFamilyName == kit->stockFamily)
                    auras.push_back(auraId);
            for (uint32 auraId : auras)
            {
                if (!player->HasAura(auraId))
                    continue;
                player->RemoveAurasDueToSpell(auraId);
                ++count;
                if (removed)
                    removed->push_back(Acore::StringFormat("aura {}", auraId));
            }
        }

        // 4. Buttons that still point at a spell the character does not know (the stock start buttons).
        bool buttons = false;
        for (uint8 button = 0; button < MAX_ACTION_BUTTONS; ++button)
        {
            ActionButton const* ab = player->GetActionButton(button);
            if (!ab || ab->GetType() != ACTION_BUTTON_SPELL || player->HasSpell(ab->GetAction()))
                continue;
            if (removed)
                removed->push_back(Acore::StringFormat("button {} (spell {})", button, ab->GetAction()));
            player->removeActionButton(button);
            buttons = true;
            ++count;
        }
        if (buttons && player->IsInWorld())
            player->SendActionButtons(1);
        return count;
    }

    namespace
    {
        // The caster's threshold passives for `stackSpell`: (stacks needed, trigger spell, amount).
        struct Threshold
        {
            int32 stacks;
            uint32 trigger;
            int32 amount;
        };

        std::vector<Threshold> Thresholds(Unit* caster, uint32 stackSpell)
        {
            std::vector<Threshold> out;
            for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
            {
                SpellInfo const* info = eff->GetSpellInfo();
                uint32 const trigger = info->Effects[eff->GetEffIndex()].TriggerSpell;
                if (uint32(eff->GetMiscValue()) == stackSpell && eff->GetMiscValueB() > 0 && trigger)
                    out.push_back({ eff->GetMiscValueB(), trigger, eff->GetAmount() });
            }
            return out;
        }

        void FollowThresholds(Unit* caster, Unit* target, uint32 stackSpell, uint32 stacks)
        {
            for (Threshold const& t : Thresholds(caster, stackSpell))
            {
                if (int32(stacks) >= t.stacks)
                {
                    int32 const bp = -t.amount;
                    caster->CastCustomSpell(target, t.trigger, &bp, nullptr, nullptr, true);
                }
                else
                    target->RemoveAurasDueToSpell(t.trigger, caster->GetGUID());
            }
        }
    }

    uint32 AddStacks(Unit* caster, Unit* target, uint32 stackSpell, int32 count)
    {
        if (!caster || !target || count <= 0)
            return 0;
        Aura* aura = target->GetAura(stackSpell, caster->GetGUID());
        if (!aura)
        {
            aura = caster->AddAura(stackSpell, target);
            if (!aura)
                return 0;
            --count;
        }
        if (count > 0)
            aura->ModStackAmount(count);
        else
            aura->RefreshDuration();
        uint32 const stacks = aura->GetStackAmount();
        FollowThresholds(caster, target, stackSpell, stacks);
        return stacks;
    }

    uint32 TakeStacks(Unit* caster, Unit* target, uint32 stackSpell, uint32 count)
    {
        if (!caster || !target)
            return 0;
        Aura* aura = target->GetAura(stackSpell, caster->GetGUID());
        if (!aura)
            return 0;
        uint32 const have = aura->GetStackAmount();
        uint32 const take = count ? std::min(count, have) : have;
        if (take >= have)
            aura->Remove();
        else
            aura->ModStackAmount(-int32(take));
        FollowThresholds(caster, target, stackSpell, have - take);
        return take;
    }

    void AddToShell(Unit* caster, uint32 shellSpell, int32 add, int32 durationMs)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(shellSpell);
        if (!caster || !info || add <= 0)
            return;
        // the shell's own spell mods: effect 1's scale the amount (Winter's Embrace), effect 2's the cap (CalcValue
        // applies them: Deep Shell, Winter's Embrace)
        if (Player* modOwner = caster->GetSpellModOwner())
            modOwner->ApplySpellMod(shellSpell, SPELLMOD_EFFECT1, add);
        int32 const capPct = info->Effects[EFFECT_1].CalcValue(caster);
        int32 const cap = int32(caster->CountPctFromMaxHealth(capPct > 0 ? capPct : 100));
        Aura* aura = caster->GetAura(shellSpell, caster->GetGUID());
        if (!aura)
        {
            caster->CastSpell(caster, shellSpell, true);
            aura = caster->GetAura(shellSpell, caster->GetGUID());
            if (!aura)
                return;
            aura->SetMaxDuration(durationMs);
            aura->SetDuration(durationMs);
            if (AuraEffect* eff = aura->GetEffect(EFFECT_0))
                eff->ChangeAmount(std::min(cap, add));
        }
        else
        {
            if (AuraEffect* eff = aura->GetEffect(EFFECT_0))
                eff->ChangeAmount(std::min(cap, eff->GetAmount() + add));
            if (aura->GetDuration() < durationMs)
            {
                aura->SetMaxDuration(std::max(aura->GetMaxDuration(), durationMs));
                aura->SetDuration(durationMs);
            }
        }
        RefreshConditionals(caster);
    }

    void HitBonuses(Unit* caster, Unit* target, uint32 spellId, uint32 stacks)
    {
        if (!caster || !target || !spellId)
            return;
        struct Bonus
        {
            uint32 trigger;
            int32 chance;
        };
        std::vector<Bonus> bonuses;
        for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
        {
            uint32 const trigger = eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell;
            if (uint32(eff->GetMiscValue()) == spellId && trigger && eff->GetMiscValueB() >= 0 && stacks >= uint32(eff->GetMiscValueB()))
                bonuses.push_back({ trigger, eff->GetAmount() });
        }
        for (Bonus const& b : bonuses)
        {
            if (b.chance > 0 && b.chance < 100 && !roll_chance_i(b.chance))
                continue;
            if (!target->IsAlive())
                return;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(b.trigger);
            if (!info)
                continue;
            if (info->StackAmount > 1)
                AddStacks(caster, target, b.trigger, 1);
            else
                caster->CastSpell(target, b.trigger, true);
        }
    }

    void ExtendByStacks(Unit* caster, uint32 stackSpell, uint32 consumed)
    {
        if (!caster || !consumed)
            return;
        std::vector<std::pair<Aura*, AuraEffect const*>> storms;
        for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
        {
            Aura* aura = eff->GetBase();
            if (uint32(eff->GetMiscValue()) == stackSpell && eff->GetMiscValueB() > 0 && !aura->IsPassive()
                && !eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell && aura->GetDuration() > 0)
                storms.emplace_back(aura, eff);
        }
        for (auto const& [aura, eff] : storms)
        {
            int32 const base = aura->GetSpellInfo()->GetMaxDuration();
            int32 const room = eff->GetMiscValueB() * IN_MILLISECONDS - std::max(0, aura->GetMaxDuration() - base);
            int32 const add = std::min(room, int32(consumed) * std::max(0, eff->GetAmount()));
            if (add <= 0)
                continue;
            aura->SetMaxDuration(aura->GetMaxDuration() + add);
            aura->SetDuration(aura->GetDuration() + add);
        }
    }

    void RefreshConditionals(Unit* unit)
    {
        if (!unit)
            return;
        std::vector<Aura*> auras;
        for (AuraEffect const* eff : unit->GetAuraEffectsByType(SPELL_AURA_PERIODIC_DUMMY))
            if (eff->GetEffIndex() == EFFECT_0 && (eff->GetMiscValue() || eff->GetMiscValueB()) && eff->GetBase()->IsPassive())
                auras.push_back(eff->GetBase());
        for (Aura* aura : auras)
            for (uint8 i = EFFECT_1; i < MAX_SPELL_EFFECTS; ++i)
                if (AuraEffect* eff = aura->GetEffect(i))
                    eff->RecalculateAmount(unit);
    }

    bool IsBoss(Unit const* unit)
    {
        Creature const* creature = unit ? unit->ToCreature() : nullptr;
        if (!creature)
            return false;
        return creature->isWorldBoss() || creature->IsDungeonBoss() || creature->GetCreatureTemplate()->rank == CREATURE_ELITE_WORLDBOSS;
    }

    namespace
    {
        void PurgeAndLog(Player* player, char const* when)
        {
            std::vector<std::string> removed;
            if (uint32 const n = Purge(player, &removed))
            {
                std::string list;
                for (std::string const& r : removed)
                    list += (list.empty() ? "" : ", ") + r;
                LOG_INFO("module", "Emberfall class kits: {} ({}): {} of the stock kit taken away at {}: {}", player->GetName(),
                         player->GetGUID().GetCounter(), n, when, list);
            }
        }
    }

    void OnLogin(Player* player) { PurgeAndLog(player, "login"); }

    void OnLevelChanged(Player* player) { PurgeAndLog(player, "level change"); }

    void Dump(Player* player, std::vector<std::string>& lines)
    {
        Kit const* kit = KitOf(player);
        if (!kit)
        {
            lines.push_back(Acore::StringFormat("Emberfall class kits: {} for class {} ({} kits loaded).",
                                                s_enabled ? "no kit" : "off", player->getClass(), s_kits.size()));
            return;
        }
        lines.push_back(Acore::StringFormat("Class kit of class {}: skill line {} ({}), replaces stock family {}, {} spells.", kit->classId,
                                            kit->skillLine, player->HasSkill(kit->skillLine) ? "known" : "not known", kit->stockFamily,
                                            kit->spells.size()));
        for (KitSpell const& s : kit->spells)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(s.spell);
            lines.push_back(Acore::StringFormat("  {} {} (level {}, {}): {}", s.spell, info ? info->SpellName[0] : "?", s.level,
                                                SourceName(s.source), player->HasSpell(s.spell) ? "known" : "-"));
        }
        uint32 stock = 0;
        for (auto const& [spellId, ps] : player->GetSpellMap())
            if (ps->State != PLAYERSPELL_REMOVED)
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId))
                    if (kit->stockFamily != SPELLFAMILY_GENERIC && info->SpellFamilyName == kit->stockFamily)
                        ++stock;
        lines.push_back(Acore::StringFormat("Stock family spells still known: {}.", stock));
    }
    void DumpAuras(Unit* unit, Player* viewer, std::vector<std::string>& lines)
    {
        if (!unit)
            return;
        std::set<uint32> kitSpells;
        for (auto const& [classId, kit] : s_kits)
            for (KitSpell const& s : kit.spells)
                kitSpells.insert(s.spell);
        uint32 n = 0;
        for (auto const& [auraId, app] : unit->GetAppliedAuras())
        {
            Aura const* aura = app->GetBase();
            if (!kitSpells.count(aura->GetId()))
                continue;
            int32 amounts[MAX_SPELL_EFFECTS] = {};
            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                if (AuraEffect const* eff = aura->GetEffect(i))
                    amounts[i] = eff->GetAmount();
            lines.push_back(Acore::StringFormat("EFAURA {} {} {} {} {} {} {}", aura->GetId(), uint32(aura->GetStackAmount()), amounts[0],
                                                amounts[1], amounts[2], aura->GetDuration(),
                                                viewer && aura->GetCasterGUID() == viewer->GetGUID() ? 1 : 0));
            ++n;
        }
        // the server's cooldowns of the kit spells (the client starts a cooldown from its own data, without spell mods)
        if (Player* player = unit->ToPlayer())
            for (uint32 spell : kitSpells)
                if (player->HasSpellCooldown(spell))
                    lines.push_back(Acore::StringFormat("EFAURA cd {} {}", spell, player->GetSpellCooldownDelay(spell)));
        lines.push_back(Acore::StringFormat("EFAURA end {} {} health {} {} combat {}", unit->GetName(), n, unit->GetHealth(), unit->GetMaxHealth(),
                                            unit->IsInCombat() ? 1 : 0));
    }
}
