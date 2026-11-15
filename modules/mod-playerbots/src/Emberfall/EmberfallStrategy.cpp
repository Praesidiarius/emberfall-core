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

#include "CellImpl.h"
#include "Config.h"
#include "EmberfallBots.h"
#include "EmberfallRotationLogic.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "SpellAuras.h"
#include "EmberfallFights.h"
#include "Group.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Timer.h"

#include <algorithm>
#include <ctime>
#include <list>

std::vector<NextAction> EmberfallCombatStrategy::getDefaultActions()
{
    // The routine first; a melee spec swings when nothing in its table can go, a ranged spec with a bow keeps its Auto
    // Shot going (a caster waits for its power).
    std::vector<NextAction> actions = {NextAction("ef rotation", ACTION_DEFAULT + 0.5f)};
    if (!EmberfallBots::FightsRanged(botAI->GetBot()))
        actions.push_back(NextAction("melee", ACTION_DEFAULT));
    else
    {
        actions.push_back(NextAction("ef auto shot", ACTION_DEFAULT + 0.2f));
        // a ranged spec with claws (the Ranger after its leap): white swings while a foe is in reach, without walking in
        if (EmberfallBots::Spec const* spec = EmberfallBots::SpecOf(botAI->GetBot()); spec && spec->swing)
            actions.push_back(NextAction("ef swing", ACTION_DEFAULT + 0.1f));
    }
    return actions;
}

void EmberfallCombatStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // above everything of the fight: a lost fight is left (EmberfallStrategy.h)
    triggers.push_back(new TriggerNode("ef escape", {NextAction("ef escape", ACTION_EMERGENCY + 5)}));
}

bool EmberfallEscapeTrigger::IsActive()
{
    float const below = EmberfallBots::EscapeBelowPct();
    if (below <= 0.0f || !bot->IsAlive() || !bot->IsInCombat() || bot->GetGroup())
        return false;
    if (EmberfallFights::Escaping(bot))
        return true;
    if (bot->GetHealthPct() >= below)
        return false;
    float toKill = 0.0f, toDie = 0.0f;
    if (!EmberfallFights::Losing(bot, &toKill, &toDie))
        return false;
    LOG_DEBUG("playerbots", "Emberfall escape: {} (level {}) runs at {}% health: {:.1f} s to die, {:.1f} s to kill",
              bot->GetName(), bot->GetLevel(), uint32(bot->GetHealthPct()), toDie, toKill);
    EmberfallFights::NoteEscape(bot);
    return true;
}

bool EmberfallEscapeAction::Execute(Event /*event*/)
{
    // away from the nearest foe, a long stride at a time; while the stride lasts the bot keeps running
    Unit* nearest = nullptr;
    float best = 0.0f;
    for (auto const& [guid, ref] : bot->GetCombatManager().GetPvECombatRefs())
        if (Unit* other = ref->GetOther(bot))
        {
            float const d = bot->GetDistance(other);
            if (!nearest || d < best)
            {
                nearest = other;
                best = d;
            }
        }
    if (!nearest)
        return false;
    if (bot->IsNonMeleeSpellCast(false))
        bot->InterruptNonMeleeSpells(false);
    if (bot->isMoving() && bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
        return true;
    return MoveAway(nearest, 25.0f);
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

namespace
{
    // The bot's and its target's facts for a step's conditions (EmberfallRotationLogic.h), each asked for only when a
    // condition needs it; the enemies near the target counted once a pick.
    class BotFacts : public EmberfallRotation::Logic::Facts
    {
    public:
        BotFacts(Player* bot, Unit* target, EmberfallTargetClock* clock) : bot(bot), target(target), clock(clock) {}

        float Power() override
        {
            Powers const type = bot->getPowerType();
            float const scale = (type == POWER_RAGE || type == POWER_RUNIC_POWER) ? 10.0f : 1.0f;
            return float(bot->GetPower(type)) / scale;
        }
        float SelfAuraLeft(uint32_t spell) override { return Left(bot->GetAura(spell)); }
        float TargetAuraLeft(uint32_t spell) override { return target ? Left(target->GetAura(spell, bot->GetGUID())) : -1.0f; }
        uint32_t SelfStacks(uint32_t spell) override { return Stacks(bot->GetAura(spell)); }
        uint32_t TargetStacks(uint32_t spell) override { return target ? Stacks(target->GetAura(spell, bot->GetGUID())) : 0; }
        float CooldownLeft(uint32_t spell) override
        {
            return bot->HasSpell(spell) ? float(bot->GetSpellCooldownDelay(spell)) / 1000.0f : -1.0f;
        }
        uint32_t Enemies() override
        {
            if (enemies >= 0)
                return uint32(enemies);
            enemies = 0;
            if (target)
            {
                std::list<Unit*> units;
                Acore::AnyUnfriendlyUnitInObjectRangeCheck check(target, bot, 10.0f);
                Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(target, units, check);
                Cell::VisitObjects(target, searcher, 10.0f);
                for (Unit* u : units)
                    if (u == target || (u->IsAlive() && u->IsInCombat() && !u->IsCritter() && bot->IsHostileTo(u)))
                        ++enemies;
                if (std::find(units.begin(), units.end(), target) == units.end())
                    ++enemies;
            }
            return uint32(enemies);
        }
        float TimeToLive() override { return clock && target ? clock->TimeToLive(target) : -1.0f; }
        bool TargetCastsInterruptible() override
        {
            if (!target)
                return false;
            for (CurrentSpellTypes type : {CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL})
                if (Spell const* s = target->GetCurrentSpell(type))
                    if (s->getState() != SPELL_STATE_FINISHED && (s->GetSpellInfo()->InterruptFlags & SPELL_INTERRUPT_FLAG_INTERRUPT) &&
                        s->GetSpellInfo()->PreventionType != SPELL_PREVENTION_TYPE_NONE)
                        return true;
            return false;
        }

    private:
        static float Left(Aura const* aura)
        {
            if (!aura)
                return -1.0f;
            return aura->GetDuration() < 0 ? 3600.0f : float(aura->GetDuration()) / 1000.0f;
        }
        static uint32_t Stacks(Aura const* aura)
        {
            return aura ? std::max<uint32_t>(aura->GetStackAmount(), aura->GetCharges()) : 0;
        }

        Player* bot;
        Unit* target;
        EmberfallTargetClock* clock;
        int32 enemies = -1;
    };
}

float EmberfallTargetClock::TimeToLive(Unit* target)
{
    uint32 const now = getMSTime();
    if (target->GetGUID() != guid)
    {
        guid = target->GetGUID();
        since = now;
        health = target->GetHealth();
    }
    return EmberfallRotation::Logic::TimeToLive(health, target->GetHealth(), float(getMSTimeDiff(since, now)) / 1000.0f);
}

uint32 EmberfallPickStep(PlayerbotAI* botAI, Unit* target, bool prepare, Unit** castOn, EmberfallBackoff const* backoff,
                         EmberfallTargetClock* clock)
{
    uint32 const now = getMSTime();
    Player* bot = botAI->GetBot();
    EmberfallBots::Spec const* spec = EmberfallBots::SpecOf(bot);
    if (!spec)
        return 0;
    if (clock && target && target->IsAlive())
        clock->TimeToLive(target);  // starts the clock at the first look at a new target
    BotFacts facts(bot, target, clock);
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
            // with two or more creatures on it the bot does not wait as long (DefensiveCrowdBonusPct, bot deaths)
            float below = float(step.selfBelowPct);
            if (EmberfallBots::DefensiveCrowdBonusPct() > 0.0f && bot->getAttackers().size() >= 2)
                below += EmberfallBots::DefensiveCrowdBonusPct();
            if (bot->GetHealthPct() >= below)
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
        // the step's own conditions (bot rotations, docs §18): its power tier, auras, stacks, cooldowns, enemies, ...
        if (!step.conds.empty() && !EmberfallRotation::Logic::AllHold(step.conds, facts))
            continue;
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

bool EmberfallRotationAction::isPossible()
{
    return EmberfallPickStep(botAI, GetTarget(), false, nullptr, &backoff, &clock) != 0;
}

bool EmberfallRotationAction::Execute(Event /*event*/)
{
    Unit* on = nullptr;
    uint32 const spell = EmberfallPickStep(botAI, GetTarget(), false, &on, &backoff, &clock);
    if (!spell || !on)
        return false;
    bool const ok = botAI->CastSpell(spell, on);
    // a cast the checks let through but the core refused (Set On without a companion, a script's own condition): the
    // next steps get their turn for 10 s
    if (!ok)
        backoff[spell] = getMSTime() + 10000;
    else if (EmberfallBots::Spec const* spec = EmberfallBots::SpecOf(bot))
        for (EmberfallBots::RoutineStep const& step : spec->steps)
            if (step.spell == spell && step.selfBelowPct)
            {
                EmberfallFights::NoteDefensive(bot);
                break;
            }
    // the pilot's trace (Playerbots.log at debug): what each bot presses
    LOG_DEBUG("playerbots", "Emberfall rotation: {} (level {}) casts {} on {}: {}", bot->GetName(), bot->GetLevel(), spell,
              on->GetName(), ok ? "ok" : "failed");
    return ok;
}

namespace
{
    uint32 constexpr SPELL_AUTO_SHOT = 75;
}

bool EmberfallAutoShotAction::isPossible()
{
    Unit* target = GetTarget();
    if (!target || !target->IsAlive() || !bot->HasSpell(SPELL_AUTO_SHOT))
        return false;
    Item* ranged = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
    if (!ranged || ranged->GetTemplate()->Class != ITEM_CLASS_WEAPON)
        return false;
    // already shooting at it
    if (Spell* current = bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
        if (current->m_targets.GetUnitTargetGUID() == target->GetGUID())
            return false;
    SpellInfo const* info = sSpellMgr->GetSpellInfo(SPELL_AUTO_SHOT);
    return info && InReach(bot, target, info) && botAI->CanCastSpell(SPELL_AUTO_SHOT, target);
}

bool EmberfallAutoShotAction::Execute(Event /*event*/)
{
    Unit* target = GetTarget();
    return target && botAI->CastSpell(SPELL_AUTO_SHOT, target);
}

bool EmberfallSwingAction::isPossible()
{
    Unit* target = GetTarget();
    return target && target->IsAlive() && bot->IsWithinMeleeRange(target) &&
           !(bot->GetVictim() == target && bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING));
}

bool EmberfallSwingAction::Execute(Event /*event*/)
{
    Unit* target = GetTarget();
    return target && bot->Attack(target, true);
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
    return bot->getStandState() == UNIT_STAND_STATE_SIT && !EmberfallBots::RestedEnough(bot);
}

bool EmberfallRestAction::Execute(Event /*event*/)
{
    if (bot->isMoving())
        bot->StopMoving();
    // our provisions: food below full health, drink below full mana, each while its aura is not on. Used as a player
    // uses the item (the core seats the bot when the aura lands and takes the item); not through PlayerbotAI::CastSpell,
    // which stands a seated bot up and fails, so the step-2 run's bots sat and stood without ever eating.
    bool const wantFood = bot->GetHealthPct() < 99.0f && !bot->HasAuraType(SPELL_AURA_MOD_REGEN);
    bool const wantDrink = bot->GetMaxPower(POWER_MANA) > 0 && bot->GetPowerPct(POWER_MANA) < 99.0f &&
                           !bot->HasAuraType(SPELL_AURA_MOD_POWER_REGEN);
    bool carries = false;
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
    {
        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
            continue;
        ItemTemplate const* proto = item->GetTemplate();
        if (EmberfallBots::IsFood(proto) || EmberfallBots::IsDrink(proto))
            carries = true;
        bool const food = wantFood && EmberfallBots::IsFood(proto);
        bool const drink = wantDrink && EmberfallBots::IsDrink(proto);
        if (!food && !drink)
            continue;
        uint32 const spell = proto->Spells[0].SpellId;
        if (bot->HasAura(spell) || bot->HasSpellCooldown(spell) || proto->RequiredLevel > bot->GetLevel())
            continue;
        if (bot->IsNonMeleeSpellCast(false))
            return true;
        if (getMSTime() < retryAt)
            break;
        SpellCastTargets targets;
        targets.SetUnitTarget(bot);
        bot->CastItemUseSpell(item, targets, 1, 0);  // the item may be gone after this
        bool const ok = bot->HasAura(spell);
        LOG_DEBUG("playerbots", "Emberfall rest: {} {} ({}% health, {}% mana): {}", bot->GetName(),
                  food ? "eats" : "drinks", uint32(bot->GetHealthPct()), uint32(bot->GetPowerPct(POWER_MANA)),
                  ok ? "ok" : "failed");
        if (ok)
            return true;
        retryAt = getMSTime() + 30000;  // a refused use (a shared cooldown): sit and regenerate, try again in 30 s
        break;
    }
    // eaten up (a stack lasts a bot about an hour; the level-up maintenance restocks it, but high levels come slowly):
    // new provisions as if bought, at most every 10 minutes; this rest is a sit without them
    if (!carries && getMSTime() >= restockAt)
    {
        restockAt = getMSTime() + 600000;
        PlayerbotFactory factory(bot, bot->GetLevel());
        factory.InitFood();
    }
    if (bot->getStandState() != UNIT_STAND_STATE_SIT)
        bot->SetStandState(UNIT_STAND_STATE_SIT);
    return true;
}
