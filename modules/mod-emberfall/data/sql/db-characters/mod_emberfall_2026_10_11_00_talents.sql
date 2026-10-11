-- mod-emberfall: per-character spec and talent state (Emberfall docs/design/talents.md §2.6, WP T3).
-- Written only by the module; deleted with the character (OnPlayerDeleteFromDB).

CREATE TABLE IF NOT EXISTS `ef_character_spec` (
  `guid` INT UNSIGNED NOT NULL,
  `active_spec` SMALLINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 = none chosen yet',
  `data_hash` CHAR(16) NOT NULL DEFAULT '' COMMENT 'the talent data hash the loadouts were last validated under',
  `flags` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'bit 0: stock talents wiped once',
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall talents: the character''s spec';

CREATE TABLE IF NOT EXISTS `ef_character_loadout` (
  `guid` INT UNSIGNED NOT NULL,
  `spec` SMALLINT UNSIGNED NOT NULL,
  `loadout` TINYINT UNSIGNED NOT NULL COMMENT '1-n',
  `name` VARCHAR(64) NOT NULL DEFAULT '',
  `ranks` TEXT NOT NULL COMMENT 'nodeID:rank (digit) or nodeID:option (a-c) pairs, comma-separated; invested nodes only',
  `data_hash` CHAR(16) NOT NULL DEFAULT '',
  `active` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `updated` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'unix time',
  PRIMARY KEY (`guid`, `spec`, `loadout`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall talents: loadouts of every spec';

CREATE TABLE IF NOT EXISTS `ef_character_actions` (
  `guid` INT UNSIGNED NOT NULL,
  `spec` SMALLINT UNSIGNED NOT NULL,
  `button` TINYINT UNSIGNED NOT NULL,
  `action` INT UNSIGNED NOT NULL DEFAULT 0,
  `type` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`guid`, `spec`, `button`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall talents: action bars of the inactive specs';

CREATE TABLE IF NOT EXISTS `ef_character_granted` (
  `guid` INT UNSIGNED NOT NULL,
  `spell` INT UNSIGNED NOT NULL,
  `source` TINYINT UNSIGNED NOT NULL DEFAULT 2 COMMENT '1 baseline, 2 node',
  PRIMARY KEY (`guid`, `spell`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall talents: spells the module taught';
