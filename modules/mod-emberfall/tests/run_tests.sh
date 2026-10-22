#!/bin/sh
# Builds and runs mod-emberfall's standalone tests: the talent tests (tests/talent_tests.cpp: the rules vectors through
# the module's TalentRules.h, the tree model, the EFT protocol and a decode fuzz) and the companion tests
# (tests/companion_tests.cpp: src/companions/EfCompanionLogic.h) and the dungeon tests (tests/dungeon_tests.cpp:
# src/dungeons/EfDungeonLogic.h, the dev warden's gating and the barrows' seals) and the unstuck tests
# (tests/unstuck_tests.cpp: src/unstuck/EfUnstuckLogic.h, the nearest town, the entrance, the rules) and the absorb tests
# (tests/absorb_tests.cpp: src/absorbs/EfAbsorbLogic.h, the EFA messages, the throttle, the caps) and the harbour tests
# (tests/harbour_tests.cpp: src/harbours/EfHarbourLogic.h, the pier's stop and the next sailing) and the dev vendor tests
# (tests/devvendor_tests.cpp: src/devvendor/EfDevVendorLogic.h, the switch's gating) and the world editor tests
# (tests/editor_tests.cpp: src/editor/EfEditorLogic.h, the EFE requests, the rules, rates, ops and a decode fuzz) and the
# class kit tests (tests/classkit_tests.cpp: src/classkit/EfClassKitLogic.h, the self stacks' cap, gains and refunds) and
# the GM command tests (tests/gmcommand_tests.cpp: src/gmcommands/EfGmCommandLogic.h, `.modify speed` and `.die`, held rates)
# and the quest loot tests (tests/questloot_tests.cpp: src/questloot/EfQuestLootLogic.h, personal quest objects' lockouts)
# and the test zone tests (tests/testzone_tests.cpp: src/testzone/EfTestZoneLogic.h, `.eftest zone`'s route and plan)
# and the meter tests (tests/meter_tests.cpp: src/meter/EfMeterLogic.h, the damage meter's fights, attribution, maths, EFM)
# and the account friend tests (tests/accountfriends_tests.cpp: src/accountfriends/EfAccountFriendsLogic.h, EFF, ADD, limits).
# No core, no database:
#
#   modules/mod-emberfall/tests/run_tests.sh [VECTORS.json ...]
#
# Default vectors: tests/data/talent_rules_example.json, a copy of Emberfall's
# tools/py/tests/data/talent_rules/example.json (Emberfall's test_talents.py checks that the copy and
# src/talents/TalentRules.h equal the client's). CXX (default c++) needs C++20.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${TMPDIR:-/tmp}/mod-emberfall-talent-tests.$$
trap 'rm -f "$OUT" "$OUT.companions" "$OUT.dungeons" "$OUT.unstuck" "$OUT.absorbs" "$OUT.harbours" "$OUT.devvendor" "$OUT.editor" "$OUT.classkit" "$OUT.gmcommands" "$OUT.questloot" "$OUT.testzone" "$OUT.meter" "$OUT.accountfriends"' EXIT
${CXX:-c++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$HERE/../src/talents" \
    "$HERE/talent_tests.cpp" -o "$OUT"
${CXX:-c++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$HERE/../src/companions" \
    "$HERE/companion_tests.cpp" -o "$OUT.companions"
${CXX:-c++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$HERE/../src/dungeons" \
    "$HERE/dungeon_tests.cpp" -o "$OUT.dungeons"
${CXX:-c++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$HERE/../src/unstuck" \
    "$HERE/unstuck_tests.cpp" -o "$OUT.unstuck"
${CXX:-c++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$HERE/../src/absorbs" \
    "$HERE/absorb_tests.cpp" -o "$OUT.absorbs"
${CXX:-c++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$HERE/../src/harbours" \
    "$HERE/harbour_tests.cpp" -o "$OUT.harbours"
${CXX:-c++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$HERE/../src/devvendor" \
    "$HERE/devvendor_tests.cpp" -o "$OUT.devvendor"
${CXX:-c++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$HERE/../src/talents" \
    -I"$HERE/../src/editor" "$HERE/editor_tests.cpp" -o "$OUT.editor"
${CXX:-c++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$HERE/../src/classkit" \
    "$HERE/classkit_tests.cpp" -o "$OUT.classkit"
${CXX:-c++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$HERE/../src/gmcommands" \
    "$HERE/gmcommand_tests.cpp" -o "$OUT.gmcommands"
${CXX:-c++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$HERE/../src/questloot" \
    "$HERE/questloot_tests.cpp" -o "$OUT.questloot"
${CXX:-c++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$HERE/../src/testzone" \
    "$HERE/testzone_tests.cpp" -o "$OUT.testzone"
${CXX:-c++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$HERE/../src/meter" \
    "$HERE/meter_tests.cpp" -o "$OUT.meter"
${CXX:-c++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$HERE/../src/accountfriends" \
    "$HERE/accountfriends_tests.cpp" -o "$OUT.accountfriends"
if [ $# -eq 0 ]; then set -- "$HERE/data/talent_rules_example.json" "$HERE/data/talent_rules_frostguard.json"; fi
"$OUT" "$@"
"$OUT.companions"
"$OUT.dungeons"
"$OUT.unstuck"
"$OUT.absorbs"
"$OUT.harbours"
"$OUT.devvendor"
"$OUT.editor"
"$OUT.classkit"
"$OUT.gmcommands"
"$OUT.questloot"
"$OUT.testzone"
"$OUT.meter"
"$OUT.accountfriends"
