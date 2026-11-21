/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version. Emberfall's part of the module (Copyright Emberfall / Tales of Orrin).
 */

/*
 * Emberfall's bots step out of telegraphs (Emberfall docs/design/playerbots-pilot.md §20, mob-abilities.md §13): when a
 * creature starts a cast one can step out of (the avoidable rule: a telegraphed spell with direct damage, a cast bar of
 * at least 1.5 s), every bot whose spot it will cover gets a reaction time drawn for it (0.4-0.9 s, sometimes late);
 * when that is up the bot walks the shortest checked way out of the area and holds there until the hit has landed, then
 * goes back to what it was doing. The areas are the companions' (mod-emberfall telegraphs/EfTelegraphs.h); the rules
 * are plain logic (telegraphs/EfTelegraphLogic.h, tests in mod-emberfall's tests/telegraph_tests.cpp).
 *
 * Triggered by the cast's start (the core's OnSpellPrepare hook), not by a scan: a bot looks only while such a cast is
 * running somewhere (one atomic load otherwise). A reaction (the module's reaction engine), so it is seen while the bot
 * casts or waits out a delay. Playerbots.Emberfall.AvoidTelegraphs (default 1; 0 off, `.reload config` is enough).
 * Released under GNU GPL v2 or later, as the module.
 */

#ifndef PLAYERBOTS_EMBERFALLTELEGRAPHS_H
#define PLAYERBOTS_EMBERFALLTELEGRAPHS_H

#include "MovementActions.h"
#include "Strategy.h"
#include "Trigger.h"

#include <map>

class Player;

namespace EmberfallTelegraphs
{
    // Playerbots.Emberfall.AvoidTelegraphs and Emberfall mode.
    bool On();
    // The cast hook, the hit count, the config switch, the realm's summary line.
    void AddScripts();

    // The counts since the last reset (the arena's pulls, the summary line): per bot and overall.
    struct Counts
    {
        uint32 casts = 0;      // avoidable casts with a bot inside at their start
        uint32 inside = 0;     // bots inside an area (at its start or later)
        uint32 stepped = 0;    // bots that walked out
        uint32 inTime = 0;     // of them, out before the hit by the plan (reaction and walk)
        uint32 noWay = 0;      // no checked spot out: the bot stays
        uint32 hits = 0;       // avoidable hits that landed on a bot
        uint64 hookUs = 0;     // time in the cast hook (its slow path)
        uint32 hookCalls = 0;
        uint64 checkUs = 0;    // time in the bots' looks while casts were running
        uint32 checkCalls = 0;
    };
    Counts Total();
    // Per spell: its avoidable casts, those with a bot inside at the start, its hits on bots; the shape (c cone, r ring
    // round the caster, g circle at a place, l line).
    struct SpellCount
    {
        uint32 casts = 0, overBots = 0, hits = 0;
        char shape = '?';
    };
    std::map<uint32, SpellCount> Spells();
    uint32 HitsOn(Player* bot);
    uint32 SteppedOut(Player* bot);
    void ResetCounts();
}

// The reaction strategy (in every Emberfall bot's reaction engine, AiFactory): one trigger, one action.
class EmberfallTelegraphStrategy : public Strategy
{
public:
    EmberfallTelegraphStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}
    std::string const getName() override { return "ef telegraph"; }
    uint32 GetType() const override { return STRATEGY_TYPE_REACTION; }

protected:
    void InitReactionTriggers(std::vector<TriggerNode*>& triggers) override;
};

class EmberfallTelegraphTrigger : public Trigger
{
public:
    EmberfallTelegraphTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ef telegraph", 1) {}
    bool IsActive() override;
};

class EmberfallTelegraphAction : public MovementAction
{
public:
    EmberfallTelegraphAction(PlayerbotAI* botAI) : MovementAction(botAI, "ef telegraph") {}
    bool isPossible() override;
    bool Execute(Event event) override;
};

#endif
