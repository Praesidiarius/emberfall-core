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
#include "EmberfallQuests.h"
#include "NamedObjectContext.h"
#include "Strategy.h"
#include "Trigger.h"

#include <unordered_map>

class EmberfallCombatStrategy : public Strategy
{
public:
    EmberfallCombatStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}
    std::string const getName() override { return "ef"; }
    std::vector<NextAction> getDefaultActions() override;
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

// The spec's next ability on the current target (0: none); the buffs and the companion when `prepare`.
uint32 EmberfallPickStep(PlayerbotAI* botAI, Unit* target, bool prepare, Unit** castOn,
                         EmberfallBackoff const* backoff = nullptr);

class EmberfallRotationAction : public Action
{
public:
    EmberfallRotationAction(PlayerbotAI* botAI) : Action(botAI, "ef rotation") {}
    std::string const GetTargetName() override { return "current target"; }
    bool isPossible() override;
    bool Execute(Event event) override;

private:
    EmberfallBackoff backoff;
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
        creators["ef prepare"] = &EmberfallActionContext::prepare;
        creators["ef quest"] = &EmberfallActionContext::quest;
        creators["ef rest"] = &EmberfallActionContext::rest;
        creators["ef group leave"] = &EmberfallActionContext::group_leave;
    }

private:
    static Action* rotation(PlayerbotAI* botAI) { return new EmberfallRotationAction(botAI); }
    static Action* prepare(PlayerbotAI* botAI) { return new EmberfallPrepareAction(botAI); }
    static Action* quest(PlayerbotAI* botAI) { return new EmberfallQuestAction(botAI); }
    static Action* rest(PlayerbotAI* botAI) { return new EmberfallRestAction(botAI); }
    static Action* group_leave(PlayerbotAI* botAI) { return new EmberfallGroupLeaveAction(botAI); }
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
    }

private:
    static Trigger* prepare(PlayerbotAI* botAI) { return new EmberfallPrepareTrigger(botAI); }
    static Trigger* quest(PlayerbotAI* botAI) { return new EmberfallQuestTrigger(botAI); }
    static Trigger* rest(PlayerbotAI* botAI) { return new EmberfallRestTrigger(botAI); }
    static Trigger* group(PlayerbotAI* botAI) { return new EmberfallGroupTrigger(botAI); }
};

#endif
