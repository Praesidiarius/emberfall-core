/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version. Emberfall's part of the module (Copyright Emberfall / Tales of Orrin).
 */

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
class Unit;
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
        uint8 selfBelowPct = 0;  // > 0: only with the bot's own health under this percent (a defensive)
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
        bool prespecRanged = false; // keeps its distance before the spec choice (levels 1-9: the hunter's shots)
        // gear: the stat weights for the module's item scores (strength, agility, intellect, spirit, stamina,
        // spell_power, attack_power); empty: the module's stock weights
        std::vector<std::pair<std::string, float>> statWeights;
        std::vector<RoutineStep> steps;
        std::vector<std::pair<uint32, uint32>> talents; // (node, rank) in buying order, class tree and spec tree
    };

    // Playerbots.Emberfall.Enable and the ef_bot_* tables loaded.
    bool Enabled();
    // The mark for testers: the bot joins its side's one bot guild (made by the first bot of the side that logs in;
    // names Playerbots.Emberfall.GuildAccord / GuildKindred, our own words). Empty names: no guild.
    void EnsureGuild(Player* bot);

    // The run-time off switch (`.playerbots rndbot off|on`): every random bot logged out, none logged in.
    bool Paused();
    void SetPaused(bool paused);
    // Reads the config and the tables (at the module's start); logs what it found.
    void Load();

    // The bot's spec: the one stored for it ("ef_spec" in playerbots_random_bots), else a damage spec of its class
    // chosen now (by weight) and stored. Null when its class has no damage spec in the tables.
    Spec const* SpecOf(Player* bot);
    // The spec choice comes at level 10: below it a bot fights with its class's pre-spec kit.
    constexpr uint8 PRESPEC_LEVELS_END = 10;
    // The bot keeps its distance now (the spec's range, or its pre-spec range below level 10).
    bool FightsRanged(Player* bot);
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

    // Rest between pulls (bot step 2): below Playerbots.Emberfall.RestHealthPct (60) health or RestManaPct (40) mana a
    // bot pulls nothing and sits, eating and drinking what it carries, until RestUntilPct (90) of both.
    // With RestMarginPct > 0 (bot deaths, docs §13) the threshold is at least what the bot's last fights cost it plus
    // that margin (capped at 99), and it rests at least up to that.
    bool NeedsRest(Player* bot);
    bool RestedEnough(Player* bot);
    // Pulling (bot deaths): a grind target with idle hostile creatures within PullCrowdYards (0: off) counts
    // PullCrowdPenaltyYards farther a creature, and more than PullCrowdMax of them rule it out.
    float PullCrowdYards();
    uint32 PullCrowdMax();
    float PullCrowdPenalty();
    // Idle hostile creatures (alive, not fighting, no critters) within `yards` of the target, the target left out.
    uint32 IdleCrowd(Player* bot, Unit* target, float yards);
    // Escape (bot deaths): below this health percent (0: off) in a fight the bot is losing, it runs.
    float EscapeBelowPct();
    // A defensive (a routine step with self_below) with two or more creatures attacking: that many points earlier (0: off).
    float DefensiveCrowdBonusPct();
    // A pull the bot leaves alone: within AvoidDeathSpotYards (40) of where it died less than AvoidDeathSpotMinutes ago
    // (0: off), as a player who died at a camp goes elsewhere for a while.
    bool AvoidAsPull(Player* bot, Unit* target);
    // How many levels above the bot a creature it attacks or a quest it takes may be (MaxTargetLevelAbove, 2).
    int32 MaxLevelAbove();
    // Our provisions: Emberfall's food and drink spells use the categories 2 and 3 (content/data/spells/consumables.yaml),
    // the stock 11 and 59.
    bool IsFood(ItemTemplate const* proto);
    bool IsDrink(ItemTemplate const* proto);
}

#endif
