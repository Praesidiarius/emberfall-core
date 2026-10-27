#!/bin/sh
# Builds and runs mod-emberfall's standalone tests: the talent tests (tests/talent_tests.cpp: the rules vectors through
# the module's TalentRules.h, the tree model, the EFT protocol and a decode fuzz) and the companion tests
# (tests/companion_tests.cpp: src/companions/EfCompanionLogic.h) and the dungeon tests (tests/dungeon_tests.cpp:
# src/dungeons/EfDungeonLogic.h, the dev warden's gating and the barrows' seals) and the unstuck tests
# (tests/unstuck_tests.cpp: src/unstuck/EfUnstuckLogic.h, the nearest town, the entrance, the rules; EfUnstuckNearby.h,
# nearby unstuck's rings, spot judging and picking, rules, messages) and the absorb tests
# (tests/absorb_tests.cpp: src/absorbs/EfAbsorbLogic.h, the EFA messages, the throttle, the caps) and the harbour tests
# (tests/harbour_tests.cpp: src/harbours/EfHarbourLogic.h, the pier's stop and the next sailing) and the dev vendor tests
# (tests/devvendor_tests.cpp: src/devvendor/EfDevVendorLogic.h, the switch's gating) and the world editor tests
# (tests/editor_tests.cpp: src/editor/EfEditorLogic.h, the EFE requests, the rules, rates, ops and a decode fuzz) and the
# class kit tests (tests/classkit_tests.cpp: src/classkit/EfClassKitLogic.h, the self stacks' cap, gains and refunds) and
# the Sunwarden's (tests/classkit_sun_tests.cpp: src/classkit/EfClassKitSunLogic.h) and the GM command tests (tests/gmcommand_tests.cpp: src/gmcommands/EfGmCommandLogic.h, `.modify speed` and `.die`, held rates)
# and the quest loot tests (tests/questloot_tests.cpp: src/questloot/EfQuestLootLogic.h, personal quest objects' lockouts)
# and the test zone tests (tests/testzone_tests.cpp: src/testzone/EfTestZoneLogic.h, `.eftest zone`'s route and plan)
# and the meter tests (tests/meter_tests.cpp: src/meter/EfMeterLogic.h, the damage meter's fights, attribution, maths, EFM)
# and the account friend tests (tests/accountfriends_tests.cpp: src/accountfriends/EfAccountFriendsLogic.h, EFF, ADD, limits)
# and the profession XP tests (tests/professionxp_tests.cpp: src/professions/EfProfessionXp.h, colours and XP)
# and the periodic crit tests (tests/periodiccrit_tests.cpp: the core's src/server/game/Spells/Auras/EmberfallPeriodicCrit.h).
# No core, no database:
#
#   modules/mod-emberfall/tests/run_tests.sh [VECTORS.json ...]
#
# Default vectors: tests/data/talent_rules_example.json, a copy of Emberfall's
# tools/py/tests/data/talent_rules/example.json (Emberfall's test_talents.py checks that the copy and
# src/talents/TalentRules.h equal the client's). CXX (default c++) needs C++20.
#
# The test programs compile side by side (EF_BUILD_JOBS at once: Emberfall's core budget, which its Makefile exports;
# default a quarter of the cores), then run one after the other; a program that fails to compile prints its errors.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
CXXFLAGS_EF="-std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined"

# one compile (the script calls itself through xargs): --compile NAME INCLUDE_DIRS(':'-separated, below tests/) SOURCE
if [ "${1:-}" = --compile ]; then
    name=$2 incs=$3 src=$4
    set --
    old_ifs=$IFS
    IFS=:
    for d in $incs; do set -- "$@" "-I$HERE/$d"; done
    IFS=$old_ifs
    # shellcheck disable=SC2086  # the flags are words
    if ! ${CXX:-c++} $CXXFLAGS_EF "$@" "$HERE/$src" -o "$EF_TEST_OUT/$name" >"$EF_TEST_OUT/$name.log" 2>&1; then
        echo "run_tests.sh: $src does not compile:" >&2
        cat "$EF_TEST_OUT/$name.log" >&2
        exit 1
    fi
    exit 0
fi

EF_TEST_OUT=$(mktemp -d "${TMPDIR:-/tmp}/mod-emberfall-tests.XXXXXX")
export EF_TEST_OUT
trap 'rm -rf "$EF_TEST_OUT"' EXIT
JOBS=${EF_BUILD_JOBS:-$(( $(nproc 2>/dev/null || echo 4) / 4 ))}
[ "$JOBS" -ge 1 ] 2>/dev/null || JOBS=1

# name, include directories, source; run in this order
TESTS="talent ../src/talents talent_tests.cpp
companions ../src/companions companion_tests.cpp
dungeons ../src/dungeons dungeon_tests.cpp
unstuck ../src/unstuck unstuck_tests.cpp
absorbs ../src/absorbs absorb_tests.cpp
harbours ../src/harbours harbour_tests.cpp
devvendor ../src/devvendor devvendor_tests.cpp
editor ../src/talents:../src/editor editor_tests.cpp
classkit ../src/classkit classkit_tests.cpp
classkitsun ../src/classkit classkit_sun_tests.cpp
gmcommands ../src/gmcommands gmcommand_tests.cpp
questloot ../src/questloot questloot_tests.cpp
testzone ../src/testzone testzone_tests.cpp
meter ../src/meter meter_tests.cpp
accountfriends ../src/accountfriends accountfriends_tests.cpp
professionxp ../src/professions professionxp_tests.cpp
periodiccrit ../../../src/server/game/Spells/Auras periodiccrit_tests.cpp"

if ! printf '%s\n' "$TESTS" | xargs -P "$JOBS" -n 3 sh "$HERE/run_tests.sh" --compile; then
    echo "run_tests.sh: a test program did not compile" >&2
    exit 1
fi

if [ $# -eq 0 ]; then set -- "$HERE/data/talent_rules_example.json"; fi
"$EF_TEST_OUT/talent" "$@"
for name in $(printf '%s\n' "$TESTS" | cut -d' ' -f1 | tail -n +2); do
    "$EF_TEST_OUT/$name"
done
