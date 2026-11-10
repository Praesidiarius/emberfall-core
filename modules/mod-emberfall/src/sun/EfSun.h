/*
 * This file is part of Emberfall's AzerothCore fork (modules/mod-emberfall). See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef EMBERFALL_SUN_H
#define EMBERFALL_SUN_H

// The sun's hours for the rest of the module (src/emberfall_sun.cpp keeps them; Emberfall docs/design/atmosphere.md
// "The sun's hours"). Anything on the server that goes by the time of day asks here, never the clock's hour itself:
// the clients read the same part of the day from the same sunrise and sunset (the EFSUN message). Safe from any thread.

#include "sun/EfSunTime.h"

namespace Emberfall::Sun
{
    // Today's sunrise and sunset in hours of the server's local clock (the mode's: real, softened or fixed).
    efsun::SunHours Hours();
    // The sun clock now (0..24): the server's local time mapped so that today's sunrise is 06:00 and its sunset 19:45.
    double SunClockNow();
    // Which part of the day it is now (dawn 05-08, day 08-18, dusk 18-21, night 21-05 on the sun clock).
    efsun::Part PartNow();
}

#endif
