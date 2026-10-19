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

## Class kits (Emberfall `docs/design/class-kit-pipeline.md`)

Emberfall's own class abilities on a world whose classes would otherwise keep their stock kits (the ef-classes profile:
the stock world with Emberfall's kits). The kits are data (Emberfall's `python -m emberfall_content.classkit build`
writes `spell_dbc`, the trainers' lists, the start skill and buttons, the specs and trees for the talent system above,
and this module's two tables); the module adds what data cannot. Switch `Emberfall.ClassKit.Enable` (default 0;
`AC_EMBERFALL_CLASS_KIT_ENABLE`, the compose file's `EF_CLASS_KIT_ENABLE`); a class without an `ef_class_kit` row is
untouched.

| File | What |
|---|---|
| `data/sql/db-world/mod_emberfall_2026_10_12_10_classkit.sql` | `ef_class_kit` (class, skill line, the stock family it replaces), `ef_class_kit_spell` (class, spell, level, source); help texts of `.efkit` |
| `src/classkit/EfClassKit.{h,cpp}` | loading and checks (every spell exists, none in the stock family, the skill line exists; boot line `>> Emberfall class kits: class 1: ...`), the purge, the dump |
| `src/emberfall_classkit.cpp` | the hooks, `.efkit` / `.efkit purge`, the kits' spell scripts |

| Script | Hook | What |
|---|---|---|
| `emberfall_classkit_world` | `WorldScript::OnAfterConfigLoad`, `OnBeforeWorldInitialized` | reads the switch, loads the kits |
| `emberfall_classkit_player` | `OnPlayerLogin`, `OnPlayerLevelChanged` | for a kit class: takes away the class's own stock class skill lines (a SkillRaceClassInfo row naming the class alone), spells and auras of the stock family, buttons of spells the character does not know; logs what it took |
| `emberfall_classkit_commands` | `CommandScript` | `.efkit` (the kit and what the selected player knows of it), `.efkit purge` |
| `spell_ef_kit_add_stacks`, `spell_ef_kit_shatter`, `spell_ef_kit_absorb_pct_health`, `spell_ef_kit_line_targets`, `spell_ef_kit_interrupt_followup`, `spell_ef_kit_rest_regen` | `SpellScript`/`AuraScript` (`spell_script_names` from the kit) | the kits' generic mechanics, each reading its numbers from the spell's own effects (contracts at the top of `src/emberfall_classkit.cpp`): stacks with thresholds, shatter + one capped shell + mana refund, an absorb in percent of max health, line targets, an interrupt follow-up, out-of-combat mana |

On Emberfall's own world (the emberfall-data profile, `class-kit-pipeline.md` §8) the same tables come from `world.sql`
and the spells from Emberfall's own Spell.dbc; the profile sets the switch to 1.

## Profession specialisations (Emberfall `docs/design/professions.md` §10)

Three trees per profession (Mining, Herbalism, Alchemy), points from profession skill (one per 10), on the talent
system's rules (`src/talents/TalentRules.h`, included, unchanged) with one budget per profession and "a stored rank never
goes down" (a whole tree is reset at the profession trainer for coin). On when the `ef_prof_*` world tables have rows;
switch `Emberfall.ProfSpecs.Enable` (default 1, `AC_EMBERFALL_PROFSPECS_ENABLE`). Independent of the talent system.

| File | What |
|---|---|
| `data/sql/db-world/mod_emberfall_2026_10_18_00_profspecs.sql` | the eight `ef_prof_*` tables (schema; Emberfall's compiler writes the rows into `world.sql`) and the help texts of `.efprof` |
| `data/sql/db-characters/mod_emberfall_2026_10_18_00_profspecs.sql` | `ef_character_prof_rank` (guid, node, rank), `ef_character_prof_granted` (the rank spells the module taught) |
| `src/profspecs/EfProfModel.h` | the loaded trees and the pure functions: the base-36 wire string, validation (TalentRules per tree, the budget, no lower rank), the expected rank spells, effect sums, resets, remapping after a data change (`tests/profspec_tests.cpp`, `tests/run_profspec_tests.sh`) |
| `src/profspecs/EfProfSpecs.{h,cpp}` | loading, per-character state, the `EFP` protocol (HELLO, APPLY -> STATE, GONE, RESET, NODES, OK/ERR), the rank spells, the effects, the trainer's reset, the GM dump |
| `src/emberfall_profspecs.cpp` | the hooks and `.efprof dump|reset [skill]|reload|send BODY` |

| Script | Hook | What |
|---|---|---|
| `emberfall_profspecs_world` | `OnAfterConfigLoad`, `OnBeforeWorldInitialized` | the switch; loads and checks the tables (every rank spell in Spell.dbc), `skill_discovery_template`; boot line `>> Emberfall profession specialisations: 9 trees, ...` |
| `emberfall_profspecs_player` | `OnPlayerLogin` | loads the ranks, drops those that no longer hold, reconciles the rank spells |
| | `OnPlayerCanUseChat` (whisper) | consumes every `EFP\t` addon whisper; answers the player's own |
| | `OnPlayerSetSkill` | a profession unlearned loses its ranks (GONE); learned or set: STATE |
| | `OnPlayerBeforeSendLoot` | yield: a herb or ore node's loot, once per loot generation, `+1` of a node's item on a roll |
| | `OnPlayerUpdateCraftingSkill` | the steady hand (a second item, the double batch) and the careful reading (one more discovery roll) after a craft |
| | `OnPlayerUpdate` | Far Sight: every 3 s the tracked nodes within sight + the ranks' yards the client does not have (`NODES`) |
| | `OnPlayerDeleteFromDB` | deletes the `ef_character_prof_*` rows |
| `emberfall_profspecs_trainers` | `AllCreatureScript::CanCreatureGossipHello/Select` | a profession trainer's menu plus "Unlearn <tree> (N points)" (box text, price); the reset |
| `emberfall_profspecs_commands` | `CommandScript` | `.efprof` |

Gather speed and duration are the rank spells' own `add_pct_modifier` auras (Emberfall's spell family 120); the module
only teaches and removes the rank spells.

## Test tools (Emberfall `docs/design/class-kit-pipeline.md` §8.2)

A test realm's players try a class kit at any level without a GM account: `src/emberfall_testtools.cpp`. Switch
`Emberfall.TestTools.Enable` (default 0; `AC_EMBERFALL_TEST_TOOLS_ENABLE`, the compose file's `EF_TEST_TOOLS_ENABLE`);
never on a live realm. `Emberfall.TestTools.Security` (0: any account), `.FoeEntry` and `.DummyEntry` (creature
templates of Emberfall's data; 0 turns that command off). Every command acts on the player's own character.

| Command | What |
|---|---|
| `.eftest level N` | own level 1 .. MaxPlayerLevel, XP 0, full health and mana (the talent system's level hook gives the points) |
| `.eftest foe [L] [N] [idle] [hpF]` | 1-5 foes of level L (default the player's) 10 yd ahead that attack (`idle`: at the player's feet until engaged; `hp1.6`: health x 1.6, as a template's HealthModifier); 5 min or until dead |
| `.eftest dummy [L]` | a dummy of level L: rooted, passive, no health regeneration, 200 x its health, keeps 1 health (`UnitScript::OnDamage`); 10 min |
| `.eftest clear` / `.eftest refresh` | the player's summons gone / full health and mana, cooldowns ended |

The level of a summon is set in `AllCreatureScript::OnBeforeCreatureSelectLevel` (a level pending for the next summon of
the command), so health, damage and armour are the `creature_classlevelstats` row of that level. Foes never leash back
to their summon point while they fight (`OnAllCreatureUpdate` keeps their leash timer fresh); a player who dies takes
their foes and dummies along (`OnPlayerJustDied`). Help texts:
`data/sql/db-world/mod_emberfall_2026_10_17_00_testtools.sql`. Boot line: `>> Emberfall test tools: on (...)`.

## Companions (Emberfall `docs/design/companions.md`)

AI party members for testing, like modern follower dungeons: a GM summons them with `.efparty` at the GM's level, by
role (tank, healer, dps, melee, ranged). Each is a Guardian of its owner (SummonProperties 61), so it follows, has the
owner's faction, gives the owner kill credit, has no threat list of its own and goes with the owner; a far teleport
summons the roster again. Switch `Emberfall.Companions.Enable` (default 1; `AC_EMBERFALL_COMPANIONS_ENABLE`); the
command needs `Emberfall.Companions.Security` (default 2). The other keys (`MaxPerPlayer`, `DpsWaitMs`, `DpsWaitThreat`,
`ThreatCap`, `Leash`, `FrameMs`, `NoProximityAggro`) are in companions.md §1.

| File | What |
|---|---|
| `data/sql/db-world/mod_emberfall_2026_10_13_00_companions.sql` | `ef_companion_archetype` (creature, role, scaling), `ef_companion_spell` (abilities by kind and priority, rank-1 IDs), the stock stand-in set (warrior, priest, mage, rogue, hunter: creatures 990001-990005 with ScriptName `npc_ef_companion`), help texts of `.efparty` |
| `src/companions/EfCompanionLogic.h` | the parts without the core: `.efparty add` arguments, formation, level scaling, rank choice, the healer's triage, the DPS start gate and threat cap, regen, the EFC message |
| `src/companions/EfCompanions.{h,cpp}` | config and data, the owner's roster (`Player::CustomData`), summoning and scaling, the mob tracker and stats, the EFC frame message |
| `src/companions/EfCompanionAI.cpp` | `npc_ef_companion`: one AI for every role (follow, assist, tank, heal, eat/drink, resurrect, leash, ground effects) |
| `src/emberfall_companions.cpp` | the hooks and `.efparty` |
| `tests/companion_tests.cpp` | the standalone tests of `EfCompanionLogic.h` (`tests/run_tests.sh`) |

| Script | Hook | What |
|---|---|---|
| `emberfall_companions_world` | `WorldScript::OnAfterConfigLoad`, `OnBeforeWorldInitialized` | reads the config, loads the archetypes (boot line `>> Emberfall companions: 5 archetypes: ...`) |
| `emberfall_companions_player` | `OnPlayerUpdate` | the owner's tracker (tank hold times, fight stats) every 100 ms and the EFC frame message (every 500 ms when it changed, else 5 s) |
| | `OnPlayerLogout`, `OnPlayerMapChanged`, `OnPlayerLevelChanged`, `OnPlayerJustDied` | forget the roster; summon it again on the new map; re-scale; count the death |
| `emberfall_companions_commands` | `CommandScript` | `.efparty [list]`, `add`, `remove`, `stay`, `follow`, `assist`, `level`, `stats`, `threat`, `reload` |
| `npc_ef_companion` | `CreatureScript` | the AI |

Protocol to Emberfall's client: an addon whisper in the owner's name, `EFC\t1;<count>;<guid hex>,<role>,<archetype>,<hp %>,
<mana % or ->,<state>;...` (the client hands this reserved prefix to its UI only from ourselves).

Movement is point movement at the height of the unit a companion goes to: the core's follow and chase generators
recompute their destinations from the map's ground, which a dungeon without vmaps (the stock-world profiles) does not
have, and a following guardian then climbs into the air.

On Emberfall's own world the archetypes are content: the world SQL fills `ef_companion_archetype` and
`ef_companion_spell` from Emberfall's `content/data/companions/companions.yaml` (a shield bearer, a lay healer, a
hedge-mage and a sellsword with Emberfall's own spells and looks; companions.md §9), so `.efparty` works there too.

## Dungeons (Emberfall `docs/design/dungeon-barrows.md`)

`src/emberfall_dungeons.cpp`; the logic without the core is `src/dungeons/EfDungeonLogic.h` (`tests/dungeon_tests.cpp`).

| Script | Hook | What |
|---|---|---|
| `instance_ef_barrows` | `InstanceMapScript` (map 751, instance_template.script) | the Barrows of Cairnmoor's three seals: the barrow bosses' SmartAI `set_inst_data` 1-3 (DONE) break them; the third wakes the First Interred (97015: selectable, no longer immune to players, its yell) and summons its honour guard (97008, three, zone combat) at the foot of the vault stair; saved with the instance (`EFB <seals> <final> <gate>`); log lines `Emberfall barrows: ...` |
| `npc_ef_dungeon_warden` | `CreatureScript` (gossip, AI) | the test realm's dungeon warden (97050 in Millbrook, 97051 inside the barrows): take me to the barrows / back to Millbrook / to the door, make me level 60, reset the barrows. **Only with `Emberfall.TestTools.Enable`**: off, its AI takes it out of the world on its first update and after every respawn (`Emberfall dungeon warden <spawn>: the test tools are off ..., it leaves the world`) and its gossip offers and does nothing |

## Dev vendors (Emberfall `docs/design/gear/frostguard-set.md` §5)

`src/emberfall_devvendor.cpp`; the gating without the core is `src/devvendor/EfDevVendorLogic.h`
(`tests/devvendor_tests.cpp`). These are NPCs for the local dev server only. The first is the dev quartermaster 92001
in Millbrook, who sells the Frostguard set for free (Emberfall `content/data/dev/npcs.yaml`).

- **Switch.** `Emberfall.DevVendor.Enable` (default 0; `AC_EMBERFALL_DEV_VENDOR_ENABLE`). It is its own switch, apart
  from the test tools. Emberfall's `ef_server.sh` sets 1; the public realms (`tools/gameserver`) set 0.
- **Spawn.** Their spawns are in the manual spawn group 92001, which the core never spawns by itself.
- **Price.** The free price is data, not code. The `npc_vendor` rows carry the zero extended cost 3001
  (`itemextendedcost_dbc`, from Emberfall's world SQL), and the core asks no money for an item with an extended cost.

| Script | Hook | What |
|---|---|---|
| `emberfall_devvendor_world` | `WorldScript::OnBeforeWorldInitialized` | on: `Map::SpawnGroupSpawn(92001)` on the group's map; the grid loader puts the NPCs in when their grid loads. Boot line on: `>> Emberfall dev vendors: on (...), spawn group 92001 (1 spawn(s)) active on map 0`; off: `>> Emberfall dev vendors: off (...)` |
| `npc_ef_dev_vendor` | `CreatureScript` (gossip, AI) | the second lock. Off: the NPC leaves the world on its first update and after every respawn (`Emberfall dev vendor <entry> (spawn N): ... is off, it leaves the world`), and its gossip, and so its vendor list, opens nothing. On: the core's own menu |

## Unstuck (Emberfall `docs/design/unstuck.md`)

`src/emberfall_unstuck.cpp`; the logic without the core is `src/unstuck/EfUnstuckLogic.h` (`tests/unstuck_tests.cpp`).
A stuck player is taken to the nearest town without a GM. `Emberfall.Unstuck.Enable` (1), `Emberfall.Unstuck.Cooldown`
(300 seconds; 0 none; kept in memory).

| Script | Hook | What |
|---|---|---|
| `spell_ef_unstuck` | `SpellScript` on SPELL_STUCK 7355 (spell_script_names from Emberfall's compiler) | CheckCast refuses it in combat, on a flight path and within the cooldown (`Unstuck: ...` system message); at the end of the cast (Emberfall's Spell.dbc: 5 s, moving and damage break it) a living player goes to the nearest `ef_unstuck_point` on their map, in an instance to its start point (`ObjectMgr::GetMapEntranceTrigger`), with no town on the map home; the dead keep the stock Stuck effect (their graveyard). Each use is logged: `Emberfall unstuck: <name> (guid N) from map M (x, y, z) to <place> map M (x, y, z)` |
| `emberfall_unstuck_commands` | `CommandScript` | `.stuck` (security 0): casts 7355, as the core's `.unstuck` does for a player account |
| `emberfall_unstuck_world` | `WorldScript` | reads the config; loads `ef_unstuck_point` (`OnLoadCustomDatabaseTable`); boot line `>> Emberfall unstuck: on (N town points, cooldown S s)` |

`data/sql/db-world/mod_emberfall_2026_10_18_10_unstuck.sql`: the `ef_unstuck_point` schema (Emberfall's world SQL fills
it from `content/data/world.yaml` teleports with `town:`), the `command` rows: `.unstuck` opened to security 0 (the stock
row's 2 overrides the core's player permission; a GM account keeps the GM form `.unstuck $player [inn/graveyard/startzone]`)
and `.stuck`.

## Absorbs (Emberfall `docs/design/cooldown-manager.md` §9.3)

`src/emberfall_absorbs.cpp`; the logic without the core is `src/absorbs/EfAbsorbLogic.h` (`tests/absorb_tests.cpp`).
3.3.5 never tells the client how much a shield has left; the Cooldown Manager shows it from these messages.
`Emberfall.Absorbs.Enable` (1; `AC_EMBERFALL_ABSORBS_ENABLE`).

Protocol: an addon whisper from the player to themselves (as EFP), at most one batch per 100 ms, one message per absorb
aura on the player (school absorb, mana shield) whose numbers changed: `EFA\t1;0;ABS;<spellId>;<left>;<cap>`; an aura
that went away: `EFA\t1;0;ABS;<spellId>;0;0`. `left` is the aura's current amount (its absorb effects added up; several
auras of one spell add up); `cap` for a class kit shell (`EfClassKit.h` `IsShellSpell`, the Frost Shell) is
`ShellCap` (effect 2's percent of max health, the cap `AddToShell` uses), for any other absorb the highest amount seen
since the aura was (re)applied. Passive auras (Last Ward's cheat death) and unlimited ones (a negative amount, Glacial
Bulwark's ward) are not reported. Nothing is sent at login unless an absorb is up.

| Script | Hook | What |
|---|---|---|
| `emberfall_absorbs_player` | `PlayerScript` `OnPlayerUpdate`, `OnPlayerLogin`, `OnPlayerLogout` | the 100 ms check and the messages (state in the player's `CustomData`; a player without absorbs costs one look at two aura lists); login and logout clear the state |
| `emberfall_absorbs_commands` | `CommandScript` | `.efabsorb` (GM): the selected player's (or one's own) tracked values, `EFABS <spellId> <left> <cap>` per absorb, then `EFABS end` |
| `emberfall_absorbs_world` | `WorldScript` | reads the config; boot line `>> Emberfall absorbs: on (EFA, every 100 ms)` |

## Harbours (Emberfall `docs/design/harbours-and-ships.md`)

`src/emberfall_harbours.cpp`; the logic without the core is `src/harbours/EfHarbourLogic.h` (`tests/harbour_tests.cpp`).
The ships themselves are stock AzerothCore MO transports (gameobject type 15 on a TaxiPathNode path, spawned from
`transports`; Emberfall's compiler writes the rows); the core's only change for them is PATCHES.md #11 (a malformed path is
skipped instead of crashing the boot). No table, no config.

| Script | Hook | What |
|---|---|---|
| `npc_ef_harbourmaster` | `CreatureScript` gossip (creature_template ScriptName from Emberfall's compiler) | the NPC's own npc_text (its entry: the route) and, per ship whose path stops within 90 yd of it on its map, a line with the next sailing from the live ship's path progress and period: `The War Ferry is at the pier: it sails in 35 s.` or `The War Ferry is due in 2 min 10 s and sails 50 s after.`; picking the line asks again |

## World editor (Emberfall `docs/design/world-editor.md`, phase A minimal)

Contributors with the editor role edit Elderwood's creature spawns and turn and move its people live on the build realm,
in change sets kept as operations in the editor's own database and applied in memory, live and again at every start. The
world database is never written. Off unless `Emberfall.Editor.Enable = 1` (`AC_EMBERFALL_EDITOR_ENABLE`) with
`Emberfall.Editor.Database` set and its tables present; off, no EFE message is ever answered.

| File | What |
|---|---|
| `src/editor/EfEditorLogic.h` | the EFE protocol and the rules as plain functions (requests, §6.3's values, density, rate windows, text rules, the ops' flat JSON, markers and pages, inverse/undo/redo, replay states); `tests/editor_tests.cpp` with a decode fuzz |
| `src/editor/EfEditor.{h,cpp}` | access (`ef_editor_access` in the auth DB, re-read every `Emberfall.Editor.RefreshSeconds` and on `.efedit access ID`), validation in §6.3's order, live apply (`CreatureData` + grids + the live units), replay at start, locks, audit, editor mode, the site's review states, local mode |
| `src/emberfall_editor.cpp` | the hooks (`OnPlayerCanUseChat` consumes every `EFE\t` whisper; another's is refused and logged), `.efedit status|access|refresh` |
| `data/sql/db-auth/mod_emberfall_2026_10_19_00_editor_access.sql` | `ef_editor_access` (the account site writes it) |
| `data/sql/db-editor/editor.sql` | the editor database (`ef_changeset`, `ef_op`, `ef_lock`, `ef_audit`): not a dbimport database; Emberfall's `realms.sh db` / `ef_server.sh editor` create it |

The world tables `ef_editor_zone`, `ef_editor_spawn`, `ef_editor_entry` come from Emberfall
(`python -m emberfall_content.editor.realmdata`), loaded after `world.sql` only where the editor runs. Config:
`Emberfall.Editor.Mode` `realm` (default) or `local` (a maintainer's server: every account a maintainer, Submit accepts).

