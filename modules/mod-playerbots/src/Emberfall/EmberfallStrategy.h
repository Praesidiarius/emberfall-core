/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version. Emberfall's part of the module (Copyright Emberfall / Tales of Orrin).
 */

/*
 * Emberfall's part of mod-playerbots (the playerbots pilot): the one fight routine for every damage spec, read from the
 * ef_bot_routine table (EmberfallBots.h). "ef": in a fight, the first step of the spec's table whose conditions hold
 * and that the bot can cast; "ef nc": out of a fight, the companion and the self buffs. Released under GNU GPL v2 or
 * later, as the module.
 */

#ifndef PLAYERBOTS_EMBERFALLSTRATEGY_H
#define PLAYERBOTS_EMBERFALLSTRATEGY_H

#include "Action.h"
#include "MovementActions.h"
#include "EmberfallQuests.h"
#include "NamedObjectContext.h"
#include "ObjectGuid.h"
#include "Strategy.h"
#include "Trigger.h"

#include <unordered_map>

class EmberfallCombatStrategy : public Strategy
{
public:
    EmberfallCombatStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}
    std::string const getName() override { return "ef"; }
    std::vector<NextAction> getDefaultActions() override;
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

class EmberfallNonCombatStrategy : public Strategy
{
public:
    EmberfallNonCombatStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}
    std::string const getName() override { return "ef nc"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

// Spells a bot's cast of failed although the checks passed (Set On without a companion), skipped until the time (ms).
using EmberfallBackoff = std::unordered_map<uint32, uint32>;

// The target's time to live for the `ttl` condition (bot rotations, docs §18): its health when the bot first looked at
// it in this fight and since when; a new target starts the clock again.
class EmberfallTargetClock
{
public:
    float TimeToLive(Unit* target);  // seconds; < 0: unknown (EmberfallRotationLogic.h)
    // the `enemies` count near the target, kept a second (-1: none for this target)
    int32 CachedEnemies(Unit* target) const;
    void CacheEnemies(Unit* target, int32 count);

private:
    ObjectGuid guid;
    uint32 since = 0;   // ms
    uint32 health = 0;
    ObjectGuid enemiesGuid;
    uint32 enemiesAt = 0;  // ms
    int32 enemiesCount = -1;
};

// The spec's next ability on the current target (0: none); the buffs and the companion when `prepare`.
uint32 EmberfallPickStep(PlayerbotAI* botAI, Unit* target, bool prepare, Unit** castOn,
                         EmberfallBackoff const* backoff = nullptr, EmberfallTargetClock* clock = nullptr);

class EmberfallRotationAction : public Action
{
public:
    EmberfallRotationAction(PlayerbotAI* botAI) : Action(botAI, "ef rotation") {}
    std::string const GetTargetName() override { return "current target"; }
    bool isPossible() override;
    bool Execute(Event event) override;

private:
    EmberfallBackoff backoff;
    EmberfallTargetClock clock;
};

// A kiting step (ef_bot_spec.kite: the Marksman since ankarna's notes of 2026-11-19, when Loose, the hunter's Auto Shot,
// went for the whole class and its "ef auto shot" action with it): a ranged bot steps away from a foe that attacks it in
// its reach, one step every 6 s (EmberfallRotationLogic.h KiteStep), and shoots between (Patient Shot on the move).
class EmberfallKiteTrigger : public Trigger
{
public:
    EmberfallKiteTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ef kite", 1) {}
    bool IsActive() override;

private:
    uint32 lastStep = 0;  // ms
};

class EmberfallKiteAction : public MovementAction
{
public:
    EmberfallKiteAction(PlayerbotAI* botAI) : MovementAction(botAI, "ef kite") {}
    bool Execute(Event event) override;
};

// White swings of a ranged spec that fights in reach too (ef_bot_spec.swing: the Ranger in Bear's Claw), without moving.
class EmberfallSwingAction : public Action
{
public:
    EmberfallSwingAction(PlayerbotAI* botAI) : Action(botAI, "ef swing") {}
    std::string const GetTargetName() override { return "current target"; }
    bool isPossible() override;
    bool Execute(Event event) override;
};

class EmberfallPrepareAction : public Action
{
public:
    EmberfallPrepareAction(PlayerbotAI* botAI) : Action(botAI, "ef prepare") {}
    bool isPossible() override;
    bool Execute(Event event) override;
};

class EmberfallPrepareTrigger : public Trigger
{
public:
    EmberfallPrepareTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ef prepare", 5) {}
    bool IsActive() override;
};

// Escape (bot deaths, Playerbots.Emberfall.EscapeBelowPct, 0: off): below that health in a fight the record says the bot
// loses (EmberfallFights::Losing), it runs from its foes as a player does, and keeps running until the fight is over.
class EmberfallEscapeTrigger : public Trigger
{
public:
    EmberfallEscapeTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ef escape", 1) {}
    bool IsActive() override;
};

class EmberfallEscapeAction : public MovementAction
{
public:
    EmberfallEscapeAction(PlayerbotAI* botAI) : MovementAction(botAI, "ef escape") {}
    bool Execute(Event event) override;
};

// Rest between pulls (EmberfallBots::NeedsRest): sit, eat and drink our provisions, until rested enough.
class EmberfallRestTrigger : public Trigger
{
public:
    EmberfallRestTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ef rest", 1) {}
    bool IsActive() override;
};

class EmberfallRestAction : public Action
{
public:
    EmberfallRestAction(PlayerbotAI* botAI) : Action(botAI, "ef rest") {}
    bool Execute(Event event) override;

private:
    uint32 retryAt = 0;    // ms
    uint32 restockAt = 0;  // ms
};

// Group play (bot step 2): a bot a player invited leaves the group by itself when its leader has been away (offline,
// another map, over 300 yd) for Playerbots.Emberfall.GroupAwayMinutes (10), or after GroupMaxMinutes (180).
class EmberfallGroupTrigger : public Trigger
{
public:
    EmberfallGroupTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ef group", 10) {}
    bool IsActive() override;

private:
    uint32 joinedAt = 0;   // s
    uint32 awaySince = 0;  // s, 0: the leader is near
};

class EmberfallGroupLeaveAction : public Action
{
public:
    EmberfallGroupLeaveAction(PlayerbotAI* botAI) : Action(botAI, "ef group leave") {}
    bool Execute(Event event) override;
};

class EmberfallStrategyContext : public NamedObjectContext<Strategy>
{
public:
    EmberfallStrategyContext() : NamedObjectContext<Strategy>(false, true)
    {
        creators["ef"] = &EmberfallStrategyContext::combat;
        creators["ef nc"] = &EmberfallStrategyContext::noncombat;
    }

private:
    static Strategy* combat(PlayerbotAI* botAI) { return new EmberfallCombatStrategy(botAI); }
    static Strategy* noncombat(PlayerbotAI* botAI) { return new EmberfallNonCombatStrategy(botAI); }
};

class EmberfallActionContext : public NamedObjectContext<Action>
{
public:
    EmberfallActionContext()
    {
        creators["ef rotation"] = &EmberfallActionContext::rotation;
        creators["ef kite"] = &EmberfallActionContext::kite;
        creators["ef prepare"] = &EmberfallActionContext::prepare;
        creators["ef quest"] = &EmberfallActionContext::quest;
        creators["ef rest"] = &EmberfallActionContext::rest;
        creators["ef group leave"] = &EmberfallActionContext::group_leave;
        creators["ef escape"] = &EmberfallActionContext::escape;
        creators["ef swing"] = &EmberfallActionContext::swing;
    }

private:
    static Action* rotation(PlayerbotAI* botAI) { return new EmberfallRotationAction(botAI); }
    static Action* kite(PlayerbotAI* botAI) { return new EmberfallKiteAction(botAI); }
    static Action* prepare(PlayerbotAI* botAI) { return new EmberfallPrepareAction(botAI); }
    static Action* quest(PlayerbotAI* botAI) { return new EmberfallQuestAction(botAI); }
    static Action* rest(PlayerbotAI* botAI) { return new EmberfallRestAction(botAI); }
    static Action* group_leave(PlayerbotAI* botAI) { return new EmberfallGroupLeaveAction(botAI); }
    static Action* escape(PlayerbotAI* botAI) { return new EmberfallEscapeAction(botAI); }
    static Action* swing(PlayerbotAI* botAI) { return new EmberfallSwingAction(botAI); }
};

class EmberfallTriggerContext : public NamedObjectContext<Trigger>
{
public:
    EmberfallTriggerContext()
    {
        creators["ef prepare"] = &EmberfallTriggerContext::prepare;
        creators["ef quest"] = &EmberfallTriggerContext::quest;
        creators["ef rest"] = &EmberfallTriggerContext::rest;
        creators["ef group"] = &EmberfallTriggerContext::group;
        creators["ef escape"] = &EmberfallTriggerContext::escape;
        creators["ef kite"] = &EmberfallTriggerContext::kite;
    }

private:
    static Trigger* prepare(PlayerbotAI* botAI) { return new EmberfallPrepareTrigger(botAI); }
    static Trigger* quest(PlayerbotAI* botAI) { return new EmberfallQuestTrigger(botAI); }
    static Trigger* rest(PlayerbotAI* botAI) { return new EmberfallRestTrigger(botAI); }
    static Trigger* group(PlayerbotAI* botAI) { return new EmberfallGroupTrigger(botAI); }
    static Trigger* escape(PlayerbotAI* botAI) { return new EmberfallEscapeTrigger(botAI); }
    static Trigger* kite(PlayerbotAI* botAI) { return new EmberfallKiteTrigger(botAI); }
};

#endif
