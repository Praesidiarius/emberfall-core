/*
 * Emberfall's part of mod-playerbots (the playerbots pilot, Emberfall docs/design/playerbots-pilot.md): random bots on
 * Emberfall's own data. A bot gets a class and one of that class's damage specs (never a tank or healer spec), names
 * of its tribe, the spec's talents by the talent tree v2 rules through mod-emberfall's talent system, and a fight
 * routine read from the playerbots database (ef_bot_* tables, generated from the game repo's content/data/bots/),
 * so tuning a routine is content, not core code. Released under GNU GPL v2 or later, as the module.
 */

#ifndef PLAYERBOTS_EMBERFALLBOTS_H
#define PLAYERBOTS_EMBERFALLBOTS_H

#include "Define.h"

#include <set>
#include <string>
#include <utility>
#include <vector>

class Player;
struct ItemTemplate;

namespace EmberfallBots
{
    enum class StepKind : uint8
    {
        Rotation = 0,   // pressed in a fight when its conditions hold, in table order
        Opener = 1,     // only on a target that is not fighting yet (the pull)
        Buff = 2,       // a self buff kept up, in and out of a fight
        Companion = 3   // calls the companion while the bot has none (out of a fight)
    };

    struct RoutineStep
    {
        uint32 spell = 0;
        StepKind kind = StepKind::Rotation;
        float nearYards = 0.0f;  // > 0: only with the target this close (abilities centred on the bot)
        uint8 belowPct = 0;      // > 0: only on a target under this health percent
        uint32 debuffAura = 0;   // > 0: only while the target lacks this aura from the bot (damage over time)
        uint32 buffAura = 0;     // > 0: only while the bot lacks this aura (a self buff)
    };

    struct Spec
    {
        uint32 id = 0;
        uint8 classId = 0;
        std::string key;
        bool ranged = false;
        uint32 weight = 1;
        std::set<uint32> weapons;   // ItemSubclassWeapon values the spec fights with (main hand); empty: any
        std::set<uint32> offhand;   // weapon subclasses in the off hand (dual wield); empty: no off-hand weapon
        bool shield = false;        // may carry a shield in the off hand
        std::vector<RoutineStep> steps;
        std::vector<std::pair<uint32, uint32>> talents; // (node, rank) in buying order, class tree and spec tree
    };

    // Playerbots.Emberfall.Enable and the ef_bot_* tables loaded.
    bool Enabled();
    // Reads the config and the tables (at the module's start); logs what it found.
    void Load();

    // The bot's spec: the one stored for it ("ef_spec" in playerbots_random_bots), else a damage spec of its class
    // chosen now (by weight) and stored. Null when its class has no damage spec in the tables.
    Spec const* SpecOf(Player* bot);
    // The damage specs of a class (for the creation of bots: classes without any are skipped).
    bool HasSpecs(uint8 classId);

    // The spec's talents as far as the level allows (mod-emberfall's BotBuild); after a randomize and each level up.
    void ApplyTalents(Player* bot);

    // A free name of the race and gender from ef_bot_name, or empty.
    std::string TakeName(uint8 race, uint8 gender);

    // Gear rules of the spec (PlayerbotFactory::CanEquipWeapon / CanEquipArmor in Emberfall mode): 1 yes, 0 no, -1 no
    // opinion (the stock rule decides).
    int8 CanEquipWeapon(Player* bot, ItemTemplate const* proto);
    int8 CanEquipShield(Player* bot);
}

#endif
