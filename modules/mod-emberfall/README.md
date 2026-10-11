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

The switch `Emberfall.OwnData` (default 0) is read through `src/server/game/World/EmberfallOwnData.h`
(`Emberfall::OwnData()`, shared with the remaining core hunks) from `worldserver.conf` or the
environment variable `AC_EMBERFALL_OWN_DATA` (the emberfall-data profile sets it). The module ships
no `conf/*.conf.dist`: AzerothCore loads a module's config file as required, and the docker
entrypoint copies only the `.dist`, so a missing `mod_emberfall.conf` would be logged at boot ("Failed open file", `Config.Policy` missing-file severity).
