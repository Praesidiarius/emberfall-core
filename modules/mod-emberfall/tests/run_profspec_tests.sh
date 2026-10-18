#!/bin/sh
# Builds and runs mod-emberfall's standalone profession specialisation tests (tests/profspec_tests.cpp over
# src/profspecs/EfProfModel.h and the talents' TalentRules.h). No core, no database:
#
#   modules/mod-emberfall/tests/run_profspec_tests.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${TMPDIR:-/tmp}/mod-emberfall-profspec-tests.$$
trap 'rm -f "$OUT"' EXIT
${CXX:-c++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$HERE/../src/profspecs" \
    "$HERE/profspec_tests.cpp" -o "$OUT"
"$OUT"
