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
| 4 | `SpellInfoCorrections` tail: unchecked writes into stock SummonProperties, CreatureDisplayInfo, Faction(Template), VehicleSeat, Achievement, Lock rows and `game_graveyard` 1364/1365 (segfault) | `SpellInfoCorrections.cpp:5355` | the tail is skipped (it only patches stock rows) |
| 5 | *(moved to the module, then config)* Wintergrasp setup creates map 571 and ASSERTs | — | the profile sets `Wintergrasp.Enable = 2`; the module forces 2 under `Emberfall.OwnData` |
| 6 | *(removed: data)* `outdoorpvp_template`: every type is a stock zone and ASSERTs on its AreaTable row | — | Emberfall's clean world has an empty `outdoorpvp_template`, so `InitOutdoorPvP` returns at its start (one WARN "Loaded 0 outdoor PvP definitions") |
| 7 | *(moved to the module)* `MSG_SET_RAID_DIFFICULTY` creates maps 571 and 0 and ASSERTs (client-triggerable by any group leader) | — | see "Module" |
| 10 | *(moved to the module)* Spirit healer confirm without the stock spell script (`SCRIPTS=none`, Emberfall WP D4) | — | see "Module" |
| 13 | Profession ranks (Emberfall `docs/design/professions.md`, the rank system): `LoadSpellLearnSkills` sets a `skill` effect's maximum to step x 75, and casting a profession spell (Mining, Herbalism and Skinning are their own gather spells) sets it again in `EffectLearnSkill`, so Sundered Vale's ranks 75 / 175 / 325 / 500 cannot be had from data | `src/server/game/Spells/SpellMgr.cpp` (`EmberfallSkillTierMax`, `LoadSpellLearnSkills`), `SpellEffects.cpp` (`EffectLearnSkill`); the unconditional half in `Player.cpp` (`_LoadSkills`) | the maximum of rank `step` is `SkillTiers.Value[step - 1]` of the skill's SkillRaceClassInfo tier (the first row of the skill with a tier; step x 75 when there is none or it is 0), and the gather cast keeps it. Unconditional: `_LoadSkills` found a ranked skill's step by comparing `Value[0]` on every pass of its loop instead of `Value[i]`, so a skill of rank 2 or more loaded with step 0 (now the rank whose maximum it is) |
| 14 | Gathering colours (Sundered Vale `professions.md` §4): `Player::UpdateGatherSkill` hardcodes yellow +25, green +50, grey +100 over the requirement | `src/server/game/Entities/Player/PlayerUpdates.cpp` (`UpdateGatherSkill`; upstream's switch moved unchanged into the file-local `UpdateGatherSkillRoll`; `EmberfallOrangeBonus`) | yellow +40, green +80, grey +120, also handed to `OnPlayerUpdateGatheringSkill`; the chances stay the `SkillChance.*` settings (100/75/25/0); a point gained with the requirement within 10 of the skill rolls a second at 25 % (x the elite multiplier). Mining's and Skinning's steps options are kept |
| 15 | Crafting colours: upstream's `CraftSkillGainChance` falls linearly from orange at yellow to grey | `PlayerUpdates.cpp` (`UpdateCraftSkill`) | Sundered Vale's steps: orange below `TrivialSkillLineRankLow`, yellow to the midpoint, green to `TrivialSkillLineRankHigh`, grey from it (`SkillGainChance`, 100/75/25/0 by the settings; a row whose high is not above its low keeps the upstream rule), and the same orange second point within 10 of `MinSkillLineRank`. Discovery and the hook unchanged |

## Unconditional fixes (no behaviour change with valid stock data)

| # | What | Where | Change |
|---|---|---|---|
| 8 | `GameEventMgr::StartEvent`/`StopEvent` index the event vector (sized by `MAX(eventEntry)` of `game_event`) without a bounds check; `WorldState::Load` starts the stock events 101 and 116 at every boot (out-of-bounds write with an empty `game_event`) | `src/server/game/Events/GameEventMgr.cpp:139`, `:204` | an unknown event ID returns (debug log) |
| 9 | `PlayerInfo::displayId_m/_f` were `uint16`, so a ChrRaces player display ID above 65535 was truncated and the next position update ASSERTed in `Unit::GetCollisionHeight` (`sCreatureDisplayInfoStore.AssertEntry`). Emberfall's display IDs start at 90001 | `src/server/game/Entities/Player/Player.h:332` | `uint32` |
| 12 | The combat log tap (Emberfall `docs/design/damage-meter.md` §1): every combat log site hands its event to one sink a module sets (`Emberfall::CombatLog::SetSink`; mod-emberfall's damage meter), before any range limit; shield absorbs from `CalcAbsorbResist` with the shield's caster. No sink (the default, or `Emberfall.Meter.Enable = 0`): one relaxed atomic load per site | `src/server/game/Entities/Unit/EmberfallCombatLog.h` (new), `Unit.cpp` (`SendSpellNonMeleeDamageLog`, `SendPeriodicAuraLog`, `SendAttackStateUpdate`, `DealDamageShieldDamage`, `CalcAbsorbResist` via `EmberfallLogAbsorb`, `SendHealSpellLog`), `Object.cpp` (`SendSpellMiss`), `Spell.cpp` (`ExecuteLogEffectInterruptCast`), `SpellEffects.cpp` (`EffectDispel`, `EffectStealBeneficialBuff`), `Player.cpp` (`EnvironmentalDamage`) | nothing without a sink; the packets are unchanged |
| 11 | Transports (Emberfall `docs/design/harbours-and-ships.md`): `TransportMgr::LoadTransportTemplates` handed any path to `GeneratePath`, which crashes the worldserver at boot on an empty path, a gap in the node indices (null entries), a teleport at the first node, a speed or acceleration of 0, or a node on a map Map.dbc lacks; `LoadDBCStores` indexed its path table by a TaxiPathNode's path unchecked (out of bounds when TaxiPath.dbc ends before it) | `src/server/game/Maps/TransportMgr.cpp` (`EmberfallTransportPathProblem`, `LoadTransportTemplates`), `src/server/game/DataStores/DBCStores.cpp` (the path lengths and nodes) | the transport, or the node, is skipped with an error (stock data never hits them) |

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
| — (new) | `npc_ef_harbourmaster` (`CreatureScript` gossip; `src/emberfall_harbours.cpp`) | the harbourmasters of Emberfall's harbours (docs/design/harbours-and-ships.md) tell the next sailing of the ships that berth at their pier, from the live MO transport's path progress; any data |
| — (new) | `emberfall_personal_quest_object`, `emberfall_personal_quest_object_stays` (`GameObjectScript` AI), `emberfall_questloot_player` (`OnPlayerBeforeSendLoot`, `OnPlayerLootItem`; `src/emberfall_questloot.cpp`) | Emberfall's personal quest objects (docs/design/group-quest-loot.md; bound by `gameobject_template.ScriptName`, which only Emberfall's world sets): a quest chest no longer despawns for the group when one player empties it; each player takes their own item and only they lose sight of the object until their own respawn |
| — (new) | `emberfall_professions_world` (`OnAfterConfigLoad`), `emberfall_professions_player` (`OnPlayerUpdateGatheringSkill`, `OnPlayerUpdateCraftingSkill`; `src/emberfall_professions.cpp`, `src/professions/EfProfessionXp.h`) | character XP for every gather and craft of a primary profession by its colour (Emberfall `docs/design/professions.md`): orange and yellow `Base + PerSkill x requirement`, green half, grey nothing; `Emberfall.Professions.Xp.Enable` (1), `.Base` (70), `.PerSkill` (0.06), `.GreenPercent` (50) |

The spec and talent system (Emberfall talents.md §3; switch `Emberfall.Talents.Enable`, default 0,
independent of `Emberfall.OwnData`) is module code only, on hooks that exist upstream: `OnPlayerCanUseChat`
(the addon-whisper protocol), `OnPlayerLogin`, `OnPlayerLevelChanged`, `OnPlayerCalculateTalentsPoints`,
`OnPlayerCanLearnTalent`, `OnPlayerDeleteFromDB`, `WorldScript` loading and a `CommandScript`; its tables come
from the module's `data/sql/db-world` and `db-characters` (applied by dbimport). No core hunk. See
`modules/mod-emberfall/README.md`.

The companions (Emberfall `docs/design/companions.md`: `.efparty`, AI party members for testing; switch
`Emberfall.Companions.Enable`, default 1) are module code and data only: guardians summoned with a stock SummonProperties
row, a `CreatureScript` AI, `PlayerScript` hooks (`OnPlayerUpdate`, `OnPlayerLogout`, `OnPlayerMapChanged`,
`OnPlayerLevelChanged`, `OnPlayerJustDied`), a `CommandScript`, and the module's `ef_companion_*` tables and creature rows
(990001-990005, stock world only). No core hunk. See `modules/mod-emberfall/README.md`.

Why the rest stays in the core: #1, #3 and #4 run inside `LoadDBCStores` / `LoadSpellInfoCorrections`
and #2's skip at the start of `SetInitialWorldSettings`, all before the first hook a module gets
(`OnLoadCustomDatabaseTable` comes before the DBCs, the next one after them is far later); #8 is
reached from `WorldState::Load` and `DisableMgr` with no hook in between; #9 is a struct field type.

## Build and deployment

| What | Where |
|---|---|
| The docker build passes `CMAKE_EXTRA_OPTIONS` to CMake (the arg was declared but unused), so the profile's worldserver can be built with `SCRIPTS=none` + `SCRIPTS_COMMANDS=static` (no unbound stock content scripts) | `apps/docker/Dockerfile:105` |
| The docker build's compile jobs come from the build argument `CBUILD_JOBS` (empty: `nproc + 1`, the stock value), so Emberfall's `ef_server.sh build` keeps to its core budget (`EF_BUILD_CORES`, a quarter of the cores by default) instead of taking every core | `apps/docker/Dockerfile` (`ARG CBUILD_JOBS`, the `cmake --build` line) |
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
