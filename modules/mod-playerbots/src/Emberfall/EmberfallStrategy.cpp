/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version. Emberfall's part of the module (Copyright Emberfall / Tales of Orrin).
 */

/*
 * Emberfall's part of mod-playerbots (the playerbots pilot): see EmberfallStrategy.h. Released under GNU GPL v2 or
 * later, as the module.
 */

#include "EmberfallStrategy.h"

#include "Config.h"
#include "EmberfallBots.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Timer.h"

#include <ctime>

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
    // resting first: nothing else moves the bot while it eats (above the rpg walk, grinding and looting)
    triggers.push_back(new TriggerNode("ef rest", {NextAction("ef rest", ACTION_HIGH + 5)}));
    triggers.push_back(new TriggerNode("ef prepare", {NextAction("ef prepare", ACTION_HIGH)}));
    // the quest kinds the stock quest walk never finishes (EmberfallQuests.h): above the rpg walk (3.0)
    triggers.push_back(new TriggerNode("ef quest", {NextAction("ef quest", 4.0f)}));
    // a player's group: leave by itself when the leader has been away long enough
    triggers.push_back(new TriggerNode("ef group", {NextAction("ef group leave", ACTION_HIGH + 2)}));
}

namespace
{
    // The group's leader when it is a real player (a player invited this bot), else null.
    Player* PlayerLeader(PlayerbotAI* botAI)
    {
        Player* bot = botAI->GetBot();
        if (!bot->GetGroup())
            return nullptr;
        Player* leader = ObjectAccessor::FindConnectedPlayer(bot->GetGroup()->GetLeaderGUID());
        if (leader && GET_PLAYERBOT_AI(leader))
            return nullptr;
        return leader ? leader : bot;  // an offline leader counts as a player away
    }
}

bool EmberfallGroupTrigger::IsActive()
{
    if (!EmberfallBots::Enabled() || !bot->GetGroup())
    {
        joinedAt = awaySince = 0;
        return false;
    }
    Player* leader = PlayerLeader(botAI);
    if (!leader)
        return false;
    uint32 const now = static_cast<uint32>(std::time(nullptr));
    if (!joinedAt)
        joinedAt = now;
    bool const away = leader == bot || !leader->IsInWorld() || leader->GetMapId() != bot->GetMapId() ||
                      bot->GetDistance(leader) > 300.0f;
    if (!away)
        awaySince = 0;
    else if (!awaySince)
        awaySince = now;
    uint32 const awayLimit = sConfigMgr->GetOption<uint32>("Playerbots.Emberfall.GroupAwayMinutes", 10) * 60;
    uint32 const maxTime = sConfigMgr->GetOption<uint32>("Playerbots.Emberfall.GroupMaxMinutes", 180) * 60;
    return (awaySince && now - awaySince >= awayLimit) || now - joinedAt >= maxTime;
}

bool EmberfallGroupLeaveAction::Execute(Event /*event*/)
{
    LOG_DEBUG("playerbots", "Emberfall group: {} leaves its group", bot->GetName());
    botAI->LeaveOrDisbandGroup();
    return true;
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

uint32 EmberfallPickStep(PlayerbotAI* botAI, Unit* target, bool prepare, Unit** castOn, EmberfallBackoff const* backoff)
{
    uint32 const now = getMSTime();
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
        if (backoff)
            if (auto it = backoff->find(step.spell); it != backoff->end() && now < it->second)
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
        // a defensive: only with the bot's own health low, on itself when the ability has no range
        if (step.selfBelowPct)
        {
            if (bot->GetHealthPct() >= float(step.selfBelowPct))
                continue;
            if (info->GetMaxRange(false, bot) <= 0.0f)
                on = bot;
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

bool EmberfallRotationAction::isPossible() { return EmberfallPickStep(botAI, GetTarget(), false, nullptr, &backoff) != 0; }

bool EmberfallRotationAction::Execute(Event /*event*/)
{
    Unit* on = nullptr;
    uint32 const spell = EmberfallPickStep(botAI, GetTarget(), false, &on, &backoff);
    if (!spell || !on)
        return false;
    bool const ok = botAI->CastSpell(spell, on);
    // a cast the checks let through but the core refused (Set On without a companion, a script's own condition): the
    // next steps get their turn for 10 s
    if (!ok)
        backoff[spell] = getMSTime() + 10000;
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

bool EmberfallRestTrigger::IsActive()
{
    if (!EmberfallBots::Enabled() || !bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() || bot->IsMounted() ||
        bot->IsInWater())
        return false;
    if (EmberfallBots::NeedsRest(bot))
        return true;
    // once seated, until rested enough
    return bot->GetStandState() == UNIT_STAND_STATE_SIT && !EmberfallBots::RestedEnough(bot);
}

bool EmberfallRestAction::Execute(Event /*event*/)
{
    if (bot->isMoving())
        bot->StopMoving();
    // our provisions: food below full health, drink below full mana, each while its aura is not on
    bool const wantFood = bot->GetHealthPct() < 99.0f;
    bool const wantDrink = bot->GetMaxPower(POWER_MANA) > 0 && bot->GetPowerPct(POWER_MANA) < 99.0f;
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
    {
        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
            continue;
        ItemTemplate const* proto = item->GetTemplate();
        bool const food = wantFood && EmberfallBots::IsFood(proto);
        bool const drink = wantDrink && EmberfallBots::IsDrink(proto);
        if (!food && !drink)
            continue;
        uint32 const spell = proto->Spells[0].SpellId;
        if (bot->HasAura(spell) || bot->HasSpellCooldown(spell) || proto->RequiredLevel > bot->GetLevel())
            continue;
        if (bot->GetStandState() != UNIT_STAND_STATE_SIT)
            bot->SetStandState(UNIT_STAND_STATE_SIT);
        if (botAI->CastSpell(spell, bot, item))
        {
            bot->DestroyItemCount(item->GetEntry(), 1, true);
            LOG_DEBUG("playerbots", "Emberfall rest: {} {} ({}% health, {}% mana)", bot->GetName(), food ? "eats" : "drinks",
                      uint32(bot->GetHealthPct()), uint32(bot->GetPowerPct(POWER_MANA)));
            return true;
        }
    }
    if (bot->GetStandState() != UNIT_STAND_STATE_SIT)
        bot->SetStandState(UNIT_STAND_STATE_SIT);
    return true;
}
