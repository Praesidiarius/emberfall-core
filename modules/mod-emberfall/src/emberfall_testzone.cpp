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

// `.eftest zone <zone> [level]` (Emberfall docs/design/class-kit-pipeline.md §8.2): the player's own character as one
// arriving at a zone has it, so a levelling zone can be tested without playing everything before it. The route is
// data (Emberfall's generated ef_test_zone* and ef_test_gear tables; src/testzone/EfTestZoneLogic.h), read from the
// world database at every use (a few hundred rows), so a new zone's tables need no restart. In order:
//
//   1. the level: the zone's entry level or the one given (`.eftest level`'s change: XP 0, the level's talent points);
//   2. the quests of the zones before it on the character's route (its race's start zone, then the shared zones by
//      level band): each one the character's class and race may take, one of an exclusive group, is rewarded the way
//      the core's RewardQuest marks it (rewarded, reputation, a reward spell that teaches) but without XP, money or
//      items, and leaves the quest log with its quest items; a breadcrumb into the zone is put in the log instead
//      (with its start item), the quests after it in its chain are left alone; the zone's own quests are untouched;
//   3. gear: per slot the item pools' green for the class at the highest required level at or below the level
//      replaces what is worn there (destroyed; bags are not touched); a two-hand weapon takes the off hand's item too;
//   4. the flight nodes of the zones before it and its own; the purse topped up to level squared silver;
//   5. saved, then taken to the zone's main hub (a start zone: the race's start).
//
// No spec is chosen and no talent spent: the character chooses at 10 in the talent window as any player would.

#include "testzone/EfTestZone.h"
#include "testzone/EfTestZoneLogic.h"

#include "Chat.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QueryResult.h"
#include "QuestDef.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "World.h"

#include <set>
#include <string>
#include <vector>

namespace Emberfall::TestZone
{
    namespace
    {
        struct Data
        {
            std::vector<ZoneRow> zones;
            std::vector<QuestRow> quests;
            std::vector<TaxiRow> taxi;
            std::vector<GearRow> gear;
        };

        Data Load()
        {
            Data d;
            if (QueryResult r = WorldDatabase.Query("SELECT `zone`, `name`, `sort`, `area`, `level_min`, `level_max`, `races`, "
                                                    "`map`, `x`, `y`, `z`, `o`, `hub` FROM `ef_test_zone` ORDER BY `sort`"))
            {
                do
                {
                    Field* f = r->Fetch();
                    ZoneRow z;
                    z.key = f[0].Get<std::string>();
                    z.name = f[1].Get<std::string>();
                    z.sort = f[2].Get<uint32>();
                    z.area = f[3].Get<uint32>();
                    z.levelMin = f[4].Get<uint8>();
                    z.levelMax = f[5].Get<uint8>();
                    z.races = f[6].Get<uint32>();
                    z.map = f[7].Get<uint16>();
                    z.x = f[8].Get<float>();
                    z.y = f[9].Get<float>();
                    z.z = f[10].Get<float>();
                    z.o = f[11].Get<float>();
                    z.hub = f[12].Get<std::string>();
                    d.zones.push_back(std::move(z));
                } while (r->NextRow());
            }
            if (QueryResult r = WorldDatabase.Query("SELECT `quest`, `zone`, `sort`, `leads_to`, `role` FROM `ef_test_zone_quest`"))
            {
                do
                {
                    Field* f = r->Fetch();
                    d.quests.push_back({f[0].Get<uint32>(), f[1].Get<std::string>(), f[2].Get<uint32>(),
                                        f[3].Get<std::string>(), f[4].Get<uint8>()});
                } while (r->NextRow());
            }
            if (QueryResult r = WorldDatabase.Query("SELECT `zone`, `node` FROM `ef_test_zone_taxi` ORDER BY `zone`, `node`"))
            {
                do
                {
                    Field* f = r->Fetch();
                    d.taxi.push_back({f[0].Get<std::string>(), f[1].Get<uint32>()});
                } while (r->NextRow());
            }
            if (QueryResult r = WorldDatabase.Query("SELECT `class`, `slot`, `item`, `required_level` FROM `ef_test_gear`"))
            {
                do
                {
                    Field* f = r->Fetch();
                    d.gear.push_back({f[0].Get<uint8>(), f[1].Get<uint8>(), f[2].Get<uint32>(), f[3].Get<uint8>()});
                } while (r->NextRow());
            }
            return d;
        }

        std::string RaceNames(uint32 mask, LocaleConstant locale)
        {
            std::string out;
            for (uint32 race = 1; race <= 32; ++race)
                if (mask & RaceBit(race))
                    if (ChrRacesEntry const* entry = sChrRacesStore.LookupEntry(race))
                        out += std::string(out.empty() ? "" : ", ") + entry->name[locale];
            return out;
        }

        // The quest's own items out of the bags, as the core's RewardQuest takes them.
        void TakeQuestItems(Player* player, Quest const* quest)
        {
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(quest->RequiredItemId[i]))
                    if (proto->Bonding == BIND_QUEST_ITEM)
                        player->DestroyItemCount(proto->ItemId, 9999, true);
            for (uint8 i = 0; i < QUEST_SOURCE_ITEM_IDS_COUNT; ++i)
                if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(quest->ItemDrop[i]))
                    if (proto->Bonding == BIND_QUEST_ITEM)
                        player->DestroyItemCount(proto->ItemId, 9999, true);
        }

        // Out of the quest log, with its start item and quest items.
        void Drop(Player* player, Quest const* quest)
        {
            uint32 const id = quest->GetQuestId();
            uint16 const slot = player->FindQuestSlot(id);
            if (slot < MAX_QUEST_LOG_SIZE)
            {
                player->SetQuestSlot(slot, 0);
                player->TakeQuestSourceItem(id, false);
            }
            TakeQuestItems(player, quest);
            player->RemoveActiveQuest(id, false);
        }

        // Rewarded as RewardQuest marks it, without XP, money or items.
        void MarkDone(Player* player, Quest const* quest)
        {
            Drop(player, quest);
            if (player->IsQuestRewarded(quest->GetQuestId()))
                return;
            player->RewardReputation(quest);
            player->SetRewardedQuest(quest->GetQuestId());
            for (int32 spell : { quest->GetRewSpellCast(), int32(quest->GetRewSpell()) })
                if (spell > 0)
                    if (SpellInfo const* info = sSpellMgr->GetSpellInfo(uint32(spell)); info && info->HasEffect(SPELL_EFFECT_LEARN_SPELL))
                    {
                        player->CastSpell(player, info->Id, true);
                        break;
                    }
        }

        // The class and race may take it, and no other quest of its exclusive group was chosen.
        bool Takes(Player* player, Quest const* quest, std::set<int32>& groups)
        {
            if (!player->SatisfyQuestClass(quest, false) || !player->SatisfyQuestRace(quest, false))
                return false;
            if (int32 const group = quest->GetExclusiveGroup(); group > 0)
                return groups.insert(group).second;
            return true;
        }

        struct GearResult
        {
            uint32 worn = 0;
            std::vector<uint32> refused;
        };

        GearResult Dress(Player* player, std::map<uint8_t, uint32_t> const& gear)
        {
            GearResult out;
            for (auto const& [slot, item] : gear)
            {
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
                uint16 dest = 0;
                if (!proto || player->CanEquipNewItem(slot, dest, item, true) != EQUIP_ERR_OK)
                {
                    out.refused.push_back(item);
                    continue;
                }
                if (slot == EQUIPMENT_SLOT_MAINHAND && proto->InventoryType == INVTYPE_2HWEAPON &&
                    player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND))
                    player->DestroyItem(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND, true);
                if (player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    player->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
                if (player->CanEquipNewItem(slot, dest, item, false) == EQUIP_ERR_OK && player->EquipNewItem(dest, item, true))
                    ++out.worn;
                else
                    out.refused.push_back(item);
            }
            return out;
        }
    }

    void MaxCombatSkills(Player* player)
    {
        uint16 const max = player->GetMaxSkillValueForLevel();
        for (uint32 i = 0; i < sSkillLineStore.GetNumRows(); ++i)
        {
            SkillLineEntry const* line = sSkillLineStore.LookupEntry(i);
            if (!line || (line->categoryId != SKILL_CATEGORY_WEAPON && line->id != SKILL_DEFENSE) || !player->HasSkill(line->id))
                continue;
            if (player->GetPureMaxSkillValue(line->id) <= 1)
                continue; // dual wield: had or not, no value
            player->SetSkill(uint16(line->id), player->GetSkillStep(uint16(line->id)), max, max);
        }
        player->UpdateDefenseBonusesMod();
    }

    void SetLevel(Player* player, uint8 level)
    {
        if (player->GetLevel() != level)
        {
            player->GiveLevel(level);
            player->InitTalentForLevel();
        }
        MaxCombatSkills(player);
        player->SetUInt32Value(PLAYER_XP, 0);
        player->SetFullHealth();
        player->SetPower(POWER_MANA, player->GetMaxPower(POWER_MANA));
    }

    bool List(ChatHandler* handler)
    {
        Data const d = Load();
        if (d.zones.empty())
        {
            handler->SendSysMessage("No zones: ef_test_zone is empty (Emberfall's world.sql writes it).");
            handler->SetSentErrorMessage(true);
            return false;
        }
        handler->SendSysMessage("Emberfall levelling zones, in route order (.eftest zone <zone> [level]):");
        for (ZoneRow const& z : d.zones)
        {
            std::string const start = z.races ? " (start: " + RaceNames(z.races, handler->GetSessionDbcLocale()) + ")" : "";
            handler->PSendSysMessage("  {} {}-{}  {}, at {}{}", z.key, z.levelMin, z.levelMax, z.name, z.hub, start);
        }
        return true;
    }

    bool Arrive(ChatHandler* handler, Player* player, std::string_view text, uint32 levelArg)
    {
        Data const d = Load();
        std::string several;
        ZoneRow const* zone = Find(d.zones, text, &several);
        if (!zone)
        {
            if (!several.empty())
                handler->PSendSysMessage("\"{}\" is more than one zone: {}.", text, several);
            else
                handler->PSendSysMessage("No zone \"{}\" (.eftest zone list).", text);
            handler->SetSentErrorMessage(true);
            return false;
        }
        uint8 const maxLevel = uint8(std::clamp<uint32>(sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL), 1, 255));
        uint8 const level = uint8(std::clamp<uint32>(levelArg ? levelArg : zone->levelMin, 1, maxLevel));

        // 1. the level, and every weapon and defence skill at the level's maximum (as `.maxskill`; professions and
        // riding untouched): a player arriving here would have trained them on the way
        SetLevel(player, level);
        player->UpdateSkillsToMaxSkillsForLevel();

        // 2. the quests of the zones before it
        std::vector<ZoneRow> const before = Before(d.zones, player->getRace(), zone->key);
        QuestPlan const plan = PlanQuests(before, d.quests, zone->key);
        std::set<int32> groups;
        uint32 done = 0, notForUs = 0, missing = 0;
        for (uint32 id : plan.done)
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(id);
            if (!quest)
            {
                ++missing;
                continue;
            }
            if (!Takes(player, quest, groups))
            {
                if (player->IsActiveQuest(id))
                    Drop(player, quest);
                ++notForUs;
                continue;
            }
            MarkDone(player, quest);
            ++done;
        }
        std::vector<std::string> crumbs;
        for (uint32 id : plan.breadcrumbs)
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(id);
            if (!quest || !Takes(player, quest, groups) || player->IsQuestRewarded(id))
                continue;
            if (!player->IsActiveQuest(id))
            {
                if (!player->CanTakeQuest(quest, false) || !player->CanAddQuest(quest, false))
                    continue;
                player->AddQuestAndCheckCompletion(quest, nullptr);
            }
            crumbs.push_back(quest->GetTitle());
        }
        player->UpdateForQuestWorldObjects();

        // 3. gear
        GearResult const gear = Dress(player, PickGear(d.gear, player->getClass(), level));

        // 4. flight nodes and money
        uint32 nodes = 0;
        for (uint32 node : Nodes(before, zone->key, d.taxi))
            if (sTaxiNodesStore.LookupEntry(node) && player->m_taxi.SetTaximaskNode(node))
                ++nodes;
        uint32 const purse = Purse(level);
        if (player->GetMoney() < purse)
            player->SetMoney(purse);

        // 5. saved, then to the hub
        player->SaveToDB(false, false);
        std::string zonesBefore;
        for (ZoneRow const& z : before)
            zonesBefore += (zonesBefore.empty() ? "" : ", ") + z.key;
        handler->PSendSysMessage("EFTEST zone {} level {}", zone->key, level);
        handler->PSendSysMessage("  before it: {}; {} quests done ({} not for your class or race, {} missing), {} left "
                                 "for after the breadcrumb", zonesBefore.empty() ? "nothing" : zonesBefore, done, notForUs,
                                 missing, plan.waiting.size());
        std::string crumbList;
        for (std::string const& c : crumbs)
            crumbList += (crumbList.empty() ? "" : "; ") + c;
        handler->PSendSysMessage("  in your log: {}", crumbList.empty() ? "no breadcrumb" : crumbList);
        handler->PSendSysMessage("  gear: {} items worn{}; flight nodes: {} learned; money: {} copper or more", gear.worn,
                                 gear.refused.empty() ? "" : " (" + std::to_string(gear.refused.size()) + " refused)",
                                 nodes, purse);
        handler->SendSysMessage("  spec and talents: none chosen (choose in the talent window from level 10)");
        handler->PSendSysMessage("  to {}", zone->hub);
        player->TeleportTo(zone->map, zone->x, zone->y, zone->z, zone->o);
        return true;
    }
}
