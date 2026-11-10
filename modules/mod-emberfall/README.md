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

The spec and talent system: Emberfall's own class trees (class trees in the class layout, spec trees in the
rows layout; the generated stock test trees and the grid layout were retired on 2026-10-27). Switch `Emberfall.Talents.Enable` (default 0; `AC_EMBERFALL_TALENTS_ENABLE`, read
through `sConfigMgr` like `Emberfall.OwnData` and independent of it; both Emberfall compose profiles set 1).
For a class the system is active only when `ef_spec` has rows for it; every other class keeps the stock
talent system untouched.

| File | What |
|---|---|
| `data/sql/db-world/mod_emberfall_2026_10_11_00_talents.sql` | the ten `ef_*` tree tables (schema only; Emberfall's compiler writes the rows into `world.sql`) and the help texts of the GM commands |
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
`LIMIT` past 5 per spec), `LOAD` (an apply for the active spec, a mark for another), `DEL` (`GONE`). The strip stays
once learned (Emberfall 2026-10-28, talents.md §1.3 rule 8; `EfTalentModel::Kept`/`CheckKept`/`WithKept`, the rule
`TalentRules.h isPermanent`): `APPLY` and `SAVE` to the live loadout without a strip slot the live loadout holds get
`ERR PERMANENT;node`, `RESET c` keeps it, `LOAD`, `SAVE` to another loadout and a spec switch put it in; the
login's DATA/LEVEL resets keep it too (`EfTalentModel::Refit`: the strip and the spec tree's points when they still fit,
the rest of the class tree refunded; 2026-10-29, when the class rows came to wait for the full strip, a loadout that
had bought rows past an unfinished strip got those points back this way); only `.ef talents reset` clears it. Limits: 20
requests per 10 s (one `THROTTLE` answer per window), `SPEC` once per 2 s, one reassembly of at most 8 x 251
bytes.

Per-spec action bars (§3.6): the active spec's bars are the core's (`character_action`); at a switch the old
spec's go to `ef_character_actions` and the new spec's come back (the first time: the current bars without
spells it does not know, plus its baseline spells in the first bar's empty buttons); `SMSG_ACTION_BUTTONS` 2
then 1.

Spells: learned with `Player::learnSpell` (saved in `character_spell`), removed with `removeSpell` plus the
auras they own or triggered; `ef_character_granted` records exactly what the module taught, so a spell the core
or a GM taught is never taken. Stock talent spells (only the retired generated stock test trees taught any) leave a no-talent-group entry
behind `removeSpell` that the core's rank logic would revive; the reconcile drops those (`DropLeftover`, kept as a
safety net).

Replacing nodes (WP M6, talents.md §1.4, `ef_talent_node.replaces`): while a node of the active loadout that
replaces spell X is invested, X is not taught (a baseline spell) or is removed (a spell the core taught: a trainer or
skill-line spell; recorded in `ef_character_granted` with `source` 3 and learned back when the node goes), and every
action button holding X (or another rank or option of the node) holds the node's current ability (its first
non-passive spell); refunded, or in a spec without the node, the buttons holding one of the node's spells go back to
X. The rewrite follows every reconcile (apply, load, reset, login, level change; `SMSG_ACTION_BUTTONS` 1 when a
button changed) and entering a spec (after its bars are loaded). `.ef talents dump` lists the swaps
(`replacing: X->by`, `by` 0 when the node is not invested) and the core spells taken.

The trees are v2 only (talents.md §5.3, 2026-10-27; db-world 2026_10_27_00): a node has at most one parent
(`ef_talent_node.parent`, the node before it in its row, which opens it once full), its effective gate in
`ef_talent_node.gate` and its pick group in `ef_talent_node.grp`. Free nodes per spec, several parents and edges
that open with part of a parent's points are gone (ef_talent_free, _edge, _gate and _pick dropped). Granted nodes
(`ef_talent_auto`) are never stored: the validation ignores their digits, and their spells are taught by the paid
points (and from their required level).

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
| `src/emberfall_classkit.cpp` | the hooks, `.efkit` / `.efkit purge`, the kits' spell scripts (the Druid's in `src/emberfall_classkit_power.cpp`, the Warlock's in `src/emberfall_classkit_heal.cpp`, the Paladin Wrathbringer's cards in `src/emberfall_classkit_wrath.cpp`: `spell_ef_kit_splash`, `spell_ef_kit_launch_stack`, `spell_ef_kit_main_stat`, `spell_ef_kit_empower`, `spell_ef_kit_execute`, `spell_ef_kit_cooldown_cut`; the Mage Pyromancer's Heat in `src/emberfall_classkit_heat.cpp`: `spell_ef_kit_power_tiers`, `spell_ef_kit_ramp_channel`, `spell_ef_kit_power_hold`; the spec power's tiers (`ef_class_kit_spec_power_tier`) in `src/classkit/EfSpecPower.cpp`) |

| Script | Hook | What |
|---|---|---|
| `emberfall_classkit_world` | `WorldScript::OnAfterConfigLoad`, `OnBeforeWorldInitialized` | reads the switch, loads the kits |
| `emberfall_classkit_player` | `OnPlayerLogin`, `OnPlayerLevelChanged` | for a kit class: takes away the class's own stock class skill lines (a SkillRaceClassInfo row naming the class alone), spells and auras of the stock family, buttons of spells the character does not know; logs what it took |
| `emberfall_classkit_commands` | `CommandScript` | `.efkit` (the kit and what the selected player knows of it), `.efkit purge` |
| `emberfall_classkit_auto_attack` (`src/emberfall_classkit_power.cpp`) | `UnitScript::ModifyMeleeDamage` | a kit player's white swing (main and off hand, before armour and the crit) deals the percent `ef_class_kit_auto_attack` (`data/sql/db-world/mod_emberfall_2026_10_29_01_auto_attack.sql`; rows from Emberfall's formulas.yaml `auto_attack`) gives for the player's level (Emberfall classes.md §4.7, feedback #168: 25 % at 1 up to 100 % from 51 [proposal]); abilities, the Hunter's Autoshot, pets and companions untouched; no table or no row: as upstream. Boot line `>> Emberfall class kits: the auto attack deals ...` |
| `spell_ef_kit_add_stacks`, `spell_ef_kit_shatter`, `spell_ef_kit_shell`, `spell_ef_kit_stack_cooldown`, `spell_ef_kit_absorb_pct_health`, `spell_ef_kit_line_targets`, `spell_ef_kit_interrupt_followup`, `spell_ef_kit_rest_regen` | `SpellScript`/`AuraScript` (`spell_script_names` from the kit) | the kits' generic mechanics, each reading its numbers from the spell's own effects (contracts at the top of `src/emberfall_classkit.cpp`): stacks with thresholds, shatter + a stacked shell (stacks follow the pool) + mana refund, cooldowns shortened per stack shattered, an absorb in percent of max health, line targets, an interrupt follow-up, out-of-combat mana |

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

## Profession XP (Emberfall `docs/design/professions.md`, the rank system)

Character XP from the primary professions (SkillLine category 11), Sundered Vale's `progression.md` §5 scaled to our
curve: every gather (a node, a skinned corpse) and every craft gives XP by its colour for the player. On with
`Emberfall.OwnData = 1` and `Emberfall.Professions.Xp.Enable` (default 1).

| Setting | Default | What |
|---|---|---|
| `Emberfall.Professions.Xp.Enable` | 1 | the switch |
| `Emberfall.Professions.Xp.Base` | 70 | XP of an orange or yellow action of requirement 0 |
| `Emberfall.Professions.Xp.PerSkill` | 0.06 | plus this per point of the action's requirement (before the rate: 175: 81, 325: 90, 500: 100) |
| `Emberfall.Professions.Xp.GreenPercent` | 50 | a green action's share; grey gives nothing |
| `Emberfall.Professions.Xp.Rate` | 0.1 | the one multiplier on all of the above (2026-10-28: profession XP is cosmetic; 7-10 XP an orange action, green 4-5) |

| File | What |
|---|---|
| `src/professions/EfProfessionXp.h` | pure: the colour of an action from the skill and its thresholds, the XP (`tests/professionxp_tests.cpp`, run by `tests/run_tests.sh`) |
| `src/emberfall_professions.cpp` | `emberfall_professions_world` (`OnAfterConfigLoad`: the settings, boot line `>> Emberfall profession XP: on ...`), `emberfall_professions_player`: `OnPlayerUpdateGatheringSkill` (the thresholds the core hands it, PATCHES.md #14: the requirement is yellow - 40) and `OnPlayerUpdateCraftingSkill` (the recipe's SkillLineAbility row: `MinSkillLineRank`, yellow `TrivialSkillLineRankLow`, green the midpoint, grey `TrivialSkillLineRankHigh`; a row without a high gives nothing) |

The XP is given with `Player::GiveXP(xp, nullptr)`: the client shows "You gain N experience."; rested XP does not double
it (the core adds the rest bonus only for kills), and at the level cap the core gives nothing. A debug line per action on
the `module` logger (skill, requirement, colour, XP). The colours and the skill-up chances themselves are the core's
(PATCHES.md #14, #15).

## Test tools (Emberfall `docs/design/class-kit-pipeline.md` §8.2)

A test realm's players try a class kit at any level without a GM account: `src/emberfall_testtools.cpp`. Switch
`Emberfall.TestTools.Enable` (default 0; `AC_EMBERFALL_TEST_TOOLS_ENABLE`, the compose file's `EF_TEST_TOOLS_ENABLE`);
never on a live realm. `Emberfall.TestTools.Security` (0: any account), `.FoeEntry` and `.DummyEntry` (creature
templates of Emberfall's data; 0 turns that command off). Every command acts on the player's own character.

| Command | What |
|---|---|
| `.eftest level N` | own level 1 .. MaxPlayerLevel, XP 0, full health and mana (the talent system's level hook gives the points), every weapon skill and Defence at the level's maximum (`TestZone::MaxCombatSkills`; `.eftest zone` and the Warden's level 60 too) |
| `.eftest zone Z [L]` | the player as one arriving at zone Z: its entry level (or L), the quests of the zones before it on the race's route rewarded without XP, money or items (its breadcrumbs put in the log, the quests after them left alone, Z's own untouched), the item pools' greens for class and level worn, the flight nodes so far, a purse of L x L silver, saved, at Z's main hub; `.eftest zone list`: the zones in route order (`src/emberfall_testzone.cpp`, `src/testzone/`) |
| `.eftest foe [L] [N] [idle] [hpF]` | 1-5 foes of level L (default the player's) 10 yd ahead that attack (`idle`: at the player's feet until engaged; `hp1.6`: health x 1.6, as a template's HealthModifier); 5 min or until dead |
| `.eftest dummy [L]` | a dummy of level L: rooted, passive, no health regeneration, 200 x its health, keeps 1 health (`UnitScript::OnDamage`); 10 min |
| `.eftest clear` / `.eftest refresh` | the player's summons gone / full health and mana, cooldowns ended |

The level of a summon is set in `AllCreatureScript::OnBeforeCreatureSelectLevel` (a level pending for the next summon of
the command), so health, damage and armour are the `creature_classlevelstats` row of that level. Foes never leash back
to their summon point while they fight (`OnAllCreatureUpdate` keeps their leash timer fresh); a player who dies takes
their foes and dummies along (`OnPlayerJustDied`). Help texts:
`data/sql/db-world/mod_emberfall_2026_10_17_00_testtools.sql`. Boot line: `>> Emberfall test tools: on (...)`.

`.eftest zone` reads its route from four world tables at every use (`data/sql/db-world/mod_emberfall_2026_10_21_00_testzone.sql`:
`ef_test_zone`, `ef_test_zone_quest`, `ef_test_zone_taxi`, `ef_test_gear`), whose rows Emberfall's compiler writes from its
zone briefs, the journey contract, the quests, people, teleports, flights and item pools (Emberfall
`tools/py/emberfall_content/zone_route.py`): a new zone works after Emberfall's `make content` and a world reload, with
no code change. The route logic without the core is `src/testzone/EfTestZoneLogic.h` (`tests/testzone_tests.cpp`).

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

`src/emberfall_unstuck.cpp`; the logic without the core is `src/unstuck/EfUnstuckLogic.h` (to town) and
`src/unstuck/EfUnstuckNearby.h` (nearby), both checked by `tests/unstuck_tests.cpp`. A stuck player frees themselves
without a GM: nearby first (`.stuck nearby`: the nearest good standing spot within 40 yd), else to the nearest town
(`.unstuck`, `.stuck`). `Emberfall.Unstuck.Enable` (1), `Emberfall.Unstuck.Cooldown` (300 seconds; 0 none),
`Emberfall.Unstuck.Nearby` (1), `Emberfall.Unstuck.NearbyCooldown` (300 seconds; 0 none); the cooldowns are per
character, kept in memory. Messages follow the session's locale (deDE German, `Befreien: ...`; else `Unstuck: ...`).

| Script | Hook | What |
|---|---|---|
| `spell_ef_unstuck` | `SpellScript` on SPELL_STUCK 7355 (spell_script_names from Emberfall's compiler) | CheckCast refuses it in combat, on a flight path and within the cooldown (`Unstuck: ...` system message); on the Stuck effect (7355 is instant in Emberfall's Spell.dbc) a living player goes to the nearest `ef_unstuck_point` on their map, in an instance to its start point (`ObjectMgr::GetMapEntranceTrigger`), with no town on the map home; the dead keep the stock Stuck effect (their graveyard). Each use is logged: `Emberfall unstuck: <name> (guid N) from map M (x, y, z) to <place> map M (x, y, z)` |
| `emberfall_unstuck_commands` | `CommandScript` | `.stuck` (security 0): casts 7355, as the core's `.unstuck` does for a player account. `.stuck nearby`: refused dead (ghosts take the town way: their graveyard), in combat, on a flight path, transport or vehicle, in a battleground or arena, within its cooldown; else the search below and `NearTeleportTo`; nothing good: `no free spot within 40 yd`, then the town way with its own rules. `.stuck status`: only the EFU line |
| `emberfall_unstuck_world` | `WorldScript` | reads the config; loads `ef_unstuck_point` (`OnLoadCustomDatabaseTable`); boot line `>> Emberfall unstuck: on (N town points, cooldown S s; nearby on, cooldown S s)` |

**The nearby search.** Rings at 5, 8, 12, 16, 20, 25, 30, 35, 40 yd, 12 points each; once a good spot turns up, the rings
up to 8 yd further are still probed, then the best wins. Each point snaps to the map's navmesh
(`MMapData::GetNavMeshQuery`, `findNearestPoly` with a ground-only filter: no water, magma, slime; at the player's height,
else from the top floor there). A spot is good when: 5-45 yd from the player on the ground; solid floor under it
(`Map::GetHeight` with vmaps within 1.5 yd of the polygon); `Map::isInLineOfSight` (vmaps and game objects) clear from
0.3 to 2.4 yd above it and blocked on at most one of four 0.8 yd chest-high rays; not under water; at least 150 yd² of
navmesh connected to it within 20 yd (`findPolysAroundCircle`; less is a closed pocket: inside a rock, a sealed room);
at most two living hostile creatures within 12 yd. Score: ground distance + 2 x the climb + 12 per hostile, lowest wins.
Logged: `Emberfall unstuck: <name> (guid N) nearby on map M from (x, y, z) to (x, y, z), D yd (ring R, N probed, open A
yd2, H hostiles)`, or `... nearby ...: no spot (...; N probed: no mesh n, too close n, ...); to town`.

**EFU** (addon whisper in the player's own name, after every unstuck request, 5 s after login (`emberfall_unstuck_player`, `PlayerScript::OnPlayerLogin`) and on `.stuck status`):
`EFU\t<nearby> <town>`, the seconds until each can be used again, -1 off. The client's game menu shows them.

`data/sql/db-world/mod_emberfall_2026_10_18_10_unstuck.sql`: the `ef_unstuck_point` schema (Emberfall's world SQL fills
it from `content/data/world.yaml` teleports with `town:`), the `command` rows: `.unstuck` opened to security 0 (the stock
row's 2 overrides the core's player permission; a GM account keeps the GM form `.unstuck $player [inn/graveyard/startzone]`)
and `.stuck`. `mod_emberfall_2026_10_24_00_unstuck_nearby.sql`: `.stuck nearby`, `.stuck status` (security 0) and the
help texts.

## Personal quest objects (Emberfall `docs/design/group-quest-loot.md`)

`src/emberfall_questloot.cpp`; the logic without the core is `src/questloot/EfQuestLootLogic.h`
(`tests/questloot_tests.cpp`). No config, no SQL: Emberfall's compiler names the scripts in `gameobject_template.ScriptName`
of every chest whose loot is only quest items (a reed stook, an eel trap, oakwort) and gives those items
`ITEM_FLAG_MULTI_DROP`, so a shared loot holds one copy per player (`LootItem::freeforall`). Ordinary chests, herb and
ore nodes, single-use goobers and doors keep the core's behaviour; a goober whose quest objective counts more than one
(the lifters' fold's three gates) gets `emberfall_personal_quest_goober` (below).

The stock chest is one loot per object: filled for the first opener only (`Player::SendLoot`), and its release when empty
(`WorldSession::DoLootRelease`, `GO_JUST_DEACTIVATED`) despawns it for everyone until `spawntimesecs`. Here each player
who needs the item takes it from the same object, and only they lose sight of it until their own respawn.

| Script | Hook | What |
|---|---|---|
| `emberfall_personal_quest_object` | `GameObjectScript::GetAI` (the AI below, Hide mode) | the object disappears for the player who took their item (`GameObjectAI::CanBeSeen`; destroyed for their client once their loot window is closed, `Player::UpdateVisibilityOf`) and comes back after the spawn's respawn time (at least 10 s); everyone else keeps it, with the sparkle while their quest wants the item |
| `emberfall_personal_quest_object_stays` | the same, Stay mode | for an object that never despawned (gameobjects.yaml `consumable: 0`, the wreck's fore-deck): stays in sight, only the lockout |
| the AI | `GossipHello` | a locked-out player's opening is refused (`Spell::SendLoot` and `GameObject::Use` ask it first; `SMSG_LOOT_RELEASE_RESPONSE`) |
| | `OnStateChanged` | the release of the emptied loot (`GO_JUST_DEACTIVATED` while the respawn time is 0) puts the object back to `GO_READY`: no despawn; the next opening fills fresh loot for that player. A real despawn (`DespawnOrUnsummon` sets the respawn time first) still despawns |
| `emberfall_questloot_player` | `OnPlayerBeforeSendLoot` | a player opening the object while another's loot is open gets their own quest item list (`Loot::FillNotNormalLootFor`) |
| | `OnPlayerLootItem` | the player who took an item from the object is locked out of it for its respawn time |
| `emberfall_personal_quest_goober` | `GameObjectScript::GetAI` (`PersonalQuestGooberAI`) | a clicked quest object credits each player once per spawn. Its `GossipHello` (asked first by `GameObject::Use`) takes the use over: credits the user and their group members in reward distance who are not locked out of the spawn (`KillCreditGO`), plays the open animation (`UseDoorOrButton`, the template's auto_close, 3 s without), and locks each credited player out (`UseLocks`: while their quest is incomplete and their objective count still covers that credit; abandoning, turning in or a repeatable's next round frees it). A locked player is refused and, after the open animation, no longer sees the spawn (`CanBeSeen`). The stock goober credited every click, so one gate clicked three times made 3/3 |

The lockouts live in the AI (per spawn, in memory): a grid unload or a restart forgets them. Quest items from creatures
need no script: `ITEM_FLAG_MULTI_DROP` on a `QuestRequired` row is enough (`Loot::FillQuestLoot` gives each group member in
loot range their own copy at the kill; `LootView` shows a freeforall quest item as lootable under every loot method, never
to the master looter or a roll; `Player::isAllowedToLoot` keeps the corpse lootable, `UNIT_DYNFLAG_LOOTABLE` per viewer,
for each member with something left on it).

## GM command fixes

`src/emberfall_gmcommands.cpp`; the logic without the core is `src/gmcommands/EfGmCommandLogic.h`
(`tests/gmcommand_tests.cpp`). No config, no SQL. Both take over the stock handler through
`AllCommandScript::OnTryExecuteCommand` (called after the core matched the typed words, before the handler; false skips
it) only for a well-formed call the invoker's permission allows; anything else (a bad value, no target, a player for
`.die`) still reaches the stock handler and its messages.

| Script | Hook | What |
|---|---|---|
| `emberfall_gmcommands_command` | `AllCommandScript::OnTryExecuteCommand` | `.modify speed [all\|walk\|backwalk\|swim\|fly] N`: the stock checks and messages (bounds 0.1-50, security, not on a flight path except `fly`), then the rate is set with `SetSpeed` (sent to the client: the stock `walk`/`backwalk`/`swim`/`fly` only stored it) and **held** per character and move type; N = 1 releases the types it covers and recalculates them (`UpdateSpeed`: auras and mount count again). `.die` on a living creature (not a pet, guardian or charmed unit): tapped for the GM unless someone tapped it already, the player damage requirement lowered as if the GM did it all (with their level), then `Unit::Kill(gm, creature)`: loot (lootable corpse, the loot sparkle), XP and quest credit like a real kill. The stock `.die` (Die.Command.Mode 1) killed without a tapper, so no loot and no reward. Logged as `Emberfall GM speed: ...` / `Emberfall .die: ...` (logger `module`) and in the GM log |
| `emberfall_gmcommands_player` | `OnPlayerUpdate`, `OnPlayerLogin`, `OnPlayerLogout` | a held rate that a recalculation replaced (`Unit::UpdateSpeed` from any speed aura applied or removed, a daze, a mount or dismount) is set again on the player's next update (not on a flight path or mid-teleport); login and logout forget the character's rates |

## Absorbs (Emberfall `docs/design/cooldown-manager.md` §9.3)

`src/emberfall_absorbs.cpp`; the logic without the core is `src/absorbs/EfAbsorbLogic.h` (`tests/absorb_tests.cpp`).
3.3.5 never tells the client how much a shield has left; the Cooldown Manager shows it from these messages.
`Emberfall.Absorbs.Enable` (1; `AC_EMBERFALL_ABSORBS_ENABLE`).

Protocol: an addon whisper from the player to themselves (as EFP), at most one batch per 100 ms, one message per absorb
aura on the player (school absorb, mana shield) whose numbers changed: `EFA\t1;0;ABS;<spellId>;<left>;<cap>`; an aura
that went away: `EFA\t1;0;ABS;<spellId>;0;0`. `left` is the aura's current amount (its absorb effects added up; several
auras of one spell add up); `cap` for a class kit shell (`EfClassKit.h` `IsShellSpell`, the Frost Shell) is
`ShellCap` (a full shell: its stack maximum x what one stack absorbs), for any other absorb the highest amount seen
since the aura was (re)applied. Passive auras (Last Ward's cheat death) and unlimited ones (a negative amount, Glacial
Bulwark's ward) are not reported. Nothing is sent at login unless an absorb is up.

| Script | Hook | What |
|---|---|---|
| `emberfall_absorbs_player` | `PlayerScript` `OnPlayerUpdate`, `OnPlayerLogin`, `OnPlayerLogout` | the 100 ms check and the messages (state in the player's `CustomData`; a player without absorbs costs one look at two aura lists); login and logout clear the state |
| `emberfall_absorbs_commands` | `CommandScript` | `.efabsorb` (GM): the selected player's (or one's own) tracked values, `EFABS <spellId> <left> <cap>` per absorb, then `EFABS end` |
| `emberfall_absorbs_world` | `WorldScript` | reads the config; boot line `>> Emberfall absorbs: on (EFA, every 100 ms)` |

## Quest log of 50 (Emberfall `docs/design/quest-log.md`, feedback #251)

`src/emberfall_questlog.cpp`; the logic without the core is `src/questlog/EfQuestLogLogic.h` and the core's
`EmberfallQuestLog.h` (`tests/questlog_tests.cpp`). The core keeps 50 quest log slots (PATCHES.md #36); slots 25-49 are
no update fields, so the client hears of them here. No switch: the core's 50 slots are unconditional.

Protocol: an addon whisper from the player to themselves (as EFA), on any update where one of slots 25-49 changed, one
message per changed slot: `EFQL\t1;0;SLOT;<slot>;<quest>;<state>;<c0>;<c1>;<c2>;<c3>;<time>` (quest 0: the slot is
empty now). The first update after a login sends `EFQL\t1;0;CLR` and then every slot in use. The longest body is 68
bytes (EFT's limit is 251).

| Script | Hook | What |
|---|---|---|
| `emberfall_questlog_player` | `PlayerScript` `OnPlayerUpdate`, `OnPlayerLogin`, `OnPlayerLogout` | compares the 125 words of slots 25-49 with what was sent and sends the changed slots (state in the player's `CustomData`); login and logout clear the state |
| `emberfall_questlog_commands` | `CommandScript` | `.efquestlog` (GM): the selected player's (or one's own) slots in use, `EFQL <slot> <quest> <state> <c0> <c1> <c2> <c3> <time>` each, then `EFQL end <used>/50` |

## Damage meter (Emberfall `docs/design/damage-meter.md`)

`src/emberfall_meter.cpp`; the logic without the core is `src/meter/EfMeterLogic.h` (`tests/meter_tests.cpp`). The
server counts every group's fights (or a solo player's) from the combat log at its source (the core's
`Emberfall::CombatLog` sink, PATCHES.md #12) and tells every member the same numbers. `Emberfall.Meter.Enable` (1;
`AC_EMBERFALL_METER_ENABLE`); 0 sets no sink and answers HELLO with OFF.

- Meters: one per group GUID or solo player; rows for players and companions (at most 40), pets, guardians and totems
  as sources of their owner's spells; a fight from the first hostile event to 3 s after everyone left combat (or 20 s
  of nothing), the last 10 fights and overall. Per participant: damage done (overkill and absorbed by the target
  apart), healing (effective, overheal, shields provided), damage taken (environmental apart), healing received,
  interrupts, dispels, deaths with a recap of the last 5 hits and heals, active time; per spell hits, crits, misses,
  total, over, absorbed, max, min.
- Protocol `EFM` (an addon whisper from the player to themselves, as EFA): `HELLO`, `VIEW`, `SPELLS`, `DEATHS`, `RESET`
  from the client; `ON`/`OFF`, `WHO`, `LIST`, `SEG`, `SUM` pushed once a second (only what changed), `SPL`, `DTH`/`DEV`,
  `END`, `OK`, `ERR` on demand (the header of `EfMeterLogic.h` has the fields). 30 requests per 10 s.

| Script | Hook | What |
|---|---|---|
| `emberfall_meter_world` | `WorldScript` | reads the config, sets the sink; boot line `>> Emberfall meter: on (EFM, ...)` |
| `emberfall_meter_player` | `PlayerScript` `OnPlayerUpdate`, `OnPlayerLogin`, `OnPlayerLogout`, `OnPlayerCanUseChat` | the combat flags of the player and its companions (every 250 ms), the push (every 1 s, after HELLO), the requests (`EFM\t` whispers to oneself, all consumed), the meter switch on a group change |
| `emberfall_meter_unit` | `UnitScript` `OnUnitDeath` | deaths of players and companions with their recap |
| `emberfall_meter_commands` | `CommandScript` | `.efmeter status` (the meter, the client's bytes and bytes per second in fights, the current fight and overall per participant), `.efmeter reset` (solo or the group's leader); both for every player |

## Harbours (Emberfall `docs/design/harbours-and-ships.md`)

`src/emberfall_harbours.cpp`; the logic without the core is `src/harbours/EfHarbourLogic.h` (`tests/harbour_tests.cpp`).
The ships themselves are stock AzerothCore MO transports (gameobject type 15 on a TaxiPathNode path, spawned from
`transports`; Emberfall's compiler writes the rows); the core's only change for them is PATCHES.md #11 (a malformed path is
skipped instead of crashing the boot). No table, no config.

| Script | Hook | What |
|---|---|---|
| `npc_ef_harbourmaster` | `CreatureScript` gossip (creature_template ScriptName from Emberfall's compiler) | the NPC's own npc_text (its entry: the route) and, per ship whose path stops within 90 yd of it on its map, a line with the next sailing from the live ship's path progress and period: `The War Ferry is at the pier: it sails in 35 s.` or `The War Ferry is due in 2 min 10 s and sails 50 s after.`; picking the line asks again |

## World editor (Emberfall `docs/design/world-editor.md`, phase A minimal)

Contributors with the editor role edit Elderwood's creature spawns and turn and move its people live on alpha1 (the build realm, its first realm, was retired on 2026-11-03),
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

## Leash (Emberfall `docs/design/leash-and-social-aggro.md`, feedback #93)

The open world's leash as in Classic: upstream renews a creature's leash timer (`Creature::GetLeashTimer`, 11 s to level
29, 15 s at 60) when it is hit, attacked or engaged; the module renews it also when the creature lands a melee or direct
spell hit (not damage over time) on its victim, and (feedback #185) when a player's or a pet's DoT ticks on it. A mob that
keeps hitting you, or that you keep hitting or dotting, never leashes; it
goes home once its timer ran out with its victim more than `CreatureLeashRadius` (30 yd, 2D) from its home, or after the
core's 10 s evade timer when it cannot reach its victim. Dungeons never leash by distance (upstream), so nothing changes
there. On with `Emberfall.OwnData = 1` and `Emberfall.Leash.MobHitsRenew` (default 1, `AC_EMBERFALL_LEASH_MOB_HITS_RENEW`); the DoT
half with `Emberfall.Leash.DotsRenew` (default 1, `AC_EMBERFALL_LEASH_DOTS_RENEW`).
Social aggro is the core's `CallAssistance` on Emberfall's faction data (no module code).

| File | What |
|---|---|
| `src/leash/EfLeashLogic.h` | pure: which landed hit renews the attacker's leash, which DoT tick renews the victim's (#185), the timer per level (`tests/leash_tests.cpp`) |
| `src/emberfall_leash.cpp` | `emberfall_leash_world` (`OnAfterConfigLoad`: the switch, boot line `>> Emberfall leash: ...`), `emberfall_leash_unit` (`UnitScript::DealDamage`, observes only: `Creature::UpdateLeashExtensionTime` for a renewing hit) |

## Account friends (Emberfall `docs/design/account-friends.md`)

Friends by account next to the stock character friends (`character_social`, untouched): a request goes to a character's
account, the other side accepts, and from then on each sees which character the other plays ("<label> is now online
playing <character>."). On unless `Emberfall.AccountFriends.Enable = 0` (`AC_EMBERFALL_ACCOUNTFRIENDS_ENABLE`); off,
HELLO answers `ERR OFF`.

| File | What |
|---|---|
| `src/accountfriends/EfAccountFriendsLogic.h` | the EFF bodies, labels, what an ADD comes to (an ignore answers as sent and stores nothing), the limits, the rate limit, the offline debounce; `tests/accountfriends_tests.cpp` |
| `src/emberfall_accountfriends.cpp` | the links of the accounts in the world, the requests, presence pushes (login: `on`; logout: `off` after 5 s unless another character came), the hooks |
| `data/sql/db-auth/mod_emberfall_2026_10_22_00_account_friends.sql` | `ef_account_friend` (one row per pair, each side's label), `ef_account_friend_request`, `ef_account_seen` |

| Script | Hook | What |
|---|---|---|
| `emberfall_accountfriends_world` | `OnAfterConfigLoad`, `OnBeforeWorldInitialized`, `OnUpdate` | the switch; requests older than 30 days go, the boot log line (`>> Emberfall account friends: on ...`); once a second, `off` for accounts that left 5 s ago and did not come back |
| `emberfall_accountfriends_player` | `OnPlayerLogin`, `OnPlayerLogout`, `OnPlayerCanUseChat` (whisper) | `on` to the friends in the world; last seen and the offline clock; consumes every `EFF\t` addon whisper and answers the player's own |

Presence is this worldserver's (a friend on another realm shows as offline).

## Start hearths (Emberfall `docs/design/stoneborn.md` §2.4)

The core binds a new character's hearth to its start (`playercreateinfo`). A race with a row in `ef_start_homebind`
(race, map, area, position; written by Emberfall's compiler from `player.yaml` `start.<race>.hearth`) gets its hearth
there at the character's first login instead: the stoneborn start at the Stonereach's sledge foot with their hearth at
the Banked Fire in Deepholm Stair. Races without a row keep the core's bind.

| File | What |
|---|---|
| `src/emberfall_homebind.cpp` | `emberfall_homebind_world` (`OnStartup`: reads the rows), `emberfall_homebind_player` (`OnPlayerFirstLogin`: `Player::SetHomebind` and `SMSG_BINDPOINTUPDATE`, as an innkeeper's bind) |
| `data/sql/db-world/mod_emberfall_2026_10_28_00_start_homebind.sql` | `ef_start_homebind` |


## Skills at a level (Emberfall `docs/design/gear/item-pools.md` §15)

There are no class trainers, so a skill Classic's trainer taught at a level comes by itself: a row of `ef_skill_level`
(class, skill, level, spell; written by Emberfall's compiler from `player.yaml` `skills_at_level`) gives the class that
skill at that level and not before. Today: the Hunter's Mail (413, proficiency spell 90013) at 40. At every login and
level change the skill is set when the level is reached (the client hears the armour types again); below it a stored
skill (a character made before the rule, a GM's level down) goes, and every equipped item needing it comes off into the
bags, or by mail when they are full.

| File | What |
|---|---|
| `src/emberfall_skill_levels.cpp` | `emberfall_skill_levels_world` (`OnStartup`: reads the rows), `emberfall_skill_levels_player` (`OnPlayerLogin`, `OnPlayerLevelChanged`: `Player::SetSkill`, `SendProficiency`, the unequip) |
| `data/sql/db-world/mod_emberfall_2026_10_29_00_skill_level.sql` | `ef_skill_level` |

## Buffs at any level (Emberfall feedback #237)

The core scales a player's positive aura to its target's level: a target more than 10 levels under the spell's
SpellLevel gets a lower rank, and with none it is dropped (`SPELL_FAILED_LOWLEVEL` when no target is left). The class
kits have no ranks, so Verdant Cocoon (SpellLevel 27, a pick at 10) failed on level-10 group members. Emberfall's
compiler gives every kit aura cast on someone else `SPELL_ATTR2_ALLOW_LOW_LEVEL_BUFF`; for such a spell the hook keeps
the target with the spell itself (`SpellInfo::GetAuraRankForLevel` returns it for the attribute).

| File | What |
|---|---|
| `src/emberfall_aura_scaling.cpp` | `emberfall_aura_scaling` (`OnScaleAuraUnitAdd`: the target scaled for an `ALLOW_LOW_LEVEL_BUFF` spell) |

## Extra action button (Emberfall `docs/contributing/quests.md` "Moments: the extra action button")

A quest moment (an escortee who sits down and wants a hand up) gives the players one spell for a while on the
Emberfall client's extra action button. The protocol is an aura: a marker aura's dummy effect with the misc value
`0x45464541` ("EFEA") carries the granted spell as its trigger spell; the content pipeline's SmartAI casts the marker on
the players and removes it (`remove_aura`), and its duration ends it anyway. The player never learned the spell, so the
core asks this module before it drops the cast (PATCHES.md #34): the cast goes through while the player carries a marker
granting that spell, and then runs with its own checks. Nothing else: granting and taking are data. On with
`Emberfall.OwnData = 1`.

| File | What |
|---|---|
| `src/extraaction/EfExtraActionLogic.h` | pure: the marker number, which aura effect grants which spell (`tests/extraaction_tests.cpp`) |
| `src/emberfall_extra_action.cpp` | sets the core's `Emberfall::ExtraAction` check (`EmberfallExtraAction.h`): the player's `SPELL_AURA_DUMMY` effects, a marker's trigger spell |

## Mini-encounters (Emberfall `docs/design/mini-encounters.md`)

The framework for short encounters with one thing in the world: fishing's fight on the line now (kind `F`), hunter
taming later (kind `T`). `Emberfall.Encounters.Enable` (1). One protocol, **EFN** (addon whispers to oneself,
`EFN\t<ver>;<seq>;<kind>;<cmd>;<fields>`, at most 255 bytes: `src/encounters/EfEncounterWire.h`, a verbatim copy of the
client's `client/src/data/EfEncounterWire.h`), one open encounter per player (`EfEncounterLogic.h`'s book), the item
groups (`ef_enc_item_group`), and a handler per kind (`EfEncounterMgr.h`: `RegisterKind`, `Begin`, `Finish`, `Send`,
`SendErr`; a kind implements `Enabled`, `OnMessage`, `OnFree`).

| Script | Hook | What |
|---|---|---|
| `emberfall_encounters_player` | `PlayerScript` | `OnPlayerCanUseChat`: every `EFN\t` addon whisper is consumed, the player's own (to itself) handled: over 20 a second dropped, undecodable `ERR;DECODE`, an unknown or switched-off kind `ERR;NOENC;OFF`, else the kind's handler. Logout, map change and death free the player's open encounter (the kind's `OnFree`) |
| `emberfall_encounters_world` | `WorldScript` | the config; `ef_enc_item_group` (boot line `>> Emberfall encounters: on (N groups, M items)`) |
| `emberfall_encounters_commands` | `CommandScript` | `.efenc dump [player]` (GM): the open encounter and the last ten EFN messages both ways; `.efenc reload`: the item groups |

## Fishing: the living waters (Emberfall `docs/design/fishing.md`, §11 the build contract)

`Emberfall.Fishing.LivingWaters` (1). A bobber (game object 90190, `ScriptName` `go_ef_fishing_bobber` from Emberfall's
compiler) whose position lies in no `ef_fish_cell` (8-yd cells, `floor(x / 8)`, `floor(y / 8)` per map; only zones
with a habitat file have rows), or the switch at 0, fishes exactly as the core does: the script returns false.

| Script | Hook | What |
|---|---|---|
| `go_ef_fishing_bobber` | `GameObjectScript::OnGossipHello` (the top of `GameObject::Use`) | the hook click in a cell: the roll (`src/fishing/EfFishLogic.h` over the shared `EfFishRules.h`: junk as the core rolls it, a fishing pool skips junk, counts a use and triples its own fish; the candidates by water, habitat bits, the part of the day by the sun (`Emberfall::Sun::PartNow()`, below: dawn 05-08, day 08-18, dusk 18-21, night 21-05 on the sun clock), the zone's weather and "after rain" (30 min), the bait aura (`ef_fish_bait`, likes in `ef_fish_species_bait`; no bait halves uncommons and refuses rares), the skill gates (base + 45, base + 95); the legendary: its `gate_quest` in the log means one bite in three when everything holds (ground-bait within 30 yd if `needs_groundbait`), its chain done a weight-2 candidate once a day per character), the size; the skill-up as the core (`OnPlayerUpdateFishingSkill`, `UpdateFishingSkill`). Junk: the core's junk loot. A common: the fish in the bobber's loot, the loot window. Uncommon and better: the bobber's spell id cleared and its loot state set to activated (the core's own way to keep a bobber with an open loot window: the channel's end leaves it, its escape timer stops), the fishing channel finished, Reeling (99264) cast with the bobber as `UNIT_FIELD_CHANNEL_OBJECT`, `OPEN` sent. A click on the bobber during its fight does nothing |
| `emberfall_fishing_player` | `PlayerScript` | each update during a fight: the bobber gone `CLOSE;GONE`, the angler dead `DIED`, mounted or over 30 yd from the bobber `MOVED`, the Reeling channel lost `MOVED` (moved) or `CHANNEL`, past the fight's maximum + 10 s `TIMEOUT`; the bobber goes. Login loads the character's best sizes |
| `FishingKind` (kind `F` of the framework) | EFN | `HELLO <hash>`: `OK` or `ERR;DATA;<server hash>` (`ef_fish_meta`). `END`: only the open fight's id, first END only (else `RESULT;<id>;REJ;NOENC`); `effish::CheckEnd` against the server's own clock from OPEN (the fastest landing any input could give less 0.5 s, the maximum + 10 s, the peak under the break and not under the fish's pull, time over 900, inputs for a rare or the legendary, the legendary's phases) and the angler (alive, unmounted, channelling Reeling, within 30 yd). Landed and fine: the fish in the bobber's loot, the loot window, `RESULT;<id>;OK;<item>;<size mm>;<grams>;<best>`; an accepted loss `RESULT;<id>;OK;0;<size>;<grams>;0`; anything else `RESULT;<id>;REJ;<FAST/SLOW/BREAK/PULL/OVER900/INPUTS/PHASES/OUTCOME/RANGE>`, logged with the numbers (logger `module`). More than 150 fights an hour is logged once |
| `at_ef_fish_watch` | `AreaTriggerScript` (`areatrigger_scripts` from Emberfall's compiler) | `ef_fish_event` kind 1 rows of the trigger: the quest (in the log) is done (`AreaExploredOrEventHappens`) when the species' time and weather conditions hold now. Kind 2 rows: landing the species does it |
| `spell_ef_fish_groundbait` | `SpellScript` on 99265 (`spell_script_names` from Emberfall's compiler) | where the ground-bait went in (8 yd before the angler), for 5 minutes |
| `emberfall_fishing_weather` | `ALEScript::OnWeatherChange` (the global weather hook) | each zone's weather and when its rain ended |
| `emberfall_fishing_world` | `WorldScript` | the config; loads every `ef_fish_*` table and the fishing pools' own fish (`gameobject_template` type 25's loot); boot line `>> Emberfall fishing: N waters, M cells, S species (...)` |
| `emberfall_fishing_commands` | `CommandScript` (GM) | `.effish cell`: the cell under your bobber (within 40 yd) or you: water, habitats, depth, time and weather bits, skill and base, bait, and each species living there now with its activity and weight; `.effish force <species>`: your next bite in a cell of its water is that species; `.effish stats [player]`: fights, landed, rejected, mean landing time since the server started, this hour's fights; `.effish reload` |

Every bite writes `ef_character_fish` (characters: caught, landed, lost, best size, first caught); the journal reads it
later. The legendary's once-a-day and the stats are kept in memory (a restart forgets them).

`data/sql/db-world/mod_emberfall_2026_11_05_00_fishing.sql`: the schemas of `ef_fish_meta`, `ef_fish_water`,
`ef_fish_cell`, `ef_fish_style`, `ef_fish_species`, `ef_fish_species_bait`, `ef_fish_bait`, `ef_fish_event`,
`ef_enc_item_group` (Emberfall's compiler writes the rows); `data/sql/db-characters/mod_emberfall_2026_11_05_00_fishing.sql`:
`ef_character_fish`. Tests: `tests/fishing_tests.cpp` (the golden vectors the client's tests share, the bounds, the
messages, the roll) and `tests/encounter_tests.cpp`.

## Hunter taming: Beastcraft, Drowse and Trust (Emberfall `docs/design/hunter-taming.md`, §14 the build contract)

`Emberfall.Taming.Enable` (1). The mini-encounters' first server-driven kind (`T`): the module keeps every beast's
state in memory (`src/taming/EfTameLogic.h`, pure and tested), ticks it, and sends `BARS` and `SLOTS`
(`EfEncounterWire.h`) and kind `T`'s `OPEN`, `EAT`, `RESULT`, `CLOSE` (`src/taming/EfTameWire.h`, a verbatim copy of
the client's). Gentling (93120) is retired; its checks moved to Drowse Shot; `gentling_tame` (93121) is the tame's
helper (TAME_CREATURE and the class quests' credit).

| Script | Hook | What |
|---|---|---|
| `emberfall_taming_player` | `PlayerScript` | login, level-up and every Beastcraft skill-up: a hunter of 10 or more learns Beastcraft (99520) and Drowse Shot (99524), the ranks by level and skill (Journeyman 20 / 50, Craftsman 30 / 125, Master 40 / 200), the recipes by skill (`learnSkillRewardedSpells`); Gentling leaves the action bars. Each update: a due tame of the player's (an out companion stabled first, else `BARS` phase 3; the hunter dead or past 40 yd: phase 4). Death while drowsing drops the beast's Drowse |
| `spell_ef_drowse_shot` | `SpellScript` on 99524 | the checks (level, tameable, not owned, a free stall when a companion is out; someone else's quarry `ERR;CLAIM;TAPPED`; no arrow `ERR;BOUNDS;ARROWS`); the hit: the strongest drowse arrow (`ef_tame_arrow`) the beast's level allows (else the strongest, a quarter), the tag, the leash timer, Drowse (none while another crowd control holds the beast), `BARS` (id 0) to the claimant's group; full: Out Cold |
| `spell_ef_out_cold` | `AuraScript` on 99525 | stands up a beast whose state the module no longer has (the aura's own duration is the safety net) |
| `emberfall_taming_creature` | `AllCreatureScript::OnAllCreatureUpdate` | every 500 ms for a beast with a state: drowsing, the decay (after 6 s, or at once while held), evade or out of combat forgets it; asleep, a draught under 15 s (the aura renewed), one item eaten every 4 s (`EAT`; refused food sniffed once, `EAT R`), the frame closed past 5 yd (`CLOSE;RANGE`), `SLOTS`, `BARS`; the sleep out: wakes (`RESULT W`), uneaten items back within 40 yd, attacks the claimant within 30 yd or evades |
| `emberfall_taming_unit` | `UnitScript::OnUnitDeath` | a beast with a state dies: `RESULT D`, the bars dropped |
| `TamingKind` (kind `T`) | EFN | `OPEN <beast>` (the claimant, alive, within 5 yd: `OPEN`, `SLOTS`, `BARS`; else `ERR;CLAIM`, `ERR;RANGE`, `ERR;NOENC;AWAKE`/`BUSY`), `PUT` (the item at the Lua bag position, in `tame_food`, fits the slot: taken from the bags), `TAKE` (back into the bags, else `ERR;BOUNDS;FULL`), `CLOSE` |
| `emberfall_taming_world` | `WorldScript` | the config; the `ef_tame_*` tables (boot line `>> Emberfall taming: on (N families, M arrows, D draughts)`); a sweep every 10 s drops states no creature ticked for 30 s |
| `emberfall_taming_commands` | `CommandScript` (GM) | `.eftame state` (the targeted beast), `.eftame force` (drowsing: Out Cold; asleep: Trust full), `.eftame reload` |

`data/sql/db-world/mod_emberfall_2026_11_05_01_taming.sql`: `ef_tame_rule`, `ef_tame_family`, `ef_tame_like`,
`ef_tame_arrow`, `ef_tame_draught` (Emberfall's compiler writes the rows from `content/data/taming/families.yaml`).
Tests: `tests/taming_tests.cpp` (the messages' round trips and fuzz, the bars, arrows, decay, claim, sleep cap, food by
family and band, the eating order, the slots).

## The seam guard (Emberfall `docs/design/kindred-map-split.md` §3.4 item 4)

The Kindred is two maps, 1 north and 754 south. Each draws a one-tile margin of the other half, purely visual, whose
terrain has a "beyond the seam" area (`ef_seam_area`: area id and the map whose margin it is; Emberfall's compiler
writes the rows). A player who stands in such an area (zone or subarea) is put back on the last spot on their own side
(`NearTeleportTo`), told "The ground ends here. Take a ship." (deDE "Hier endet das Land. Nimm ein Schiff.", frFR "La
terre s'arrête ici. Prenez un navire."; the session's locale, else English); with no spot on this map (a login there)
to their hearth. GMs in GM mode, flight paths, ships and vehicles are left alone; a spot in the air is not remembered.
No rows: nothing happens. Each turn-back is logged: `Emberfall seam guard: <name> (guid N) in area A (zone Z) of map M
at (x, y, z) sent back to (x, y, z)` (or `... home to map M`).

| Script | Hook | What |
|---|---|---|
| `emberfall_seam_player` | `PlayerScript` (`OnPlayerUpdate`, once a second; `OnPlayerLogin`: the first look after 3 s) | the look: zone and area of the position, the last own spot in `Player::CustomData` (`ef_seam`) |
| `emberfall_seam_world` | `WorldScript::OnLoadCustomDatabaseTable` | loads `ef_seam_area`; boot line `>> Emberfall seam guard: N area(s) beyond the seam` |

`data/sql/db-world/mod_emberfall_2026_11_06_01_seam_area.sql`: `ef_seam_area`. The rules: `src/seam/EfSeamLogic.h`;
tests: `tests/seam_tests.cpp`. The core lists map 754 as a continent (`PATCHES.md` #39).

## Account-wide achievements (Emberfall `content/data/achievements.yaml`)

3.3.5 knows only per-character achievements. An Achievement.dbc row with Emberfall's flag 0x8000 (the bit later
clients give account-wide achievements; unused in 3.3.5; Emberfall's exploration achievements carry it) is shared by
every character of the account: the first character to earn it puts it into `ef_account_achievement` (account,
achievement, first date; kept when that character is deleted) and into every other character's `character_achievement`
with the same date (they load it quietly at their next login). At login a character gets what the account has and it
lacks (a newer character; `Player::CompletedAchievement` a second later: toast, and the criteria that build on it, a
continent's exploration), and the account gets what the character earned before the module. Criteria progress stays per
character; a GM in GM mode earns nothing (the core's rule), the grant waits for a login out of it. Character-wide stays
the default.

| Script | Hook | What |
|---|---|---|
| `emberfall_account_achievements_player` | `OnPlayerAchievementComplete` | an account-flagged achievement: into `ef_account_achievement` and the account's other characters |
| | `OnPlayerLogin` | the character's account-flagged achievements into the account; the account's missing here granted after 1 s, then the achievement-completion criteria re-checked (a continent's exploration counts the zones that came in quietly) |

`data/sql/db-characters/mod_emberfall_2026_11_08_00_account_achievements.sql`: `ef_account_achievement`, and every
character's `at_login` gets `AT_LOGIN_CHECK_ACHIEVS` (0x400) once, so levels reached and areas explored before
Emberfall's first achievements count at the next login.

## The sun's hours (Emberfall `docs/design/atmosphere.md`, "The sun's hours")

The game clock is the server's real local time; this makes the world's sunrise and sunset the real ones too. The server
works out each day's sunrise and sunset for its place and date and tells every client (so all players see the same
sky); what the server itself does by the time of day reads the same **sun clock**: the local time mapped so that the
day's sunrise is 06:00 and its sunset 19:45, where Emberfall's lights and fishing's windows are written.

| Setting | Default | What |
|---|---|---|
| `Emberfall.Sun.Mode` (`AC_EMBERFALL_SUN_MODE`; the compose file's `EF_SUN_MODE`) | `real` | `real`: the real sunrise and sunset; `softened`: half way between the fixed hours and the real ones; `fixed`: the fixed hours |
| `Emberfall.Sun.Latitude`, `Emberfall.Sun.Longitude` | 47.37, 8.54 | the place (degrees north, east; Zurich) |
| `Emberfall.Sun.FixedSunrise`, `Emberfall.Sun.FixedSunset` | 6.0, 19.75 | the fixed hours |
| `Emberfall.Sun.DaylightEvent` | 200 | the game event that runs by day (Emberfall's world.yaml: NPCs with `hours`, quest markers); 0: none |

Read at start and on `.reload config`. The daylight saving is the server clock's own: the hours are local hours by the
UTC offset of the date's noon (`TZ`).

| File | What |
|---|---|
| `src/sun/EfSunTime.h` | a **verbatim copy** of Emberfall's `client/src/data/EfSunTime.h` (Apache-2.0; Emberfall's `tools/py/tests/test_sun_hours.py` compares them): the sun clock and its inverse, the parts of the day, the modes' names, the EFSUN message |
| `src/sun/EfSunLogic.h` | pure: dates, the NOAA sunrise equation (`SolarHours`), the day's hours per mode in whole minutes (`HoursFor`; a polar day or night keeps an hour of night or day), a daily event's window on the real clock (`DayWindow`, `CurrentWindow`) |
| `src/sun/EfSun.h` | for the module's other scripts: `Emberfall::Sun::Hours()`, `SunClockNow()`, `PartNow()` (any thread). **Anything that goes by day and night asks here, never the clock's hour** |
| `src/emberfall_sun.cpp` | the scripts below |
| `tests/sun_tests.cpp` | Zurich's sunrises and sunsets through the year (solstices, equinoxes, both daylight-saving change days), other places, the modes, the sun clock's vectors (the client's too), the parts of the day at the windows' edges, the message, the event's window (`tests/run_tests.sh`) |

| Script | Hook | What |
|---|---|---|
| `emberfall_sun_world` | `WorldScript::OnAfterConfigLoad` | the settings; a reload works out the day again and tells everyone |
| | `OnBeforeWorldInitialized` | the daylight event's hours from its data (`game_event.start_time`'s time of day, `length`; it must come round once a day), its first window; boot line `>> Emberfall sun: real at 47.37 N 8.54 E, 2026-11-10 (UTC+1): sunrise 07:22, sunset 16:58 (EFSUN); game event 200 follows the sun` |
| | `OnUpdate` (every 10 s) | the server's day turned: the new hours to everyone in the world; the daylight event's window moved when it is over |
| `emberfall_sun_player` | `PlayerScript::OnPlayerLogin` | the day's hours with the login's first packets |
| `emberfall_sun_commands` | `CommandScript` | `.efsun` (GM, console): the mode, place and date, today's hours in use and in each mode, the clock and the sun clock now with the part of the day, the daylight event's window; `.efsun mode real\|softened\|fixed` (admin, console): switches at once for everyone, until the next config reload or restart |

Protocol: an addon whisper in the player's own name (as EFU), `EFSUN\t<ver>;<mode>;<sunrise>;<sunset>`, the hours in
whole minutes after the server's local midnight (`EFSUN\t1;real;442;1018`). Nothing comes back. A client without it
ignores the reserved prefix; a later version may add fields after these four.

The daylight event: the core starts and stops a game event by its own clock (`GameEventMgr::CheckOneGameEvent`: `Start`,
every `Occurence`, for `Length`). The script sets the event's `Start` and `Length` to the window the sun gives it (of
yesterday's, today's and tomorrow's the first that has not ended) and calls `World::ForceGameEventUpdate()`; the core
does the rest as for any event. The manager hands its events out read-only (`GetEventMap() const`) and has no setter:
the script casts the const away for these two fields (no core patch; on the world thread, where the manager's own
update runs). In `fixed` mode the window is the data's hours in the server's local time (06:00-20:00), whatever the
database session's time zone made of `start_time`.

Used by: fishing's time of day (`emberfall_fishing.cpp TimeBitNow`: the bite roll, `.effish cell`, `at_ef_fish_watch`).
No table, no SQL.
