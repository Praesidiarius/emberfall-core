-- mod-emberfall: the living waters (src/emberfall_fishing.cpp, Emberfall docs/design/fishing.md §11.3): each character's
-- record per species. The pilot writes it at every bite; the journal (phase 3) reads it.
CREATE TABLE IF NOT EXISTS `ef_character_fish` (
  `guid` INT UNSIGNED NOT NULL,
  `species` INT UNSIGNED NOT NULL,
  `caught` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'hooked',
  `landed` INT UNSIGNED NOT NULL DEFAULT 0,
  `lost` INT UNSIGNED NOT NULL DEFAULT 0,
  `best_mm` INT UNSIGNED NOT NULL DEFAULT 0,
  `first_caught` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'unix time',
  PRIMARY KEY (`guid`, `species`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall fishing: each character''s fish';
