# Emberfall patches to AzerothCore

Every change this fork makes to upstream code, so that upstream merges can be checked hunk by hunk.
Each hunk is marked in the source with a comment starting `EMBERFALL:`
(`git grep -n 'EMBERFALL:'` lists them). Licence: AGPL-3.0, as upstream.

## Emberfall.OwnData (WP S2)

Lets the worldserver run on Emberfall's own DBC/map/world data, which has none of the stock
3.3.5a rows the core assumes (Emberfall `docs/content/engine-ids.md` §1, §2, §14). Switch:
`Emberfall.OwnData` in `worldserver.conf` (default `0`, env `AC_EMBERFALL_OWN_DATA`). With `0`
the server behaves exactly like upstream.

| # | What | Where | With `Emberfall.OwnData = 1` |
|---|---|---|---|
| 0 | The switch | `src/server/game/World/WorldConfig.h:522` (`CONFIG_EMBERFALL_OWN_DATA`), `WorldConfig.cpp:367`, `src/server/apps/worldserver/worldserver.conf.dist:4955` | — |
| 1 | DBC version sentinels (AreaTable 4987, CharTitles 177, GemProperties 1629, Item 56806, ItemExtendedCost 2997, Map 724, Spell 80864) | `src/server/game/DataStores/DBCStores.cpp:652` | not checked (logged once) |
| 2 | Start-area map files: eight stock coordinates on maps 0/1/530 | `src/server/game/World/World.cpp:331` | skipped; instead, after `LoadPlayerInfo`, every `playercreateinfo` position is checked and a missing `.map` file is a WARN, not an exit (`World.cpp:705`) |
| 3 | `SpellInfoCorrections`: ERROR per missing stock spell | `src/server/game/Spells/SpellInfoCorrections.cpp:32` | a missing spell is skipped silently (fixes still apply to the stock IDs we author, e.g. 3286, 15007) |
| 4 | `SpellInfoCorrections` tail: unchecked writes into stock SummonProperties, CreatureDisplayInfo, Faction(Template), VehicleSeat, Achievement, Lock rows and `game_graveyard` 1364/1365 (segfault) | `SpellInfoCorrections.cpp:5380` | the tail is skipped (it only patches stock rows) |
| 5 | Wintergrasp setup creates map 571 and ASSERTs | `src/server/game/Battlefield/BattlefieldMgr.cpp:48` | not set up when Map.dbc has no 571 (the profile also sets `Wintergrasp.Enable = 2`) |
| 6 | `outdoorpvp_template`: every type is a stock zone and ASSERTs on its AreaTable row | `src/server/game/OutdoorPvP/OutdoorPvPMgr.cpp:47` | outdoor PvP is not loaded |
| 7 | `CMSG_SET_RAID_DIFFICULTY` creates maps 571 and 0 and ASSERTs (client-triggerable by any group leader) | `src/server/game/Handlers/MiscHandler.cpp:1393` | when Map.dbc lacks 571 or 0: the home maps are not created; the change is refused if a member is inside a shared-difficulty raid map (the only case that needs them) |
| 10 | Spirit healer: the gossip option casts spell 17251, and only the stock spell script on it (`spell_gen_spirit_healer_res`) sends `SMSG_SPIRIT_HEALER_CONFIRM`; a worldserver built with `SCRIPTS=none` never offers the resurrection (Emberfall WP D4) | `src/server/game/Entities/Player/PlayerGossip.cpp:325` | the gossip option sends the confirm itself (the cast stays, for its visual) |

## Unconditional fixes (no behaviour change with valid stock data)

| # | What | Where | Change |
|---|---|---|---|
| 8 | `GameEventMgr::StartEvent`/`StopEvent` index the event vector (sized by `MAX(eventEntry)` of `game_event`) without a bounds check; `WorldState::Load` starts the stock events 101 and 116 at every boot (out-of-bounds write with an empty `game_event`) | `src/server/game/Events/GameEventMgr.cpp:139`, `:204` | an unknown event ID returns (debug log) |
| 9 | `PlayerInfo::displayId_m/_f` were `uint16`, so a ChrRaces player display ID above 65535 was truncated and the next position update ASSERTed in `Unit::GetCollisionHeight` (`sCreatureDisplayInfoStore.AssertEntry`). Emberfall's display IDs start at 90001 | `src/server/game/Entities/Player/Player.h:332` | `uint32` |

## Build and deployment

| What | Where |
|---|---|
| The docker build passes `CMAKE_EXTRA_OPTIONS` to CMake (the arg was declared but unused), so the profile's worldserver can be built with `SCRIPTS=none` + `SCRIPTS_COMMANDS=static` (no unbound stock content scripts) | `apps/docker/Dockerfile:105` |
| The `emberfall-data` server profile: own compose project, containers `ef-data-*`, MySQL 127.0.0.1:3320, authserver 3725, worldserver 8086, data `var/serverdata-ef`, config/logs `var/emberfall-data`, images `emberfall/ac-wotlk-*:own-data` | `docker-compose.emberfall-data.yml`, `apps/docker/emberfall-data/initdb/01-world-schema.sql` |
| The stock dev server's local overrides (host networking, local data, Warden off) | `docker-compose.override.yml` |

The profile is driven from the Emberfall repository: `tools/server/ef_server.sh` (build, up, data,
world, restart, check, account) and `tools/server/make_clean_world.sh` (the clean world DB).

## Fallback without the patches

Emberfall's `make content-server` also writes `dist/server/sql/dbc_placeholders.sql` (WP D2): stock-ID
rows for the `*_dbc` override tables, `game_graveyard` 1364/1365 and `disables` for events 101/116,
covering #1, #4, #7 and #8 for a stock binary. #2 (start-area map files) and #9 (display ID
truncation) have no data-only workaround except stub `.map` files and player display IDs ≤ 65535.
