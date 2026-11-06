-- mod-emberfall: per-character one-time migrations (Emberfall docs/design/professions.md §25). A row says the
-- migration `name` ran for the character; written only by the module (src/emberfall_one_profession.cpp: the login that
-- cuts a character from before 2026-11-06 to its one main profession, `one_profession`), deleted with the character
-- (OnPlayerDeleteFromDB).

CREATE TABLE IF NOT EXISTS `ef_character_migration` (
  `guid` INT UNSIGNED NOT NULL,
  `name` VARCHAR(32) NOT NULL,
  `applied` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`guid`, `name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall: one-time per-character migrations that ran';
