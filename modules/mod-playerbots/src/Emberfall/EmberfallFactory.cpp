/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version. Emberfall's part of the module (Copyright Emberfall / Tales of Orrin).
 */

/*
 * Emberfall's part of mod-playerbots (the playerbots pilot): PlayerbotFactory::Randomize on Emberfall's own data.
 * Released under GNU GPL v2 or later, as the module.
 *
 * What the stock Randomize does and what stays here:
 *   level, stats, quests reset, cooldowns, instance binds     kept
 *   stock class spells by id (InitClassSpells)                 off: Emberfall's class kits teach other spells
 *   the class trainers' spells (InitAvailableSpells)           kept: our trainers, our data
 *   ClearSpells (a full randomize)                             off: the spells of creation stay
 *   stock talents and premade spec links, glyphs               off: the spec and its talents go through mod-emberfall
 *   gear from item_template by level and stat weights          kept (our items; LimitGearExpansion must be 0, the
 *                                                              spec's weapons from ef_bot_spec)
 *   bags (stock item 51809), ammunition, reagents, potions,    off: stock item ids
 *   consumables, enchants, gems, keyring, mounts
 *   food from item_template by level                           kept (our food)
 *   reputation, guild, arena team, pet, profession skills      off for the pilot
 *   money                                                      a little, by level (the stock gives 10-50 gold a level)
 */

#include "EmberfallBots.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotFactory.h"
#include "PlayerbotRepository.h"
#include "Playerbots.h"
#include "Random.h"

void PlayerbotFactory::RandomizeEmberfall(bool incremental)
{
    uint32 const oldLevel = bot->GetLevel();
    Prepare();
    if (!incremental)
    {
        ResetQuests();
        if (level < oldLevel)
            ClearAllItems();
    }
    ClearInventory();
    bot->RemoveAllSpellCooldown();
    UnbindInstance();

    bot->GiveLevel(level);
    bot->InitStatsForLevel(true);
    CancelAuras();

    // the spells of creation stay (no ClearSpells); the skills' own spells and the trainers' up to the level
    bot->LearnDefaultSkills();
    InitAvailableSpells();

    // the damage spec, fixed for the bot, and its talents by the talent tree v2 rules (mod-emberfall)
    EmberfallBots::ApplyTalents(bot);
    if (botAI)
    {
        PlayerbotRepository::instance().Reset(botAI);
        botAI->ResetStrategies(false);
    }

    InitEquipment(incremental, false);
    InitFood();

    bot->SetMoney(urand(level * 200, level * 1000));
    bot->SetFullHealth();
    bot->SetPower(POWER_MANA, bot->GetMaxPower(POWER_MANA));
    bot->SaveToDB(false, false);
}
