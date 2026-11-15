/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version. Emberfall's part of the module (Copyright Emberfall / Tales of Orrin).
 */

/*
 * Emberfall's part of mod-playerbots: a bot's spec power (Emberfall docs/design/playerbots-pilot.md §16). mod-emberfall
 * sets a spec's own power (the Pyromancer's Heat, the Stormbringer's Static, the Corsair's Energy, the Executioner's
 * Openings, the Damnator's Seals; the Hunter's Focus is its class's) at login, level change and the spec choice
 * (ClassKit::ApplySpecPower). The bot factory's randomize (PlayerbotFactory::RandomizeEmberfall: a re-level of the
 * follow rule, the module's own re-rolls) calls Player::InitStatsForLevel, which puts the class's power back
 * (Player::InitDataForForm): Apply sets the spec's again. Check compares a bot's power type with its spec's and logs it
 * ("Emberfall power:", an error when it differs): after the randomize and the refresh at once, after a login and a level
 * up a few seconds later (the hooks of both modules have run by then). Released under GNU GPL v2 or later.
 */

#ifndef PLAYERBOTS_EMBERFALLPOWER_H
#define PLAYERBOTS_EMBERFALLPOWER_H

#include "Define.h"

class Player;

namespace EmberfallPower
{
    // The spec's own power or the class's again (mod-emberfall's ClassKit::ApplySpecPower); nothing without it, or with
    // Playerbots.Emberfall.KeepSpecPower 0 (default 1; 0 only to show the old fault).
    void Apply(Player* bot);
    // The power type the bot's active spec runs on (-1 when mod-emberfall's class kits are not in the build).
    int32 Expected(Player* bot);
    // Compares and logs ("Emberfall power:", debug when right, an error when not); true when right.
    bool Check(Player* bot, char const* when);
    // The login and level-up checks (a player script) and their queue, from the world tick.
    void AddScripts();
    void Update(uint32 diff);
}

#endif
