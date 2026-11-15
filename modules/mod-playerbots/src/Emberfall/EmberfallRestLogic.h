/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version. Emberfall's part of the module (Copyright Emberfall / Tales of Orrin).
 */

/*
 * Bot idle (Emberfall docs/design/playerbots-pilot.md §15): when a seated bot keeps its seat, as pure logic with no core
 * types, so mod-emberfall's standalone tests (tests/botrest_tests.cpp) run it without a server. EmberfallBots::KeepsSeat
 * feeds it the bot's state; the module's think (PlayerbotAI::DoNextAction) stands a seated bot without a master up only
 * when it does not.
 */

#ifndef PLAYERBOTS_EMBERFALLRESTLOGIC_H
#define PLAYERBOTS_EMBERFALLRESTLOGIC_H

namespace EmberfallRest::Logic
{
    struct SeatState
    {
        bool on = true;          // Playerbots.Emberfall.RestSeated (an Emberfall bot with a spec)
        bool seated = false;
        bool inCombat = false;   // a fight always stands it up (the combat code does too)
        bool eating = false;     // a food aura (health regeneration while seated)
        bool drinking = false;   // a drink aura (mana regeneration while seated)
        bool restedEnough = false;
    };

    // A seated bot out of a fight keeps its seat while it eats or drinks, or is not rested enough yet; otherwise the
    // stock stand-up applies (a bot seated by something else, once rested, stands up as before).
    inline bool KeepsSeat(SeatState const& s)
    {
        if (!s.on || !s.seated || s.inCombat)
            return false;
        return s.eating || s.drinking || !s.restedEnough;
    }
}

#endif
