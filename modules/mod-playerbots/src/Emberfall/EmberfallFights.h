/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version. Emberfall's part of the module (Copyright Emberfall / Tales of Orrin).
 */

/*
 * Emberfall's part of mod-playerbots: a record of each fight of a bot in Emberfall mode, for the death measurements
 * (Emberfall docs/design/playerbots-pilot.md §13). From the bot's entry into a fight to its end it notes the bot's
 * health and mana at the start, what it was doing, who struck first, how many creatures were in the fight at the pull,
 * at most and at the end, their levels, the creatures standing near the first one, the damage dealt and taken, the
 * defensives used. One line in Playerbots.log at the end: "Emberfall death:" (info) when the bot died, "Emberfall
 * fight:" (debug) when it lived. Observation only: nothing here changes a fight. Released under GNU GPL v2 or later.
 */

#ifndef PLAYERBOTS_EMBERFALLFIGHTS_H
#define PLAYERBOTS_EMBERFALLFIGHTS_H

#include "Define.h"

class Player;

namespace EmberfallFights
{
    // The rotation cast a defensive (a step with self_below_pct) successfully.
    void NoteDefensive(Player* bot);
    // The bot ran from the fight (EmberfallStrategy's escape); Escaping: it has in this fight.
    void NoteEscape(Player* bot);
    bool Escaping(Player* bot);
    // The health (or mana) share the bot's last fights lived cost it: the larger of the last two, in percent.
    float LastLossPct(Player* bot, bool mana);
    // The fight so far says the bot dies before its foes: its health over the damage it takes a second against the
    // foes' health over the damage it deals a second (from 3 s into the fight).
    bool Losing(Player* bot, float* secondsToKill = nullptr, float* secondsToDie = nullptr);
    // The script hooks (a player script and a unit script); from AddPlayerbotsScripts.
    void AddScripts();
}

#endif
