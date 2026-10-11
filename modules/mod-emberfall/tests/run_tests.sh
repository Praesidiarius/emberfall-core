#!/bin/sh
# Builds and runs mod-emberfall's standalone talent tests (tests/talent_tests.cpp): the rules vectors through
# the module's TalentRules.h, the tree model, the EFT protocol and a decode fuzz. No core, no database:
#
#   modules/mod-emberfall/tests/run_tests.sh [VECTORS.json ...]
#
# Default vectors: tests/data/talent_rules_example.json, a copy of Emberfall's
# tools/py/tests/data/talent_rules/example.json (Emberfall's test_talents.py checks that the copy and
# src/talents/TalentRules.h equal the client's). CXX (default c++) needs C++20.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${TMPDIR:-/tmp}/mod-emberfall-talent-tests.$$
trap 'rm -f "$OUT"' EXIT
${CXX:-c++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$HERE/../src/talents" \
    "$HERE/talent_tests.cpp" -o "$OUT"
if [ $# -eq 0 ]; then set -- "$HERE/data/talent_rules_example.json"; fi
"$OUT" "$@"
