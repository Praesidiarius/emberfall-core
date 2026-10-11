# Emberfall patches to AzerothCore

Every change this fork makes to upstream code, so that upstream merges can be checked hunk by hunk.
Each hunk is marked in the source with a comment starting `EMBERFALL:`
(`git grep -n 'EMBERFALL:'` lists them). Licence: AGPL-3.0, as upstream.

## Emberfall.OwnData (WP S2)

Lets the worldserver run on Emberfall's own DBC/map/world data, which has none of the stock
3.3.5a rows the core assumes (Emberfall `docs/content/engine-ids.md` §1, §2, §14). Switch:
`Emberfall.OwnData` in `worldserver.conf` or the environment variable `AC_EMBERFALL_OWN_DATA`
(default `0`; the emberfall-data profile sets `1`). With `0` the server behaves exactly like upstream.
What a script hook can do lives in the module `modules/mod-emberfall` (section "Module" below);
the core keeps only what runs before or outside any hook.

| # | What | Where | With `Emberfall.OwnData = 1` |
|---|---|---|---|
| 0 | The switch: `Emberfall::OwnData()` reads the key through `sConfigMgr` (no `WorldConfig` enum entry, no `worldserver.conf.dist` block: both are upstream hot spots) | `src/server/game/World/EmberfallOwnData.h` (new file), included by the files below | — |
| 1 | DBC version sentinels (AreaTable 4987, CharTitles 177, GemProperties 1629, Item 56806, ItemExtendedCost 2997, Map 724, Spell 80864); runs inside `LoadDBCStores`, before any hook | `src/server/game/DataStores/DBCStores.cpp:653` | not checked (logged once) |
| 2 | Start-area map files: eight stock coordinates on maps 0/1/530, an `exit(1)` early in `SetInitialWorldSettings` | `src/server/game/World/World.cpp:332` | skipped; the module checks every `playercreateinfo` position instead (a missing `.map` file is a WARN, not an exit) |
| 3 | `SpellInfoCorrections`: ERROR per missing stock spell | `src/server/game/Spells/SpellInfoCorrections.cpp:32` | a missing spell is skipped silently (fixes still apply to the stock IDs we author, e.g. 3286, 15007) |
| 4 | `SpellInfoCorrections` tail: unchecked writes into stock SummonProperties, CreatureDisplayInfo, Faction(Template), VehicleSeat, Achievement, Lock rows and `game_graveyard` 1364/1365 (segfault) | `SpellInfoCorrections.cpp:5380` | the tail is skipped (it only patches stock rows) |
| 5 | *(moved to the module, then config)* Wintergrasp setup creates map 571 and ASSERTs | — | the profile sets `Wintergrasp.Enable = 2`; the module forces 2 under `Emberfall.OwnData` |
| 6 | *(removed: data)* `outdoorpvp_template`: every type is a stock zone and ASSERTs on its AreaTable row | — | Emberfall's clean world has an empty `outdoorpvp_template`, so `InitOutdoorPvP` returns at its start (one WARN "Loaded 0 outdoor PvP definitions") |
| 7 | *(moved to the module)* `MSG_SET_RAID_DIFFICULTY` creates maps 571 and 0 and ASSERTs (client-triggerable by any group leader) | — | see "Module" |
| 10 | *(moved to the module)* Spirit healer confirm without the stock spell script (`SCRIPTS=none`, Emberfall WP D4) | — | see "Module" |

## Unconditional fixes (no behaviour change with valid stock data)

| # | What | Where | Change |
|---|---|---|---|
| 8 | `GameEventMgr::StartEvent`/`StopEvent` index the event vector (sized by `MAX(eventEntry)` of `game_event`) without a bounds check; `WorldState::Load` starts the stock events 101 and 116 at every boot (out-of-bounds write with an empty `game_event`) | `src/server/game/Events/GameEventMgr.cpp:139`, `:204` | an unknown event ID returns (debug log) |
| 9 | `PlayerInfo::displayId_m/_f` were `uint16`, so a ChrRaces player display ID above 65535 was truncated and the next position update ASSERTed in `Unit::GetCollisionHeight` (`sCreatureDisplayInfoStore.AssertEntry`). Emberfall's display IDs start at 90001 | `src/server/game/Entities/Player/Player.h:332` | `uint32` |

## Module (`modules/mod-emberfall`)

A standard AzerothCore module (AzerothCore's `modules/CMakeLists.txt` builds every
`modules/<name>/src`; the loader is `Addmod_emberfallScripts()`). `SCRIPTS=none` does not affect
modules; the Dockerfile's `CMODULES=static` default links it into the worldserver. New files only, so
it never conflicts on a rebase. `/modules/*` is in upstream's `.gitignore`: add new module files with
`git add -f`. Every script does nothing unless `Emberfall.OwnData = 1`.

| Was | Script (hook) | With `Emberfall.OwnData = 1` |
|---|---|---|
| #10 (`PlayerGossip.cpp`) | `emberfall_spirit_healer_confirm` (`AllCreatureScript::CanCreatureGossipSelect`) | a dead player's spirit healer option (`GOSSIP_OPTION_SPIRITHEALER`) sends `SMSG_SPIRIT_HEALER_CONFIRM`; the core still casts 17251 (its visual). The stock spell script on 17251 does this in a full build; `SCRIPTS=none` has none |
| #2, second half (`World.cpp`) | `emberfall_own_data_world` (`WorldScript::OnBeforeWorldInitialized`) | WARN `Emberfall.OwnData: no map file for the start position of race R class C ...` per `playercreateinfo` position without a `.map` file (Emberfall's log check forbids it once the server has our maps) |
| #5 (`BattlefieldMgr.cpp`) | `emberfall_own_data_world` (`WorldScript::OnAfterConfigLoad`) | `Wintergrasp.Enable` forced to 2 (a WARN when it was not) |
| #7 (`MiscHandler.cpp`) | `emberfall_own_data_server` (`ServerScript::CanPacketReceive`) | when Map.dbc lacks map 571 or 0: a group leader's raid difficulty change is refused (`SendRaidDifficulty`) instead of the core creating both maps and asserting. With Emberfall's placeholder rows (`dbc_placeholders.sql`, loaded by default) both maps exist and the stock handler runs, as with the old patch |

The spec and talent system (Emberfall talents.md §3; switch `Emberfall.Talents.Enable`, default 0,
independent of `Emberfall.OwnData`) is module code only, on hooks that exist upstream: `OnPlayerCanUseChat`
(the addon-whisper protocol), `OnPlayerLogin`, `OnPlayerLevelChanged`, `OnPlayerCalculateTalentsPoints`,
`OnPlayerCanLearnTalent`, `OnPlayerDeleteFromDB`, `WorldScript` loading and a `CommandScript`; its tables come
from the module's `data/sql/db-world` and `db-characters` (applied by dbimport). No core hunk. See
`modules/mod-emberfall/README.md`.

Why the rest stays in the core: #1, #3 and #4 run inside `LoadDBCStores` / `LoadSpellInfoCorrections`
and #2's skip at the start of `SetInitialWorldSettings`, all before the first hook a module gets
(`OnLoadCustomDatabaseTable` comes before the DBCs, the next one after them is far later); #8 is
reached from `WorldState::Load` and `DisableMgr` with no hook in between; #9 is a struct field type.

## Build and deployment

| What | Where |
|---|---|
| The docker build passes `CMAKE_EXTRA_OPTIONS` to CMake (the arg was declared but unused), so the profile's worldserver can be built with `SCRIPTS=none` + `SCRIPTS_COMMANDS=static` (no unbound stock content scripts) | `apps/docker/Dockerfile:105` |
| The `emberfall-data` server profile: own compose project, containers `ef-data-*`, MySQL 127.0.0.1:3320, authserver 3725, worldserver 8086, data `var/serverdata-ef`, config/logs `var/emberfall-data`, images `emberfall/ac-wotlk-*:own-data` | `docker-compose.emberfall-data.yml`, `apps/docker/emberfall-data/initdb/01-world-schema.sql` |
| The `emberfall-data` profile runs with vmaps and mmaps (Emberfall WP X2): `MoveMaps.Enable`, `vmap.enableLOS`, `vmap.enableHeight`, `vmap.enableIndoorCheck` and `vmap.petLOS` = 1 (they were 0 until Emberfall extracted its own vmaps/mmaps with `tools/server/extract.sh --full`). No code change | `docker-compose.emberfall-data.yml` (`AC_MOVE_MAPS_ENABLE`, `AC_VMAP_*`) |
| The profile's compose file takes `EF_PREFIX` (container names), `EF_IMAGE_TAG`, `EF_VAR_DIR` and `EF_SERVERDATA_DIR` (defaults: the values above), so Emberfall's `tools/server/upstream_check.sh` can run a throwaway stack (`ef-check-*`, compose project `ef-check`) from the same file | `docker-compose.emberfall-data.yml` |
| The profile's compose file also takes `EF_WORLD_DB`, `EF_DBIMPORT_DATABASES`, `EF_OWN_DATA`, `EF_TALENTS_ENABLE` and `EF_MOVE_MAPS` (defaults: the emberfall-data values), so Emberfall's `ef_server.sh` can run the "stock-test" profile (`ef-stock-*`, MySQL 3340, auth 3727, world 8088, stock world DB and the stock `var/serverdata` read-only, images `:stock-test` with the full stock scripts) from the same file | `docker-compose.emberfall-data.yml` |
| The stock dev server's local overrides (host networking, local data, Warden off) | `docker-compose.override.yml` |

The profile is driven from the Emberfall repository: `tools/server/ef_server.sh` (build, up, data,
world, restart, check, account) and `tools/server/make_clean_world.sh` (the clean world DB).

## Fallback without the patches

Emberfall's `make content-server` also writes `dist/server/sql/dbc_placeholders.sql` (WP D2): stock-ID
rows for the `*_dbc` override tables, `game_graveyard` 1364/1365 and `disables` for events 101/116,
covering #1, #4, #7 and #8 for a stock binary. #2 (start-area map files) and #9 (display ID
truncation) have no data-only workaround except stub `.map` files and player display IDs ≤ 65535.
