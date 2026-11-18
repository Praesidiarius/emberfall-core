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

// Emberfall (PATCHES.md, "Module"; the core's #40): the stat rules (Emberfall docs/design/classes.md §4.10, Nathanael
// 2026-11-15), for players, with Emberfall.OwnData = 1 and Emberfall.Stats.Rules (default 1):
//   - Intellect gives spell power one for one (the core's #40 hunk; this script switches it on);
//   - Strength gives attack power one for one, a hunter's Agility too (OnPlayerAfterUpdateAttackPowerAndDamage: the base
//     in place of upstream's class formula; src/stats/EfStatRulesLogic.h AttackPowerBase);
//   - the mana pool is its class and level's (ef_player_mana, rows from Emberfall's formulas.yaml player_stats.mana;
//     OnPlayerAfterUpdateMaxPower: the pool in place of create mana and Intellect's bonus, talents on top);
//   - mana comes back at the row's rate a second, in and out of a fight, while casting too (the core's #40 hunk reads
//     ManaRegenPerSecond below);
//   - Spirit is no stat: an item's Spirit is dropped (OnPlayerApplyItemModsBefore), health out of combat comes from
//     Vigour (the core's #40 hunk);
//   - a percent mana cost is a share of the player's maximum mana, not of base mana (the core's #40 hunk in
//     SpellInfo::CalcPowerCost, ManaCostBase; Nathanael 2026-11-18, feedback #646).
// Without rows in ef_player_mana (or without the table) the pool and the regen stay upstream's; the rest still holds.

#include "Config.h"
#include "DatabaseEnv.h"
#include "EmberfallOwnData.h"
#include "EmberfallStatRules.h"
#include "Log.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "WorldScript.h"
#include "stats/EfStatRulesLogic.h"

#include <unordered_map>

namespace
{
    using namespace Emberfall::StatRules;

    std::unordered_map<uint16, ManaRow> s_mana;

    ManaRow const* RowOf(Player const* player)
    {
        auto it = s_mana.find(RowKey(player->getClass(), player->GetLevel()));
        return it != s_mana.end() ? &it->second : nullptr;
    }

    // the core's #40 hunk in Player::UpdateManaRegen: the row's mana a second, or -1 (upstream's regen) without a row
    float RowRegen(Player const* player)
    {
        if (!player || player->GetCreateMana() == 0)
            return -1.0f;
        ManaRow const* row = RowOf(player);
        return row ? row->regen : -1.0f;
    }

    bool TableExists(char const* name)
    {
        QueryResult r = WorldDatabase.Query("SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() "
                                            "AND TABLE_NAME = '{}'", name);
        return r && r->Fetch()[0].Get<uint64>() == 1;
    }

    void LoadMana()
    {
        s_mana.clear();
        if (!TableExists("ef_player_mana"))
        {
            LOG_INFO("server.loading", ">> Emberfall stat rules: no ef_player_mana table (the mana pool and regen as upstream)");
            return;
        }
        uint32 bad = 0;
        if (QueryResult r = WorldDatabase.Query("SELECT `class`, `level`, `pool`, `regen` FROM `ef_player_mana`"))
        {
            do
            {
                Field* f = r->Fetch();
                ManaRow row;
                row.classId = f[0].Get<uint8>();
                row.level = f[1].Get<uint8>();
                row.pool = f[2].Get<float>();
                row.regen = f[3].Get<float>();
                if (!ValidRow(row))
                {
                    ++bad;
                    continue;
                }
                s_mana[RowKey(row.classId, row.level)] = row;
            } while (r->NextRow());
        }
        if (bad)
            LOG_ERROR("server.loading", ">> Emberfall stat rules: {} row(s) of ef_player_mana skipped (class, level and pool above 0, regen not below 0)", bad);
        LOG_INFO("server.loading", ">> Emberfall stat rules: {} class and level rows of the mana pool and its regen", s_mana.size());
    }

    class emberfall_stat_rules_world : public WorldScript
    {
    public:
        emberfall_stat_rules_world()
            : WorldScript("emberfall_stat_rules_world", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_LOAD_CUSTOM_DATABASE_TABLE }) { }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            Settings& settings = Current();
            settings.on = Emberfall::OwnData() && sConfigMgr->GetOption<bool>("Emberfall.Stats.Rules", true, false);
            settings.manaRegenPerSecond = settings.on ? &RowRegen : nullptr;
            if (Emberfall::OwnData())
                LOG_INFO("server.loading", ">> Emberfall stat rules: {}", settings.on
                    ? "on (Intellect = spell power, Strength = attack power, the mana pool by class and level, flat mana regen, no Spirit, "
                      "percent mana costs of the pool)"
                    : "off (upstream's stats)");
        }

        void OnLoadCustomDatabaseTable() override
        {
            if (On())
                LoadMana();
        }
    };

    class emberfall_stat_rules_player : public PlayerScript
    {
    public:
        emberfall_stat_rules_player()
            : PlayerScript("emberfall_stat_rules_player", { PLAYERHOOK_ON_AFTER_UPDATE_MAX_POWER,
                PLAYERHOOK_ON_AFTER_UPDATE_ATTACK_POWER_AND_DAMAGE, PLAYERHOOK_ON_APPLY_ITEM_MODS_BEFORE }) { }

        // Player::UpdateMaxPower: the class and level's pool in place of create mana and Intellect's bonus. Additive on
        // upstream's value, so the class kits' own hook on the same call (a class or spec power's maximum) adds in any
        // order; a power other than mana and a class without create mana are left alone.
        void OnPlayerAfterUpdateMaxPower(Player* player, Powers& power, float& value) override
        {
            if (!On() || power != POWER_MANA || player->GetCreateMana() == 0)
                return;
            ManaRow const* row = RowOf(player);
            if (!row)
                return;
            UnitMods const mod = UnitMods(static_cast<uint16>(UNIT_MOD_POWER_START) + POWER_MANA);
            value = PoolInPlaceOfIntellect(value, float(player->GetCreateMana()), row->pool, player->GetManaBonusFromIntellect(),
                                           player->GetPctModifierValue(mod, BASE_PCT), player->GetPctModifierValue(mod, TOTAL_PCT));
        }

        // Player::UpdateAttackPowerAndDamage, after upstream's class formula: the base from Strength (and a hunter's
        // Agility) one for one, upstream's BASE_PCT on it; the auras' flat and percent parts as they were. The unit
        // modifier's BASE_VALUE keeps upstream's number (setting it would call this function again); nothing else reads it.
        void OnPlayerAfterUpdateAttackPowerAndDamage(Player* player, float& /*level*/, float& base_attPower, float& /*attPowerMod*/,
                                                     float& /*attPowerMultiplier*/, bool ranged) override
        {
            if (!On())
                return;
            UnitMods const mod = ranged ? UNIT_MOD_ATTACK_POWER_RANGED : UNIT_MOD_ATTACK_POWER;
            base_attPower = AttackPowerBase(player->getClass(), player->GetStat(STAT_STRENGTH), player->GetStat(STAT_AGILITY))
                * player->GetPctModifierValue(mod, BASE_PCT);
        }

        // An item's Spirit gives nothing (rule 5; Emberfall's items carry none since 2026-11-15, this holds for old data).
        void OnPlayerApplyItemModsBefore(Player* /*player*/, uint8 /*slot*/, bool /*apply*/, uint8 /*itemProtoStatNumber*/,
                                         uint32 statType, int32& val) override
        {
            if (On() && DroppedItemStat(statType))
                val = 0;
        }
    };
}

void AddEmberfallStatRulesScripts()
{
    new emberfall_stat_rules_world();
    new emberfall_stat_rules_player();
}
