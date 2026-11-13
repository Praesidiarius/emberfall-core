/*
 * Emberfall's part of mod-playerbots (the playerbots pilot): see EmberfallStrategy.h. Released under GNU GPL v2 or
 * later, as the module.
 */

#include "EmberfallStrategy.h"

#include "EmberfallBots.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

std::vector<NextAction> EmberfallCombatStrategy::getDefaultActions()
{
    // The routine first; a melee spec swings when nothing in its table can go (a ranged spec waits for its power).
    std::vector<NextAction> actions = {NextAction("ef rotation", ACTION_DEFAULT + 0.5f)};
    EmberfallBots::Spec const* spec = EmberfallBots::SpecOf(botAI->GetBot());
    if (!spec || !spec->ranged)
        actions.push_back(NextAction("melee", ACTION_DEFAULT));
    return actions;
}

void EmberfallNonCombatStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("ef prepare", {NextAction("ef prepare", ACTION_HIGH)}));
}

namespace
{
    bool HasPowerFor(Player* bot, SpellInfo const* info)
    {
        int32 const cost = info->CalcPowerCost(bot, info->GetSchoolMask());
        if (cost <= 0)
            return true;
        if (info->PowerType >= MAX_POWERS)
            return true;
        return int32(bot->GetPower(Powers(info->PowerType))) >= cost;
    }

    // In reach for a targeted ability: melee abilities in melee range, the others within their maximum range.
    bool InReach(Player* bot, Unit* target, SpellInfo const* info)
    {
        float const maxRange = info->GetMaxRange(false, bot);
        if (maxRange <= 0.0f)
            return true;
        if (maxRange <= NOMINAL_MELEE_RANGE + 0.5f)
            return bot->IsWithinMeleeRange(target);
        return bot->IsWithinCombatRange(target, maxRange) && bot->IsWithinLOSInMap(target);
    }
}

uint32 EmberfallPickStep(PlayerbotAI* botAI, Unit* target, bool prepare, Unit** castOn)
{
    Player* bot = botAI->GetBot();
    EmberfallBots::Spec const* spec = EmberfallBots::SpecOf(bot);
    if (!spec)
        return 0;
    bool const fighting = target && target->IsAlive();
    bool const pull = fighting && !target->IsInCombat();
    for (EmberfallBots::RoutineStep const& step : spec->steps)
    {
        if (!bot->HasSpell(step.spell))
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(step.spell);
        if (!info)
            continue;
        Unit* on = nullptr;
        switch (step.kind)
        {
            case EmberfallBots::StepKind::Companion:
                if (!prepare || bot->IsInCombat() || bot->GetPet() || bot->GetGuardianPet())
                    continue;
                on = bot;
                break;
            case EmberfallBots::StepKind::Buff:
                if (step.buffAura && bot->HasAura(step.buffAura))
                    continue;
                on = bot;
                break;
            case EmberfallBots::StepKind::Opener:
                if (prepare || !pull)
                    continue;
                on = target;
                break;
            case EmberfallBots::StepKind::Rotation:
                if (prepare || !fighting)
                    continue;
                on = target;
                break;
        }
        if (on == target)
        {
            if (step.belowPct && target->GetHealthPct() >= float(step.belowPct))
                continue;
            if (step.debuffAura && target->HasAura(step.debuffAura, bot->GetGUID()))
                continue;
            if (step.nearYards > 0.0f)
            {
                if (bot->GetDistance(target) > step.nearYards)
                    continue;
                // centred on the bot: cast on itself
                if (info->GetMaxRange(false, bot) <= 0.0f)
                    on = bot;
            }
            else if (!InReach(bot, target, info))
                continue;
        }
        if (!HasPowerFor(bot, info))
            continue;
        if (!botAI->CanCastSpell(step.spell, on))
            continue;
        if (castOn)
            *castOn = on;
        return step.spell;
    }
    return 0;
}

bool EmberfallRotationAction::isPossible() { return EmberfallPickStep(botAI, GetTarget(), false, nullptr) != 0; }

bool EmberfallRotationAction::Execute(Event /*event*/)
{
    Unit* on = nullptr;
    uint32 const spell = EmberfallPickStep(botAI, GetTarget(), false, &on);
    if (!spell || !on)
        return false;
    bool const ok = botAI->CastSpell(spell, on);
    // the pilot's trace (Playerbots.log at debug): what each bot presses
    LOG_DEBUG("playerbots", "Emberfall rotation: {} (level {}) casts {} on {}: {}", bot->GetName(), bot->GetLevel(), spell,
              on->GetName(), ok ? "ok" : "failed");
    return ok;
}

bool EmberfallPrepareAction::isPossible() { return EmberfallPickStep(botAI, nullptr, true, nullptr) != 0; }

bool EmberfallPrepareAction::Execute(Event /*event*/)
{
    Unit* on = nullptr;
    uint32 const spell = EmberfallPickStep(botAI, nullptr, true, &on);
    if (!spell || !on)
        return false;
    bool const ok = botAI->CastSpell(spell, on);
    LOG_DEBUG("playerbots", "Emberfall prepare: {} casts {}: {}", bot->GetName(), spell, ok ? "ok" : "failed");
    return ok;
}

bool EmberfallPrepareTrigger::IsActive()
{
    return !bot->IsInCombat() && EmberfallPickStep(botAI, nullptr, true, nullptr) != 0;
}
