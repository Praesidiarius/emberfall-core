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

// Standalone tests of the active spec's place in UNIT_FIELD_BYTES_1 (src/talents/EfSpecField.h; tests/run_tests.sh
// builds and runs them). The golden values are the client's too (client/tests/UnitSpecTests.cpp): a change here is a
// change of what every client reads.

#include "EfSpecField.h"

#include <iostream>
#include <string>

using namespace Emberfall::Talents;

namespace
{
    int g_failed = 0, g_checks = 0;

    void check(bool ok, std::string const& what)
    {
        ++g_checks;
        if (!ok)
        {
            ++g_failed;
            std::cerr << "FAIL: " << what << "\n";
        }
    }

    // The field as the core sets it: byte 0 the stand state, `kByte` the spec, byte 2 the visibility flags.
    uint32_t Field(uint8_t stand, uint8_t spec, uint8_t vis) { return stand | (uint32_t(spec) << (8 * SpecField::kByte)) | (uint32_t(vis) << 16); }
}

int main()
{
    check(SpecField::kByte == 1, "the spec rides in the field's second byte (UNIT_BYTES_1_OFFSET_PET_TALENTS)");
    check(SpecField::Encode(12) == 12 && SpecField::Encode(93) == 93, "spec IDs go as they are (the Corsair 12, the Damnator 93)");
    check(SpecField::Encode(0) == 0, "no spec: 0");
    check(SpecField::Encode(300) == 0, "an ID beyond a byte is not shown (the client falls back)");
    // golden: the client reads 0x00020C08 (kneeling, the Corsair, stealth flag) as spec 12
    check(Field(8, 12, 2) == 0x00020C08u, "the golden field value");
    check(SpecField::Decode(0x00020C08u) == 12, "the golden field reads as the Corsair");
    check(SpecField::Decode(0x00000008u) == 0, "a stand state alone is no spec");
    check(SpecField::Decode(Field(0, 93, 0)) == 93, "the Damnator");

    std::cout << "specfield: " << (g_checks - g_failed) << "/" << g_checks << " checks passed\n";
    return g_failed ? 1 : 0;
}
