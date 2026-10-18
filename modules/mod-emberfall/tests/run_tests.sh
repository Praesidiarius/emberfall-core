#!/bin/sh
# Builds and runs mod-emberfall's standalone tests: the talent tests (tests/talent_tests.cpp: the rules vectors through
# the module's TalentRules.h, the tree model, the EFT protocol and a decode fuzz) and the companion tests
# (tests/companion_tests.cpp: src/companions/EfCompanionLogic.h) and the dungeon tests (tests/dungeon_tests.cpp:
# src/dungeons/EfDungeonLogic.h, the dev warden's gating and the barrows' seals) and the unstuck tests
# (tests/unstuck_tests.cpp: src/unstuck/EfUnstuckLogic.h, the nearest town, the entrance, the rules) and the absorb tests
# (tests/absorb_tests.cpp: src/absorbs/EfAbsorbLogic.h, the EFA messages, the throttle, the caps) and the harbour tests
# (tests/harbour_tests.cpp: src/harbours/EfHarbourLogic.h, the pier's stop and the next sailing). No core, no database:
#
#   modules/mod-emberfall/tests/run_tests.sh [VECTORS.json ...]
#
# Default vectors: tests/data/talent_rules_example.json, a copy of Emberfall's
# tools/py/tests/data/talent_rules/example.json (Emberfall's test_talents.py checks that the copy and
# src/talents/TalentRules.h equal the client's). CXX (default c++) needs C++20.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${TMPDIR:-/tmp}/mod-emberfall-talent-tests.$$
trap 'rm -f "$OUT" "$OUT.companions" "$OUT.dungeons" "$OUT.unstuck" "$OUT.absorbs" "$OUT.harbours"' EXIT
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
if [ $# -eq 0 ]; then set -- "$HERE/data/talent_rules_example.json" "$HERE/data/talent_rules_frostguard.json"; fi
"$OUT" "$@"
"$OUT.companions"
"$OUT.dungeons"
"$OUT.unstuck"
"$OUT.absorbs"
"$OUT.harbours"
