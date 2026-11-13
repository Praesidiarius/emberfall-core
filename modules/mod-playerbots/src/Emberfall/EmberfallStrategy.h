/*
 * Emberfall's part of mod-playerbots (the playerbots pilot): the one fight routine for every damage spec, read from the
 * ef_bot_routine table (EmberfallBots.h). "ef": in a fight, the first step of the spec's table whose conditions hold
 * and that the bot can cast; "ef nc": out of a fight, the companion and the self buffs. Released under GNU GPL v2 or
 * later, as the module.
 */

#ifndef PLAYERBOTS_EMBERFALLSTRATEGY_H
#define PLAYERBOTS_EMBERFALLSTRATEGY_H

#include "Action.h"
#include "NamedObjectContext.h"
#include "Strategy.h"
#include "Trigger.h"

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

// The spec's next ability on the current target (0: none); the buffs and the companion when `prepare`.
uint32 EmberfallPickStep(PlayerbotAI* botAI, Unit* target, bool prepare, Unit** castOn);

class EmberfallRotationAction : public Action
{
public:
    EmberfallRotationAction(PlayerbotAI* botAI) : Action(botAI, "ef rotation") {}
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
    }

private:
    static Action* rotation(PlayerbotAI* botAI) { return new EmberfallRotationAction(botAI); }
    static Action* prepare(PlayerbotAI* botAI) { return new EmberfallPrepareAction(botAI); }
};

class EmberfallTriggerContext : public NamedObjectContext<Trigger>
{
public:
    EmberfallTriggerContext() { creators["ef prepare"] = &EmberfallTriggerContext::prepare; }

private:
    static Trigger* prepare(PlayerbotAI* botAI) { return new EmberfallPrepareTrigger(botAI); }
};

#endif
