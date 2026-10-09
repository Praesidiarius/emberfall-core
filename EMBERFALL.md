# Emberfall fork of AzerothCore

This branch carries Emberfall's changes on top of upstream AzerothCore. Licence: AGPL-3.0, as upstream.

Current changes:

- `docker-compose.override.yml`: host networking, MySQL bound to 127.0.0.1, server data from
  `var/serverdata` instead of the upstream client-data download, Warden off, vmaps/mmaps off.

Setup, data extraction and accounts: see `docs/dev-server.md` in the Emberfall repository.

Planned (spec section 5.2): audit and isolate hardcoded Blizzard IDs, then point the server at
Emberfall's own DBCs, maps and world database.
