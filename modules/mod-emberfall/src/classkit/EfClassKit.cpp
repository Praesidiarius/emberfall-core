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
#include "EfClassKitLogic.h"
#include "EfCompanions.h"

#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DataMap.h"
#include "DatabaseEnv.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SharedDefines.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "StringFormat.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>

namespace Emberfall::ClassKit
{
    namespace
    {
        bool s_enabled = false;
        std::map<uint8, Kit> s_kits; // by class
        // The shell auras (IsShellSpell): written by Load and AddToShell, read by the absorb reports (map threads).
        std::set<uint32> s_shellSpells;
        std::mutex s_shellMutex;
        // The stack-cooldown passives (spell_ef_kit_stack_cooldown: Winter's Embrace), written by Load only.
        std::set<uint32> s_stackCooldownSpells;

        // A player's cuts of a stack-cooldown spell's current cooldown (CooldownByStacks): how much was taken off it and
        // where it then ended, so a new cooldown (another end) starts from nothing. Rides on the player (CustomData).
        struct StackCooldownCuts : public DataMap::Base
        {
            struct Cut
            {
                uint32 end = 0;
                int32 takenMs = 0;
            };
            std::map<uint32, Cut> bySpell;
        };

        void AddShellSpell(uint32 spellId)
        {
            std::lock_guard<std::mutex> lock(s_shellMutex);
            s_shellSpells.insert(spellId);
        }

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
        s_stackCooldownSpells.clear();
        ClearFightMarkers();
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
                // a helper aura that lasts until cancelled marks the fight (First Flash Spent): it goes when the fight
                // ends (EfSpecPower.cpp); not one that stays even through death (the Executioner's Growing Precision:
                // its stacks are kept out of combat, SPELL_ATTR3_ALLOW_AURA_WHILE_DEAD)
                if (s.source == Source::Helper && info->GetMaxDuration() == -1 && !info->IsPassive()
                    && !info->HasAttribute(SPELL_ATTR3_ALLOW_AURA_WHILE_DEAD))
                    AddFightMarker(s.spell);
                // a shatter spell's effect 3 trigger is a shell aura (IsShellSpell), known before any AddToShell (a
                // shell kept over a relog)
                SpellScriptsBounds bounds = sObjectMgr->GetSpellScriptsBounds(s.spell);
                for (auto it = bounds.first; it != bounds.second; ++it)
                {
                    std::string const& script = sObjectMgr->GetScriptName(it->second);
                    if (script == "spell_ef_kit_shatter" && info->Effects[EFFECT_2].TriggerSpell)
                        AddShellSpell(info->Effects[EFFECT_2].TriggerSpell);
                    // a stack-cooldown passive (CooldownByStacks)
                    if (script == "spell_ef_kit_stack_cooldown")
                        s_stackCooldownSpells.insert(s.spell);
                }
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
        LoadSpecPowers();
        LoadClassPowers();
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

    int32 ShellPerStack(Unit* caster, uint32 shellSpell)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(shellSpell);
        if (!caster || !info)
            return 0;
        // effect 2's percent with its spell mods (CalcValue applies them: Deep Shell, Rimeholder 8)
        int32 const pct = info->Effects[EFFECT_1].CalcValue(caster);
        int32 per = int32(caster->CountPctFromMaxHealth(pct > 0 ? pct : 10));
        // effect 1's spell mods scale every stack (Rimeholder 2)
        if (Player* modOwner = caster->GetSpellModOwner())
            modOwner->ApplySpellMod(shellSpell, SPELLMOD_EFFECT1, per);
        return std::max(1, per);
    }

    namespace
    {
        int32 ShellMaxStacks(SpellInfo const* info) { return std::max<int32>(1, int32(info->StackAmount)); }

        // the pool `left` on the shell aura: its stacks follow (ceil), then the amount is put back (SetStackAmount
        // recalculates the amounts from the spell)
        void SetShell(Aura* aura, int32 perStack, int64 left)
        {
            int32 const stacks = Logic::ShellStacks(left, perStack, ShellMaxStacks(aura->GetSpellInfo()));
            if (stacks <= 0)
            {
                aura->Remove();
                return;
            }
            if (aura->GetStackAmount() != stacks)
                aura->SetStackAmount(uint8(stacks));
            if (AuraEffect* eff = aura->GetEffect(EFFECT_0))
                if (eff->GetAmount() != int32(left))
                    eff->ChangeAmount(int32(left));
        }
    }

    void AddShellStacks(Unit* caster, uint32 shellSpell, int32 stacks, int32 durationMs)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(shellSpell);
        if (!caster || !info || stacks <= 0)
            return;
        AddShellSpell(shellSpell);
        int32 const per = ShellPerStack(caster, shellSpell);
        Aura* aura = caster->GetAura(shellSpell, caster->GetGUID());
        int64 left = 0;
        if (!aura)
        {
            caster->CastSpell(caster, shellSpell, true);
            aura = caster->GetAura(shellSpell, caster->GetGUID());
            if (!aura)
                return;
            aura->SetMaxDuration(durationMs);
            aura->SetDuration(durationMs);
        }
        else
        {
            if (AuraEffect const* eff = aura->GetEffect(EFFECT_0))
                left = eff->GetAmount();
            if (aura->GetDuration() < durationMs)
            {
                aura->SetMaxDuration(std::max(aura->GetMaxDuration(), durationMs));
                aura->SetDuration(durationMs);
            }
        }
        SetShell(aura, per, Logic::ShellAfterGain(left, per, stacks, ShellMaxStacks(info)));
        RefreshConditionals(caster);
    }

    void FollowShell(Aura* shell, int32 absorbed)
    {
        if (!shell || shell->IsRemoved())
            return;
        Unit* caster = shell->GetCaster();
        AuraEffect* eff = shell->GetEffect(EFFECT_0);
        if (!caster || !eff || eff->GetAmount() <= 0)
            return;
        // called before the engine takes `absorbed` off the pool (Unit::CalcAbsorbResist): the stacks follow what will
        // be left, the amount stays for the engine to reduce; a pool worn to 0 the engine removes itself
        int32 const before = eff->GetAmount();
        int32 const stacks = Logic::ShellStacks(int64(before) - absorbed, ShellPerStack(caster, shell->GetId()),
                                                ShellMaxStacks(shell->GetSpellInfo()));
        if (stacks <= 0 || stacks == shell->GetStackAmount())
            return;
        shell->SetStackAmount(uint8(stacks));
        if (eff->GetAmount() != before)
            eff->ChangeAmount(before);
    }

    bool IsShellSpell(uint32 spellId)
    {
        std::lock_guard<std::mutex> lock(s_shellMutex);
        return s_shellSpells.count(spellId) != 0;
    }

    int32 ShellCap(Unit* caster, uint32 shellSpell)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(shellSpell);
        if (!caster || !info)
            return 0;
        return ShellPerStack(caster, shellSpell) * ShellMaxStacks(info);
    }

    bool TargetsCaster(uint32 spellId)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        return info && info->Effects[EFFECT_0].TargetA.GetTarget() == TARGET_UNIT_CASTER;
    }

    void HitBonuses(Unit* caster, Unit* target, uint32 spellId, uint32 stacks, bool selfTriggers)
    {
        if (!caster || !target || !spellId)
            return;
        struct Bonus
        {
            uint32 trigger;
            int32 chance;
        };
        std::vector<Bonus> bonuses;
        SpellInfo const* hitInfo = sSpellMgr->GetSpellInfo(spellId);
        for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
        {
            uint32 const trigger = eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell;
            if (!trigger || eff->GetMiscValueB() < 0 || stacks < uint32(eff->GetMiscValueB()))
                continue;
            if (IsRule(eff->GetBase()))   // a talent rule (Death's Momentum, Blood Frenzy): its trigger is the rule's
                continue;
            // the spell named by misc, or (misc 0) every spell of the effect's class mask (Hallowed Ground: the holy swords)
            bool const named = uint32(eff->GetMiscValue()) == spellId;
            // (a dummy without a class mask names no spell: the core's IsAffected takes an empty mask for the whole
            // family, and a growth stage's dummy, whose trigger is the next stage or the Bloom, would fire on every hit)
            bool const masked = eff->GetMiscValue() == 0 && hitInfo && eff->HasSpellClassMask() && eff->IsAffectedOnSpell(hitInfo);
            if (named || masked)
                bonuses.push_back({ trigger, eff->GetAmount() });
        }
        for (Bonus const& b : bonuses)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(b.trigger);
            if (!info)
                continue;
            bool const self = TargetsCaster(b.trigger);
            if (self && !selfTriggers)
                continue;
            // a trigger with a lockout of its own (Heaven's Answer Spent): not while the caster carries it
            if (info->ExcludeCasterAuraSpell && caster->HasAura(info->ExcludeCasterAuraSpell))
                continue;
            // a trigger that asks the target for an aura of the caster's (TargetAuraSpell: Oathblade's sword on a Condemned
            // enemy, Hallowed Ground's burst on an enemy standing in it)
            if (!self && info->TargetAuraSpell && !target->HasAura(info->TargetAuraSpell, caster->GetGUID()))
                continue;
            if (b.chance > 0 && b.chance < 100 && !roll_chance_i(b.chance))
                continue;
            if (self)
            {
                if (info->StackAmount > 1)
                    AddSelfStacks(caster, b.trigger, 1);
                else
                    caster->CastSpell(caster, b.trigger, true);
                continue;
            }
            if (!target->IsAlive())
                continue;
            if (info->StackAmount > 1)
                AddStacks(caster, target, b.trigger, 1);
            else
                caster->CastSpell(target, b.trigger, true);
        }
    }

    bool IsRule(Aura const* aura)
    {
        if (!aura)
            return false;
        for (AuraScript const* script : aura->m_loadedScripts)
            if (std::string const* name = script ? script->_GetScriptName() : nullptr; name && name->rfind("spell_ef_kit_rule_", 0) == 0)
                return true;
        return false;
    }

    std::vector<AuraEffect const*> Rules(Unit* caster, char const* script, uint32 spellId)
    {
        std::vector<AuraEffect const*> out;
        if (!caster || !script)
            return out;
        SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
        for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
        {
            if (!eff->GetBase()->GetScriptByName(script))
                continue;
            bool const masked = eff->HasSpellClassMask();
            bool named = false;
            if (spellId)
                named = uint32(eff->GetMiscValue()) == spellId
                        || (eff->GetMiscValue() == 0 && masked && info && eff->IsAffectedOnSpell(info));
            else
                named = eff->GetMiscValue() == 0 && !masked;
            if (named)
                out.push_back(eff);
        }
        return out;
    }

    uint32 AddSelfStacks(Unit* caster, uint32 stackSpell, int32 count)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(stackSpell);
        if (!caster || !info || count <= 0)
            return 0;
        int32 const cap = Logic::StackCap(info->Effects[EFFECT_0].CalcValue(caster), int32(info->StackAmount));
        Aura* aura = caster->GetAura(stackSpell, caster->GetGUID());
        int32 gain = count;
        if (!aura)
        {
            aura = caster->AddAura(stackSpell, caster);
            if (!aura)
                return 0;
            --gain;
        }
        int32 const have = aura->GetStackAmount();
        int32 const after = Logic::StacksAfter(have, gain, cap);
        if (after > have)
            aura->ModStackAmount(after - have);
        else
            aura->RefreshDuration();
        return aura->GetStackAmount();
    }

    int32 Lengthen(Unit* caster, uint32 auraSpell, int32 addMs, int32 maxExtraSeconds)
    {
        Aura* aura = caster ? caster->GetAura(auraSpell, caster->GetGUID()) : nullptr;
        if (!aura || aura->GetDuration() <= 0)
            return 0;
        int32 base = aura->GetSpellInfo()->GetMaxDuration();
        if (Player* modOwner = caster->GetSpellModOwner())
            modOwner->ApplySpellMod(auraSpell, SPELLMOD_DURATION, base);
        int32 const add = Logic::LengthenBy(addMs, maxExtraSeconds * IN_MILLISECONDS, aura->GetMaxDuration(), base);
        if (add <= 0)
            return 0;
        aura->SetMaxDuration(aura->GetMaxDuration() + add);
        aura->SetDuration(aura->GetDuration() + add);
        return add;
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

    void CooldownByStacks(Unit* caster, uint32 stackSpell, uint32 consumed)
    {
        Player* player = caster ? caster->ToPlayer() : nullptr;
        if (!player || !consumed || s_stackCooldownSpells.empty())
            return;
        struct Want
        {
            uint32 spell;
            int32 msPerStack;
            int32 maxMs;
        };
        std::vector<Want> wants;
        for (uint32 passive : s_stackCooldownSpells)
        {
            Aura const* aura = player->GetAura(passive, player->GetGUID());
            AuraEffect const* eff = aura ? aura->GetEffect(EFFECT_0) : nullptr;
            if (!eff || uint32(eff->GetMiscValue()) != stackSpell)
                continue;
            uint32 const spell = aura->GetSpellInfo()->Effects[EFFECT_0].TriggerSpell;
            AuraEffect const* most = aura->GetEffect(EFFECT_1);
            if (spell)
                wants.push_back({ spell, eff->GetAmount(), most ? most->GetAmount() * IN_MILLISECONDS : 0 });
        }
        if (wants.empty())
            return;
        StackCooldownCuts* cuts = player->CustomData.GetDefault<StackCooldownCuts>("emberfall_stack_cooldowns");
        for (Want const& w : wants)
        {
            auto cd = player->GetSpellCooldownMap().find(w.spell);
            if (cd == player->GetSpellCooldownMap().end())
                continue;
            int32 const remaining = int32(player->GetSpellCooldownDelay(w.spell));
            StackCooldownCuts::Cut& cut = cuts->bySpell[w.spell];
            // another end than the one this left: a new cooldown, nothing taken off it yet
            if (cut.end != cd->second.end)
                cut = {};
            int32 const take = Logic::StackCooldownCut(consumed, w.msPerStack, cut.takenMs, w.maxMs, remaining);
            if (take <= 0)
                continue;
            player->ModifySpellCooldown(w.spell, -take);
            cut.takenMs += take;
            cut.end = cd->second.end;
        }
    }

    int32 CutCooldownPct(Unit* caster, uint32 spell, int32 pct, int32 floorMs)
    {
        Player* player = caster ? caster->ToPlayer() : nullptr;
        if (!player || !spell || pct <= 0 || !player->HasSpellCooldown(spell))
            return 0;
        auto cd = player->GetSpellCooldownMap().find(spell);
        if (cd == player->GetSpellCooldownMap().end())
            return 0;
        // a player's cuts of each cooldown, so the floor counts every cut of the one cooldown (CooldownByStacks' record)
        StackCooldownCuts* cuts = player->CustomData.GetDefault<StackCooldownCuts>("emberfall_pct_cooldowns");
        StackCooldownCuts::Cut& cut = cuts->bySpell[spell];
        if (cut.end != cd->second.end)
            cut = {};
        // 20 % of the BASE cooldown (Spell.dbc's, ankarna's Q4 of 2026-10-23), not of what is left
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
        int32 const baseMs = info ? int32(std::max(info->RecoveryTime, info->CategoryRecoveryTime)) : 0;
        int32 const take = Logic::CutBasePct(int32(player->GetSpellCooldownDelay(spell)), baseMs, pct,
                                             int32(cd->second.maxduration), floorMs, cut.takenMs);
        if (take <= 0)
            return 0;
        player->ModifySpellCooldown(spell, -take);
        cut.takenMs += take;
        cut.end = cd->second.end;
        return take;
    }

    int32 ShortenCooldown(Unit* caster, uint32 spell, int32 cutMs)
    {
        Player* player = caster ? caster->ToPlayer() : nullptr;
        if (!player || !spell || !player->HasSpellCooldown(spell))
            return 0;
        int32 const take = Logic::CooldownCut(cutMs, int32(player->GetSpellCooldownDelay(spell)));
        if (take > 0)
            player->ModifySpellCooldown(spell, -take);
        return take;
    }

    int32 EmpowerPct(Unit* caster, uint32 spell)
    {
        if (!caster || !spell)
            return 0;
        int32 pct = 0;
        for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
            if (uint32(eff->GetMiscValue()) == spell && !eff->GetBase()->IsPassive()
                && !eff->GetSpellInfo()->Effects[eff->GetEffIndex()].TriggerSpell)
                pct += eff->GetAmount();
        return pct;
    }

    void RefreshEmpowered(Unit* caster, uint32 spell)
    {
        if (!caster || !spell)
            return;
        auto refresh = [caster, spell](Unit* unit)
        {
            if (!unit || !unit->IsInWorld())
                return;
            if (Aura* aura = unit->GetAura(spell, caster->GetGUID()))
                for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                    if (AuraEffect* eff = aura->GetEffect(i))
                        eff->RecalculateAmount(caster);
        };
        refresh(caster);
        if (Player* player = caster->ToPlayer())
            if (Group* group = player->GetGroup())
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                    if (Player* member = ref->GetSource(); member && member != player && member->GetMap() == caster->GetMap())
                        refresh(member);
    }

    uint32 LaunchStacks(Unit* caster, Unit* target, SpellInfo const* launcher)
    {
        if (!caster || !launcher || !caster->IsAlive())
            return 0;
        struct Launch
        {
            uint32 aura;
            uint32 trigger;
            int32 perCast;
            float reach;
        };
        std::vector<Launch> launches;
        for (AuraEffect const* eff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
        {
            SpellInfo const* info = eff->GetSpellInfo();
            uint32 const trigger = info->Effects[eff->GetEffIndex()].TriggerSpell;
            if (trigger && uint32(eff->GetMiscValue()) == info->Id && info->StackAmount > 1 && eff->IsAffectedOnSpell(launcher))
                // the spell's own value: a stacking aura's amounts are multiplied by its stacks
                launches.push_back({ info->Id, trigger, info->Effects[eff->GetEffIndex()].CalcValue(caster),
                    float(std::max(1, eff->GetMiscValueB())) });
        }
        uint32 launched = 0;
        for (Launch const& l : launches)
        {
            Aura* aura = caster->GetAura(l.aura, caster->GetGUID());
            int32 const count = aura ? Logic::LaunchCount(l.perCast, aura->GetStackAmount()) : 0;
            if (count <= 0)
                continue;
            std::vector<Unit*> targets;
            if (l.perCast > 0)
            {
                if (target && target != caster && target->IsAlive() && caster->IsValidAttackTarget(target))
                    targets.push_back(target);
            }
            else
            {
                // all of them at the enemies near the caster, the nearest first, round in turn
                std::list<Unit*> near;
                Acore::AnyUnfriendlyUnitInObjectRangeCheck check(caster, caster, l.reach);
                Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(caster, near, check);
                Cell::VisitObjects(caster, searcher, l.reach);
                near.remove_if([caster](Unit* u) { return !u->IsAlive() || !caster->IsValidAttackTarget(u)
                    || (!u->IsInCombat() && !u->IsHostileTo(caster)); });
                near.sort([caster](Unit* a, Unit* b) { return caster->GetExactDist2dSq(a) < caster->GetExactDist2dSq(b); });
                targets.assign(near.begin(), near.end());
            }
            if (targets.empty())
                continue;
            for (int32 i = 0; i < count; ++i)
                caster->CastSpell(targets[size_t(i) % targets.size()], l.trigger, true);
            launched += uint32(count);
            if (Aura* still = caster->GetAura(l.aura, caster->GetGUID()))
                still->ModStackAmount(-count);
        }
        return launched;
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

    Unit* MostInjuredAlly(Unit* caster, float range)
    {
        if (!caster || !caster->IsInWorld())
            return nullptr;
        std::vector<Unit*> allies;
        auto consider = [caster, range, &allies](Unit* unit)
        {
            if (!unit || !unit->IsInWorld() || !unit->IsAlive() || unit->GetMap() != caster->GetMap()
                || std::find(allies.begin(), allies.end(), unit) != allies.end())
                return;
            if (unit != caster && (!caster->IsWithinDistInMap(unit, range) || !caster->IsFriendlyTo(unit)))
                return;
            allies.push_back(unit);
        };
        auto withControlled = [&consider](Unit* unit)
        {
            consider(unit);
            for (Unit* controlled : unit->m_Controlled)
                consider(controlled);
        };
        withControlled(caster);
        if (Player* player = caster->ToPlayer())
            if (Group* group = player->GetGroup())
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                    if (Player* member = ref->GetSource(); member && member != player)
                        withControlled(member);
        std::vector<uint32> health, max;
        for (Unit* unit : allies)
        {
            health.push_back(unit->GetHealth());
            max.push_back(unit->GetMaxHealth());
        }
        int32 const index = Logic::MostInjured(health.begin(), health.end(), max.begin());
        return index >= 0 ? allies[index] : nullptr;
    }

    std::vector<Unit*> PartyUnits(Unit* caster, WorldObject const* center, float range, bool pets)
    {
        std::vector<Unit*> out;
        if (!caster || !caster->IsInWorld())
            return out;
        if (!center)
            center = caster;
        auto consider = [&](Unit* unit)
        {
            if (!unit || !unit->IsInWorld() || !unit->IsAlive() || unit->GetMap() != caster->GetMap()
                || std::find(out.begin(), out.end(), unit) != out.end())
                return;
            if (unit != caster && !caster->IsFriendlyTo(unit))
                return;
            if (!center->IsWithinDistInMap(unit, range))
                return;
            out.push_back(unit);
        };
        // companions are guardians the player controls (EfCompanions summons them as ally guardians); a hunter's or
        // warlock's pet is a Pet: only with `pets`
        auto withControlled = [&](Unit* unit)
        {
            consider(unit);
            for (Unit* controlled : unit->m_Controlled)
                if (controlled && (pets || !controlled->IsPet()))
                    consider(controlled);
            // the .efparty companions are summoned guardians the core does not list as controlled: the roster's
            if (Player* player = unit->ToPlayer())
                for (Creature* companion : Emberfall::Companions::LiveCompanions(player))
                    consider(companion);
        };
        withControlled(caster);
        if (Player* player = caster->ToPlayer())
            if (Group* group = player->GetGroup())
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                    if (Player* member = ref->GetSource(); member && member != player)
                        if (!group->isRaidGroup() || group->SameSubGroup(player, member))
                            withControlled(member);
        return out;
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

    void OnLogin(Player* player)
    {
        PurgeAndLog(player, "login");
        ApplySpecPower(player, true);
    }

    void OnLevelChanged(Player* player)
    {
        PurgeAndLog(player, "level change");
        ApplySpecPower(player);
    }

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
