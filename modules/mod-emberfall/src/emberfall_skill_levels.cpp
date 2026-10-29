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

// Emberfall skills learned at a level (Emberfall docs/design/gear/item-pools.md §15; 2026-10-29, Nathanael: "the hunter
// should only be able to wear mail from level 40 on, like Classic"). There are no class trainers (2026-10-21), so a skill
// that Classic's trainer taught at a level comes by itself: a row of ef_skill_level (class, skill, level, spell; Emberfall's
// compiler writes it from player.yaml `skills_at_level`) gives the class that skill at that level and not before.
//
// At every login and level change, for each row of the player's class:
// - at the level or above, without the skill: Player::SetSkill (an armour skill is 1/1), which teaches the skill's
//   SkillLineAbility spells (the armour proficiency, `spell`), and the client hears the armour types again
//   (SMSG_SET_PROFICIENCY: the tooltip's type line turns white);
// - below the level, with the skill (a character made before the rule, or a GM's level down): the skill goes
//   (SetSkill 0 removes its SkillLineAbility spells), the client hears the armour types without it, and every equipped
//   item that needs the skill comes off into the bags, or by mail when they are full (Player::AutoUnequipOffhandIfNeed's
//   way). A login does that before the player sees anything: the core equipped the items while the skill was still known.
//
// The skill must have a SkillRaceClassInfo row for the race and class (skills.yaml), or the core drops it at the next
// login. Read at startup; a world without the table (an older one) has no rows.

#include "DatabaseEnv.h"
#include "Item.h"
#include "Language.h"
#include "Log.h"
#include "Mail.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldScript.h"
#include "WorldSession.h"

#include <vector>

namespace
{
    struct SkillAtLevel
    {
        uint8 classId = 0;
        uint16 skill = 0;
        uint8 level = 0;
        uint32 spell = 0;      // the proficiency spell the skill teaches (its item class and subclass mask)
    };

    std::vector<SkillAtLevel>& Rows()
    {
        static std::vector<SkillAtLevel> rows;
        return rows;
    }

    // The item class and subclass mask the proficiency spell allows; 0 when it is none.
    uint32 SubclassMask(SkillAtLevel const& row, ItemClass& itemClass)
    {
        SpellInfo const* info = row.spell ? sSpellMgr->GetSpellInfo(row.spell) : nullptr;
        if (!info || info->EquippedItemClass < 0 || info->EquippedItemSubClassMask <= 0)
            return 0;
        itemClass = ItemClass(info->EquippedItemClass);
        return uint32(info->EquippedItemSubClassMask);
    }

    // Takes every equipped item that needs `skill` off: into the bags, or by mail when they are full.
    uint32 UnequipNeeding(Player* player, uint16 skill)
    {
        uint32 moved = 0;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item || item->GetSkill() != skill)
                continue;
            ItemPosCountVec dest;
            if (player->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false) == EQUIP_ERR_OK)
            {
                player->RemoveItem(INVENTORY_SLOT_BAG_0, slot, true);
                player->StoreItem(dest, item, true);
            }
            else
            {
                player->MoveItemFromInventory(INVENTORY_SLOT_BAG_0, slot, true);
                CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
                item->DeleteFromInventoryDB(trans);
                item->SaveToDB(trans);
                std::string subject = player->GetSession()->GetAcoreString(LANG_NOT_EQUIPPED_ITEM);
                MailDraft(subject, "There were problems with equipping one or several items").AddItem(item)
                    .SendMailTo(trans, player, MailSender(player, MAIL_STATIONERY_GM), MAIL_CHECK_MASK_COPIED);
                CharacterDatabase.CommitTransaction(trans);
            }
            ++moved;
        }
        return moved;
    }

    void Apply(Player* player, char const* when)
    {
        for (SkillAtLevel const& row : Rows())
        {
            if (row.classId != player->getClass())
                continue;
            ItemClass itemClass = ITEM_CLASS_ARMOR;
            uint32 const mask = SubclassMask(row, itemClass);
            uint32 const known = itemClass == ITEM_CLASS_WEAPON ? player->GetWeaponProficiency() : player->GetArmorProficiency();
            if (player->GetLevel() >= row.level)
            {
                if (player->HasSkill(row.skill))
                    continue;
                player->SetSkill(row.skill, 0, 1, 1);
                if (row.spell && !player->HasSpell(row.spell))
                    player->learnSpell(row.spell);
                // the proficiency effect tells the client only a type it has not told before (SpellEffects.cpp
                // EffectProficiency): after a level down in the same session it would stay silent
                if (mask)
                    player->SendProficiency(itemClass, known | mask);
                LOG_INFO("module", "Emberfall skills at level: {} ({}) learned skill {} at level {} ({})", player->GetName(),
                         player->GetGUID().GetCounter(), row.skill, player->GetLevel(), when);
            }
            else
            {
                if (!player->HasSkill(row.skill))
                    continue;
                player->SetSkill(row.skill, 0, 0, 0);
                // the core keeps the type in its own mask until the next login (it never takes one away); the
                // client's tooltip shows it red at once
                if (mask)
                    player->SendProficiency(itemClass, known & ~mask);
                uint32 const moved = UnequipNeeding(player, row.skill);
                LOG_INFO("module", "Emberfall skills at level: {} ({}) lost skill {} below level {} ({}), {} item(s) taken off",
                         player->GetName(), player->GetGUID().GetCounter(), row.skill, row.level, when, moved);
            }
        }
    }

    class emberfall_skill_levels_world : public WorldScript
    {
    public:
        emberfall_skill_levels_world() : WorldScript("emberfall_skill_levels_world", { WORLDHOOK_ON_STARTUP }) { }

        void OnStartup() override
        {
            Rows().clear();
            QueryResult exists = WorldDatabase.Query("SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = "
                                                     "DATABASE() AND TABLE_NAME = 'ef_skill_level'");
            if (exists && exists->Fetch()[0].Get<uint64>() == 1)
            {
                if (QueryResult r = WorldDatabase.Query("SELECT `class`, `skill`, `level`, `spell` FROM `ef_skill_level`"))
                {
                    do
                    {
                        Field* f = r->Fetch();
                        Rows().push_back({ f[0].Get<uint8>(), f[1].Get<uint16>(), f[2].Get<uint8>(), f[3].Get<uint32>() });
                    } while (r->NextRow());
                }
            }
            LOG_INFO("module", "Emberfall skills at level: {} row(s)", Rows().size());
        }
    };

    class emberfall_skill_levels_player : public PlayerScript
    {
    public:
        emberfall_skill_levels_player() : PlayerScript("emberfall_skill_levels_player", {
            PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LEVEL_CHANGED }) { }

        void OnPlayerLogin(Player* player) override { Apply(player, "login"); }

        void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override { Apply(player, "level change"); }
    };
}

void AddEmberfallSkillLevelsScripts()
{
    new emberfall_skill_levels_world();
    new emberfall_skill_levels_player();
}
