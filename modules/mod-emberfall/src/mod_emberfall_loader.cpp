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

// Emberfall (PATCHES.md, "Module"): the loader AzerothCore's modules/CMakeLists.txt calls for the
// directory mod-emberfall (Add<directory with _ for ->Scripts()).

void AddEmberfallOwnDataScripts();
void AddEmberfallTalentsScripts();

void Addmod_emberfallScripts()
{
    AddEmberfallOwnDataScripts();
    AddEmberfallTalentsScripts();
}
