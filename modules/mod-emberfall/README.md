# mod-emberfall

Emberfall's AzerothCore module: the parts of the fork's changes that AzerothCore's script hooks can
carry, so they need no core patch (PATCHES.md, section "Module"). Standard module layout: AzerothCore's
`modules/CMakeLists.txt` compiles every `src/*.cpp` of every `modules/<name>/src` and calls the loader
`Add<name with - as _>Scripts()`, here `Addmod_emberfallScripts()` (`src/mod_emberfall_loader.cpp`).

Modules are independent of the stock scripts: the image's `SCRIPTS=none` (no stock content scripts)
does not touch them; `MODULES=static` (the Dockerfile's `CMODULES` default) links them into the
worldserver. The boot log lists the module under "Using modules configuration" only when it has a
config file; this one has none (see below), so look for `mod-emberfall` in CMake's
"Modules configuration" output, or for the scripts' effects.

| Script | Hook | What (with `Emberfall.OwnData = 1`) | Was |
|---|---|---|---|
| `emberfall_spirit_healer_confirm` | `AllCreatureScript::CanCreatureGossipSelect` | a dead player's spirit healer option (`GOSSIP_OPTION_SPIRITHEALER`) sends `SMSG_SPIRIT_HEALER_CONFIRM` (the stock spell script on 17251 does it in a full build); the core still casts 17251 | core patch #10 (`PlayerGossip.cpp`) |
| `emberfall_own_data_world` | `WorldScript::OnBeforeWorldInitialized` | every `playercreateinfo` position must be on a `.map` file: a WARN `Emberfall.OwnData: no map file for the start position ...` (the core skips the stock start-area check, patch #2) | core patch #2, second half (`World.cpp`) |
| `emberfall_own_data_world` | `WorldScript::OnAfterConfigLoad` | `Wintergrasp.Enable` forced to 2 (no Wintergrasp in our data; the profile sets 2 anyway) | core patch #5 (`BattlefieldMgr.cpp`) |
| `emberfall_own_data_server` | `ServerScript::CanPacketReceive` | `MSG_SET_RAID_DIFFICULTY` from a group leader is refused when Map.dbc lacks map 571 or 0 (the core would create both and assert); with Emberfall's placeholder rows (the default) nothing changes | core patch #7 (`MiscHandler.cpp`) |

## Specs and talents (Emberfall `docs/design/talents.md` §3, WPs T3, M1-M6, T6)

The spec and talent system: Emberfall's own class trees, and generated trees for stock classes on the
stock-test profile. Switch `Emberfall.Talents.Enable` (default 0; `AC_EMBERFALL_TALENTS_ENABLE`, read
through `sConfigMgr` like `Emberfall.OwnData` and independent of it; both Emberfall compose profiles set 1).
For a class the system is active only when `ef_spec` has rows for it; every other class keeps the stock
talent system untouched.

| File | What |
|---|---|
| `data/sql/db-world/mod_emberfall_2026_10_11_00_talents.sql` | the ten `ef_*` tree tables (schema only; Emberfall's compiler writes the rows: `world.sql`, or `dist/test-talents/talents.sql` for the generated stock trees) and the help texts of the GM commands |
| `data/sql/db-world/mod_emberfall_2026_10_11_01_talents_help.sql` | the help texts of `.ef`, `.ef talents` and every subcommand as of M4/M5 (`.help ef ...`) |
| `data/sql/db-characters/mod_emberfall_2026_10_11_00_talents.sql` | `ef_character_spec`, `ef_character_loadout`, `ef_character_actions`, `ef_character_granted` |
| `data/sql/db-characters/mod_emberfall_2026_10_12_00_talents_replaced.sql` | M6: `ef_character_granted.source` 3 (a core-taught spell a replacing node took; the column comment only) |
| `src/talents/TalentRules.h` | a **verbatim copy** of Emberfall's `client/src/data/TalentRules.h` (Apache-2.0): the rules of the trees; client preview and server validation run the same JSON vectors |
| `src/talents/EftProtocol.h` | the `EFT` addon-message protocol as plain functions: parse, chunking both ways, names, stored `nodeID:rank` pairs, reassembly, rate limit |
| `src/talents/EfTalentModel.h` | the loaded trees and the pure functions over them: budgets, wire strings <-> pairs, re-mapping after a data change, the expected spell set (without what invested replacing nodes replace), a loadout's replacing nodes (`Replacements`: replaced, by, the node's spells), the structural checks, the canonical JSON of the data hash |
| `src/talents/EfLoadouts.h` | the pure parts of saved loadouts and per-spec bars: new loadout IDs and the limit, a spec's first bars |
| `src/talents/EfTalents.{h,cpp}` | loading (`ef_*` -> model; refuses inconsistent data, e.g. a hash that does not match the rows), per-character state (on the `Player`'s `CustomData`), the protocol handlers, validation, the reconcile, persistence |
| `src/emberfall_talents.cpp` | the hooks and the GM commands |
| `tests/` | `run_tests.sh`: a standalone build (no core) of the rules vectors (`tests/data/talent_rules_example.json`, a copy of Emberfall's), the model, the protocol, the loadout and bar helpers and a decode fuzz |

| Script | Hook | What |
|---|---|---|
| `emberfall_talents_world` | `WorldScript::OnAfterConfigLoad`, `OnBeforeWorldInitialized` | reads the switch (a `.reload config` that flips it loads or drops the trees); loads and checks the `ef_*` tables (and that the switch spell exists); the boot log lists the trees (`>> Emberfall talents: class 1: class tree 901 ...`) |
| `emberfall_talents_player` | `OnPlayerLogin` | loads the character's state; the first time under the system resets its stock talents in every talent group and drops a second group (§3.5, flag bit 0); re-validates every loadout (data hash, level; resets what fails, `RESET` notices for the next `HELLO`) and reconciles the spells |
| | `OnPlayerLevelChanged` | re-validates (level loss), reconciles (baseline spells), pushes `STATE` |
| | `OnPlayerCalculateTalentsPoints`, `OnPlayerCanLearnTalent` | 0 stock talent points and no stock talent learning for a class with trees |
| | `OnPlayerCanUseChat` (whisper) | consumes every `EFT\t` addon whisper; answers the player's own (to itself) |
| | `OnPlayerUpdate` | finishes a switch whose cast went off (the swap: bars, spells, bars) or answers `ERR` when the cast ended otherwise; a cheap no-op while no switch runs |
| | `OnPlayerEnterCombat` | interrupts a running switch cast (`ERR COMBAT`) |
| | `OnPlayerDeleteFromDB` | deletes the `ef_character_*` rows in the deletion transaction |
| `emberfall_talents_spells` | `AllSpellScript::OnSpellCast` | the switch spell went off (no `spell_script_names` row needed) |
| `emberfall_talents_commands` | `CommandScript` | `.ef talents dump` (state, loadouts, granted spells, talent auras, stored bars, a running switch), `.ef talents reset`, `.ef talents reload` (admin), `.ef spec ID` (instant switch with bars, no cast), `.ef send BODY` (a body as if sent by the GM's own client) |

Protocol (`EFT\t<ver>;<seq>;<cmd>;...`, talents.md §4; the answers per request are its §4.4 table): `HELLO` ->
`STATE`, every `LOADOUT`, pending `RESET`s, `OK`; `SPEC` (the first choice at once; a switch casts the switch
spell: `SWITCH`, and at the cast's end `STATE`, `LOADOUT`, `OK` or `ERR COMBAT`/`STATE`); `APPLY` (chunked,
validated with `TalentRules.h` after the protocol, hash and player-state checks, then saved and reconciled:
`STATE`, `LOADOUT`, `OK`); `RESET` (class/spec/both trees of the active loadout); `SAVE` (validated, stored;
`LIMIT` past 5 per spec), `LOAD` (an apply for the active spec, a mark for another), `DEL` (`GONE`). Limits: 20
requests per 10 s (one `THROTTLE` answer per window), `SPEC` once per 2 s, one reassembly of at most 8 x 251
bytes.

Per-spec action bars (§3.6): the active spec's bars are the core's (`character_action`); at a switch the old
spec's go to `ef_character_actions` and the new spec's come back (the first time: the current bars without
spells it does not know, plus its baseline spells in the first bar's empty buttons); `SMSG_ACTION_BUTTONS` 2
then 1.

Spells: learned with `Player::learnSpell` (saved in `character_spell`), removed with `removeSpell` plus the
auras they own or triggered; `ef_character_granted` records exactly what the module taught, so a spell the core
or a GM taught is never taken. Stock talent spells (generated trees only) leave a no-talent-group entry behind
`removeSpell` that the core's rank logic would revive; the reconcile drops those (`DropLeftover`).

Replacing nodes (WP M6, talents.md §1.4, `ef_talent_node.replaces`): while a node of the active loadout that
replaces spell X is invested, X is not taught (a baseline spell) or is removed (a spell the core taught: a trainer or
skill-line spell; recorded in `ef_character_granted` with `source` 3 and learned back when the node goes), and every
action button holding X (or another rank or option of the node) holds the node's current ability (its first
non-passive spell); refunded, or in a spec without the node, the buttons holding one of the node's spells go back to
X. The rewrite follows every reconcile (apply, load, reset, login, level change; `SMSG_ACTION_BUTTONS` 1 when a
button changed) and entering a spec (after its bars are loaded). `.ef talents dump` lists the swaps
(`replacing: X->by`, `by` 0 when the node is not invested) and the core spells taken.

Free nodes (`ef_talent_free`, talents.md §8 Decision 6) are full for their spec at no cost and never stored: the
validation treats them as full whatever the client sends, the stored pairs leave them out, and their spells are
taught like an invested node's.

The switch `Emberfall.OwnData` (default 0) is read through `src/server/game/World/EmberfallOwnData.h`
(`Emberfall::OwnData()`, shared with the remaining core hunks) from `worldserver.conf` or the
environment variable `AC_EMBERFALL_OWN_DATA` (the emberfall-data profile sets it). The module ships
no `conf/*.conf.dist`: AzerothCore loads a module's config file as required, and the docker
entrypoint copies only the `.dist`, so a missing `mod_emberfall.conf` would be logged at boot ("Failed open file", `Config.Policy` missing-file severity).
