/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version. Emberfall's part of the module (Copyright Emberfall / Tales of Orrin).
 */

/*
 * Emberfall's part of mod-playerbots: the idle trace (Emberfall docs/design/playerbots-pilot.md §15). What a bot in
 * Emberfall mode does after a fight it lived, from the fight's end until its next fight, its death, a walk that takes it
 * 25 yd from the spot, a flight or a teleport, at most 120 s: the time it stood, sat, walked, cast and looted, the first
 * purposeful act (loot, eat, walk, cast, talk, the next fight) and, for the standing, what held it (the action the bot's
 * brain ran last before each wait, whether that wait was the action's own delay, and whether the bot was set inactive).
 * One line in Playerbots.log at the window's end ("Emberfall idle:", info). Off unless Playerbots.Emberfall.IdleTrace = 1;
 * observation only: nothing here changes what a bot does. Released under GNU GPL v2 or later.
 */

#ifndef PLAYERBOTS_EMBERFALLIDLE_H
#define PLAYERBOTS_EMBERFALLIDLE_H

#include "Define.h"

#include <string>

class Player;

namespace EmberfallIdle
{
    // Playerbots.Emberfall.IdleTrace (default 0: off), read with the other Emberfall settings.
    bool Enabled();
    void Load();
    // The bot left a fight alive (EmberfallFights' leave-combat hook): a window opens.
    void FightEnded(Player* bot);
    // Each AI update of a bot (PlayerbotAI::UpdateAI): aiDelay is the time until its brain runs again, active whether
    // the module lets it act (PlayerbotAI::AllowActivity; an inactive bot waits PassiveDelay).
    void Tick(Player* bot, uint32 aiDelay, bool active);
    // The bot's brain ran an action (Engine::ListenAndExecute), or "none" when a run executed nothing.
    void NoteAction(Player* bot, std::string const& name);
}

#endif
