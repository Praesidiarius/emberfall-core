/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version. Emberfall's part of the module (Copyright Emberfall / Tales of Orrin).
 */

/*
 * Bot rotations (Emberfall docs/design/playerbots-pilot.md §18): the conditions of a routine step (content/data/bots/
 * specs.yaml `if:`, written by the game repo's botpilot into ef_bot_routine.conds) as pure logic with no core types, so
 * mod-emberfall's standalone tests (tests/botrotation_tests.cpp) run them without a server. EmberfallStrategy.cpp feeds
 * the facts of the bot and its target through Facts.
 *
 * The encoding: "[!]OP:SPELL:VALUE" items joined by ";", all must hold; "!" negates the item.
 *   P  the bot's power >= VALUE (the shown value: a spec power in the rage or runic slot counts in its tenths)
 *   S  the bot has aura SPELL with VALUE seconds or more left (0: on at all; a permanent aura counts as long)
 *   T  the target has the bot's own aura SPELL, the same
 *   K  the bot's aura SPELL has VALUE stacks or charges or more
 *   U  the bot's own aura SPELL on the target has VALUE stacks or more
 *   R  the bot knows SPELL and its cooldown is over within VALUE seconds
 *   E  VALUE or more enemies in the fight near the target (the target counted)
 *   L  the target lives VALUE seconds or more by the damage it has taken (unknown: holds)
 *   C  the target casts a spell that can be interrupted
 *   N  a swing is queued: an on-next-swing spell waits for the bot's main hand (the Executioner v4.0's Brutal Swing,
 *      Final Verdict, Whirling Steel: Emberfall docs/design/classes/warrior-executioner.md §21)
 *   W  the main hand's running swing is at VALUE percent of its time or more (no running swing: 100; the sweet spot)
 */

#ifndef PLAYERBOTS_EMBERFALLROTATIONLOGIC_H
#define PLAYERBOTS_EMBERFALLROTATIONLOGIC_H

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace EmberfallRotation::Logic
{
    struct Cond
    {
        char op = 0;
        bool negate = false;
        uint32_t spell = 0;
        float value = 0.0f;
    };

    // What a condition asks about the bot and its target, answered by the caller (lazily: enemies and the time to live
    // cost more than an aura lookup).
    class Facts
    {
    public:
        virtual ~Facts() = default;
        virtual float Power() = 0;                                     // the bot's power, as shown
        virtual float SelfAuraLeft(uint32_t spell) = 0;                // seconds; < 0: no such aura; a permanent one: large
        virtual float TargetAuraLeft(uint32_t spell) = 0;              // the bot's own aura on the target
        virtual uint32_t SelfStacks(uint32_t spell) = 0;               // stacks or charges, 0 without the aura
        virtual uint32_t TargetStacks(uint32_t spell) = 0;
        virtual float CooldownLeft(uint32_t spell) = 0;                // seconds; < 0: the bot does not know it
        virtual uint32_t Enemies() = 0;
        virtual float TimeToLive() = 0;                                // seconds; < 0: unknown
        virtual bool TargetCastsInterruptible() = 0;
        virtual bool SwingQueued() { return false; }                   // an on-next-swing spell waits
        virtual float SwingProgress() { return 100.0f; }               // percent of the running swing (none: 100)
    };

    // The items of an encoded string; malformed items are left out (the generator's check refuses them).
    inline std::vector<Cond> Parse(std::string const& text)
    {
        std::vector<Cond> out;
        size_t start = 0;
        while (start < text.size())
        {
            size_t end = text.find(';', start);
            if (end == std::string::npos)
                end = text.size();
            std::string item = text.substr(start, end - start);
            start = end + 1;
            Cond c;
            size_t i = 0;
            if (i < item.size() && item[i] == '!')
            {
                c.negate = true;
                ++i;
            }
            size_t const a = item.find(':', i), b = a == std::string::npos ? a : item.find(':', a + 1);
            if (a != i + 1 || b == std::string::npos)
                continue;
            c.op = item[i];
            c.spell = static_cast<uint32_t>(std::strtoul(item.c_str() + a + 1, nullptr, 10));
            c.value = std::strtof(item.c_str() + b + 1, nullptr);
            if (std::string("PSTKURELCNW").find(c.op) == std::string::npos)
                continue;
            out.push_back(c);
        }
        return out;
    }

    inline bool HoldsPlain(Cond const& c, Facts& f)
    {
        switch (c.op)
        {
            case 'P': return f.Power() >= c.value;
            case 'S': { float const left = f.SelfAuraLeft(c.spell); return left >= 0.0f && left >= c.value; }
            case 'T': { float const left = f.TargetAuraLeft(c.spell); return left >= 0.0f && left >= c.value; }
            case 'K': return f.SelfStacks(c.spell) >= c.value;
            case 'U': return f.TargetStacks(c.spell) >= c.value;
            case 'R': { float const left = f.CooldownLeft(c.spell); return left >= 0.0f && left <= c.value; }
            case 'E': return float(f.Enemies()) >= c.value;
            case 'L': { float const ttl = f.TimeToLive(); return ttl < 0.0f || ttl >= c.value; }
            case 'C': return f.TargetCastsInterruptible();
            case 'N': return f.SwingQueued();
            case 'W': return f.SwingProgress() >= c.value;
        }
        return true;
    }

    inline bool Holds(Cond const& c, Facts& f) { return HoldsPlain(c, f) != c.negate; }

    inline bool AllHold(std::vector<Cond> const& conds, Facts& f)
    {
        for (Cond const& c : conds)
            if (!Holds(c, f))
                return false;
        return true;
    }

    // A kiting step (ef_bot_spec.kite, the Marksman since ankarna's notes of 2026-11-19: no Loose, the companion no longer
    // holds the enemy, Patient Shot casts on the move): a ranged bot steps away from a foe that attacks it within
    // `reach` yards, unless it is rooted, in a group (a tank there), or casting a shot that the step would break (a
    // planted cast); one step every `everyMs` at most, so it shoots in between rather than running the whole fight.
    struct KiteFacts
    {
        bool kite = false;           // the spec kites
        bool ranged = false;         // it fights at range now (the spec's, or its pre-spec range below 10)
        bool grouped = false;
        bool rooted = false;
        bool castingPlanted = false; // a cast that moving breaks (Full Draw) runs
        float foeDistance = -1.0f;   // the nearest foe attacking the bot (-1: none)
        float reach = 5.0f;
        uint32_t nowMs = 0;
        uint32_t lastStepMs = 0;     // 0: never
        uint32_t everyMs = 6000;
    };

    inline bool KiteStep(KiteFacts const& k)
    {
        if (!k.kite || !k.ranged || k.grouped || k.rooted || k.castingPlanted)
            return false;
        if (k.foeDistance < 0.0f || k.foeDistance > k.reach)
            return false;
        return k.lastStepMs == 0 || k.nowMs - k.lastStepMs >= k.everyMs;
    }

    // The target's time to live from two health samples: the health it had when the bot first fought it, the health
    // now, the seconds between. Unknown (-1) in the first `minSeconds` or while it has lost nothing.
    inline float TimeToLive(uint32_t healthAtStart, uint32_t healthNow, float seconds, float minSeconds = 3.0f)
    {
        if (seconds < minSeconds || healthNow >= healthAtStart)
            return -1.0f;
        float const perSecond = float(healthAtStart - healthNow) / seconds;
        return float(healthNow) / perSecond;
    }
}

#endif
