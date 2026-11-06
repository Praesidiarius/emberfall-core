-- mod-emberfall: the living waters in every zone (Emberfall docs/design/fishing.md §13): a species lives in any number of
-- waters (a band's species are shared across the band's zones), so its waters move from four columns of ef_fish_species
-- into ef_fish_species_water. The rows are Emberfall's compiler's (emptied and refilled each build), so the species table
-- is simply made again without the water columns.

DROP TABLE IF EXISTS `ef_fish_species`;
CREATE TABLE IF NOT EXISTS `ef_fish_species` (
  `id` INT UNSIGNED NOT NULL,
  `name` VARCHAR(64) NOT NULL DEFAULT '',
  `item` INT UNSIGNED NOT NULL DEFAULT 0,
  `rarity` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 common, 1 uncommon, 2 rare, 3 legendary',
  `style` INT UNSIGNED NOT NULL DEFAULT 0,
  `speed` INT UNSIGNED NOT NULL DEFAULT 1000,
  `size_typ_mm` INT UNSIGNED NOT NULL DEFAULT 0,
  `size_max_mm` INT UNSIGNED NOT NULL DEFAULT 0,
  `weight_k` INT UNSIGNED NOT NULL DEFAULT 1000,
  `clue` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 none, 1 rings, 2 swirl, 3 bubbles, 4 wake, 5 leap, 6 circle',
  `time_mask` TINYINT UNSIGNED NOT NULL DEFAULT 15,
  `weather_mask` TINYINT UNSIGNED NOT NULL DEFAULT 31,
  `h_shallows` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `h_deep` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `h_inlet` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `h_reeds` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `h_rocks` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `h_ruins` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `boost1_time` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `boost1_weather` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `boost1_permil` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `boost2_time` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `boost2_weather` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `boost2_permil` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `boost3_time` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `boost3_weather` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `boost3_permil` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `phase1_style` INT UNSIGNED NOT NULL DEFAULT 0,
  `phase1_until` INT UNSIGNED NOT NULL DEFAULT 0,
  `phase2_style` INT UNSIGNED NOT NULL DEFAULT 0,
  `phase2_until` INT UNSIGNED NOT NULL DEFAULT 0,
  `phase3_style` INT UNSIGNED NOT NULL DEFAULT 0,
  `phase3_until` INT UNSIGNED NOT NULL DEFAULT 0,
  `gate_quest` INT UNSIGNED NOT NULL DEFAULT 0,
  `needs_groundbait` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall fishing: the species (effish::Species)';

CREATE TABLE IF NOT EXISTS `ef_fish_species_water` (
  `species` INT UNSIGNED NOT NULL,
  `water` SMALLINT UNSIGNED NOT NULL,
  PRIMARY KEY (`species`, `water`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall fishing: the waters each species lives in';
