#!/bin/sh
# Builds and runs mod-emberfall's standalone tests: the talent tests (tests/talent_tests.cpp: the rules vectors through
# the module's TalentRules.h, the tree model, the EFT protocol and a decode fuzz) and the companion tests
# (tests/companion_tests.cpp: src/companions/EfCompanionLogic.h). No core, no database:
#
#   modules/mod-emberfall/tests/run_tests.sh [VECTORS.json ...]
#
# Default vectors: tests/data/talent_rules_example.json, a copy of Emberfall's
# tools/py/tests/data/talent_rules/example.json (Emberfall's test_talents.py checks that the copy and
# src/talents/TalentRules.h equal the client's). CXX (default c++) needs C++20.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${TMPDIR:-/tmp}/mod-emberfall-talent-tests.$$
trap 'rm -f "$OUT" "$OUT.companions"' EXIT
${CXX:-c++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$HERE/../src/talents" \
    "$HERE/talent_tests.cpp" -o "$OUT"
${CXX:-c++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$HERE/../src/companions" \
    "$HERE/companion_tests.cpp" -o "$OUT.companions"
if [ $# -eq 0 ]; then set -- "$HERE/data/talent_rules_example.json" "$HERE/data/talent_rules_frostguard.json"; fi
"$OUT" "$@"
"$OUT.companions"
