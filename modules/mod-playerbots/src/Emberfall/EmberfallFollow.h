/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version. Emberfall's part of the module (Copyright Emberfall / Tales of Orrin).
 */

/*
 * The bots follow a player (Emberfall docs/design/playerbots-pilot.md §14; Nathanael, 2026-11-15: "every 2 hours when
 * i am online ... 90% of them +/- 3 levels of my character and all of those 90% in the same level band zones").
 *
 * Off unless Playerbots.Emberfall.FollowAccounts names an account (env AC_PLAYERBOTS_EMBERFALL_FOLLOW_ACCOUNTS; a
 * realm's PLAYERBOTS_FOLLOW_ACCOUNTS). The followed character is the online character of the first listed account
 * that has one. At its login (once the bots are logged in, at most 10 minutes later), every FollowIntervalMinutes
 * (120) while it stays online, and at `.playerbots rndbot follow`, a pass runs: FollowShare (90) percent of each
 * side's online bots end within FollowLevels (3) of its level, in zones of their side whose band holds their level
 * (Playerbots.ZoneBracket.<zone> and Playerbots.Emberfall.ZoneSide.<zone>, from content/data/bots/zones.yaml), spread
 * over those zones (the fewest bots first; FollowMaxPerZone caps a zone, 0 none) and their quest givers and inns as
 * the placement at a bot's first login. A bot gets its new level by the module's own randomize path (level, our
 * trainers' spells, its spec's talents, gear by our weights, food, quest log reset); the rest stays spread.
 * A bot in a group, in a fight, dead, in an instance, on a vehicle or a flight, or within FollowSightYards (100) of a
 * real player is never touched; it is taken in a later pass. FollowBotsPerSecond (2) bots a second, so a pass does
 * not hitch the world. `.playerbots rndbot off` stops a running pass and starts none.
 */

#ifndef PLAYERBOTS_EMBERFALLFOLLOW_H
#define PLAYERBOTS_EMBERFALLFOLLOW_H

#include "Define.h"

#include <string>

namespace EmberfallFollow
{
    // Reads the settings (EmberfallBots::Load, at the module's start and at `.playerbots rndbot reload`).
    void Load();
    // Any account to follow configured (and Emberfall mode on).
    bool Enabled();
    // The world thread, every tick (Playerbots.cpp's world script): who is followed, when a pass is due, the pass.
    void Update(uint32 diff);
    // `.playerbots rndbot follow`: a pass now (if a followed character is online); the answer for the console.
    std::string RunNow();
    // `.playerbots rndbot follow status`: the settings, who is followed, the last and the next pass.
    std::string Status();
}

#endif
