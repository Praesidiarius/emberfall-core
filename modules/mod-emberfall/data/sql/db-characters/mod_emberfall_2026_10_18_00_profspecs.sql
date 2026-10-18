-- mod-emberfall: per-character profession specialisation state (Emberfall docs/design/professions.md §10.3).
-- Written only by the module; deleted with the character (OnPlayerDeleteFromDB).

CREATE TABLE IF NOT EXISTS `ef_character_prof_rank` (
  `guid` INT UNSIGNED NOT NULL,
  `node` SMALLINT UNSIGNED NOT NULL COMMENT 'ef_prof_node.id (stable across content versions)',
  `rank` TINYINT UNSIGNED NOT NULL,
  PRIMARY KEY (`guid`, `node`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall profession specialisations: ranks';

CREATE TABLE IF NOT EXISTS `ef_character_prof_granted` (
  `guid` INT UNSIGNED NOT NULL,
  `spell` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`guid`, `spell`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall profession specialisations: rank spells the module taught';
