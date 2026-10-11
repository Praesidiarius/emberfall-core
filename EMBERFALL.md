# Emberfall fork of AzerothCore

This branch carries Emberfall's changes on top of upstream AzerothCore. Licence: AGPL-3.0, as upstream.

Current changes (every code change is listed in `PATCHES.md`):

- `docker-compose.override.yml`: host networking, MySQL bound to 127.0.0.1, server data from
  `var/serverdata` instead of the upstream client-data download, Warden off, vmaps/mmaps off.
- `Emberfall.OwnData` (default off): skips the stock-data checks and fix-ups so the worldserver runs
  on Emberfall's own DBCs, maps and world database (`PATCHES.md`).
- `modules/mod-emberfall`: the parts of `Emberfall.OwnData` that AzerothCore's script hooks can carry
  (spirit healer confirm, start-position check, Wintergrasp off, raid-difficulty guard), so the core
  patch stays small (`PATCHES.md`, "Module").
- `modules/mod-emberfall` also carries the spec and talent system (`Emberfall.Talents.Enable`; Emberfall
  `docs/design/talents.md` §3): the `ef_*` world and `ef_character_*` tables, the `EFT` addon-message
  protocol, validation and spell reconcile, GM `.ef` commands. Module code only.
- `docker-compose.emberfall-data.yml`: a second, separate server ("emberfall-data" profile) on
  Emberfall's own data, with its own containers (`ef-data-*`), ports (MySQL 3320, auth 3725, world
  8086), volume, data (`var/serverdata-ef`) and images (`emberfall/ac-wotlk-*:own-data`). Driven
  by `tools/server/ef_server.sh` in the Emberfall repository. The same file runs Emberfall's "stock-test"
  profile (`EF_PROFILE=stock-test`: `ef-stock-*`, stock data and world DB, full stock scripts) for testing
  the talent system on stock classes.

Setup, data extraction and accounts: see `docs/dev-server.md` in the Emberfall repository.

Planned (spec section 5.2): audit and isolate hardcoded Blizzard IDs, then point the server at
Emberfall's own DBCs, maps and world database.
