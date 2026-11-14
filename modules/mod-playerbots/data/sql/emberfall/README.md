# Emberfall's SQL for mod-playerbots

The image has no module sources, so the module never makes or updates its database itself on an Emberfall realm
(`Playerbots.Updates.EnableDatabases = 0`). Emberfall's tooling applies these instead (game repo:
`tools/gameserver/realms.sh bots-db`, the pilot stack's `bp_bots.sh db`; docs/design/playerbots-pilot.md):

- `playerbots_schema.sql`: the playerbots database, once (a database without its tables). Every table of the module's
  `playerbots/base` and `playerbots/updates` (as of 405f8119), with the rows of the small mechanical tables only (stat
  weights, enchant templates); the chat texts, speech, bot names, guild and arena names, travel nodes, caches and
  custom strategies are tables without rows (other people's words or the stock world's data). Made by the pilot's
  `bp_schema.py` from the module's own SQL.
- `characters_schema.sql`: the module's three (empty) name tables in the characters database, idempotent.

Emberfall's own tables (`ef_bot_spec`, `ef_bot_routine`, `ef_bot_talent`, `ef_bot_name`, `ef_bot_character`) come
from the game repo's `content/data/bots/` (`python -m emberfall_content.botpilot sql`). This folder's name holds no
"auth", "characters" or "world", so AzerothCore's dbimport never reads it.
