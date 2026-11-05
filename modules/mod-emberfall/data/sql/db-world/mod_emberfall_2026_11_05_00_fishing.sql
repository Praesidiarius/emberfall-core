-- mod-emberfall: the living waters and the mini-encounter framework (src/emberfall_fishing.cpp, src/emberfall_encounters.cpp;
-- Emberfall docs/design/fishing.md §11.3, mini-encounters.md §3). The schemas only: Emberfall's compiler writes the rows
-- (content/data/fishing/: species.yaml, waters.yaml, habitats/<zone>.yaml; content/data/encounters/item_groups.yaml).
-- Numbers are integers; `permil` columns are x1000. The columns of ef_fish_species are effish::Species (EfFishRules.h).

CREATE TABLE IF NOT EXISTS `ef_fish_meta` (
  `id` TINYINT UNSIGNED NOT NULL COMMENT 'always 1',
  `data_hash` BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'the first 8 bytes of the SHA-256 of the species and styles rows (the EFN HELLO hash)',
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall fishing: the data hash';

CREATE TABLE IF NOT EXISTS `ef_fish_water` (
  `id` SMALLINT UNSIGNED NOT NULL,
  `map` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `zone` INT UNSIGNED NOT NULL DEFAULT 0,
  `kind` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 lake, 1 river, 2 pond, 3 sea',
  `name` VARCHAR(64) NOT NULL DEFAULT '',
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall fishing: the water bodies with habitats';

CREATE TABLE IF NOT EXISTS `ef_fish_cell` (
  `map` SMALLINT UNSIGNED NOT NULL,
  `cx` INT NOT NULL COMMENT 'floor(world x / 8)',
  `cy` INT NOT NULL COMMENT 'floor(world y / 8)',
  `water` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `mask` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'habitat bits: 1 shallows, 2 deep, 4 inlet, 8 reeds, 16 rocks, 32 ruins',
  `depth_dm` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`map`, `cx`, `cy`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall fishing: the 8-yd habitat cells (a bobber in no cell fishes as the core does)';

CREATE TABLE IF NOT EXISTS `ef_fish_style` (
  `id` INT UNSIGNED NOT NULL,
  `kind` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '1 darter, 2 bulldog, 3 snagger',
  `pull` INT UNSIGNED NOT NULL DEFAULT 0,
  `drain` INT UNSIGNED NOT NULL DEFAULT 0,
  `every_min_ms` INT UNSIGNED NOT NULL DEFAULT 0,
  `every_max_ms` INT UNSIGNED NOT NULL DEFAULT 0,
  `magnitude` INT UNSIGNED NOT NULL DEFAULT 0,
  `hold_ms` INT UNSIGNED NOT NULL DEFAULT 0,
  `over900_ms` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall fishing: the fight styles (effish::Style)';

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
  `water1` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `water2` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `water3` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `water4` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
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

CREATE TABLE IF NOT EXISTS `ef_fish_species_bait` (
  `species` INT UNSIGNED NOT NULL,
  `item` INT UNSIGNED NOT NULL,
  `permil` INT UNSIGNED NOT NULL DEFAULT 1000 COMMENT '0 refuses; a bait not listed is 1000',
  PRIMARY KEY (`species`, `item`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall fishing: what each species makes of each bait';

CREATE TABLE IF NOT EXISTS `ef_fish_bait` (
  `item` INT UNSIGNED NOT NULL,
  `aura` INT UNSIGNED NOT NULL DEFAULT 0,
  `live` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`item`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall fishing: the baits and the auras their use casts';

CREATE TABLE IF NOT EXISTS `ef_fish_event` (
  `quest` INT UNSIGNED NOT NULL,
  `kind` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '1 watch (the trigger under the species'' conditions), 2 land (landing the species)',
  `species` INT UNSIGNED NOT NULL DEFAULT 0,
  `trigger_id` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`quest`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall fishing: quests done by the water';

CREATE TABLE IF NOT EXISTS `ef_enc_item_group` (
  `group_name` VARCHAR(32) NOT NULL,
  `item` INT UNSIGNED NOT NULL,
  `band` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`group_name`, `item`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall mini-encounters: the item groups (mini-encounters.md §3)';
