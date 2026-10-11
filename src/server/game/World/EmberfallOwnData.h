/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
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

// EMBERFALL: own data (PATCHES.md #0) - the Emberfall.OwnData switch (worldserver.conf or the
// environment variable AC_EMBERFALL_OWN_DATA; default 0 = upstream behaviour). A new file, so it
// never conflicts with upstream; the core hunks and modules/mod-emberfall read it.

#ifndef EMBERFALL_OWN_DATA_H
#define EMBERFALL_OWN_DATA_H

#include "Config.h"

namespace Emberfall
{
    inline bool OwnData()
    {
        return sConfigMgr->GetOption<bool>("Emberfall.OwnData", false, false);
    }
}

#endif
