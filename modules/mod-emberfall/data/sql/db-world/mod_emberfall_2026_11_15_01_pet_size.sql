-- mod-emberfall: the hunter companion's size and growth (src/emberfall_petsize.cpp, src/petsize/EfPetSizeLogic.h; Emberfall
-- docs/design/hunter-taming.md §18, feedback #571 and #572). The schemas only: Emberfall's compiler writes the rows
-- (content/data/pets.yaml `size`, content/data/taming/model_sizes.yaml; tools/py/emberfall_content/pet_size.py). Sizes are
-- the side-on size sqrt(height x length) in thousandths of a yard; `permil` columns are x1000. No rows: the core's sizes.

CREATE TABLE IF NOT EXISTS `ef_pet_size_rule` (
  `id` TINYINT UNSIGNED NOT NULL COMMENT 'always 1',
  `reach_per_yard_permil` INT UNSIGNED NOT NULL DEFAULT 1000 COMMENT 'combat reach: the size x this',
  `reach_min_milli` INT UNSIGNED NOT NULL DEFAULT 1500,
  `reach_max_milli` INT UNSIGNED NOT NULL DEFAULT 3000,
  `radius_per_yard_permil` INT UNSIGNED NOT NULL DEFAULT 400 COMMENT 'bounding radius: the size x this',
  `radius_min_milli` INT UNSIGNED NOT NULL DEFAULT 300,
  `radius_max_milli` INT UNSIGNED NOT NULL DEFAULT 1200,
  `scale_min_permil` INT UNSIGNED NOT NULL DEFAULT 250 COMMENT 'the object scale within these',
  `scale_max_permil` INT UNSIGNED NOT NULL DEFAULT 4000,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall companion size: the reach, radius and scale limits';

CREATE TABLE IF NOT EXISTS `ef_pet_size_kind` (
  `family` INT UNSIGNED NOT NULL COMMENT 'CreatureFamily: the companion kind',
  `size_milli` INT UNSIGNED NOT NULL COMMENT 'its full size',
  PRIMARY KEY (`family`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall companion size: the full size of each kind';

CREATE TABLE IF NOT EXISTS `ef_pet_size_stage` (
  `stage` TINYINT UNSIGNED NOT NULL COMMENT '0: as tamed',
  `seconds` INT UNSIGNED NOT NULL COMMENT 'out with its hunter to reach it',
  `share_permil` INT UNSIGNED NOT NULL COMMENT 'of the kind''s full size',
  PRIMARY KEY (`stage`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall companion size: the growth stages';

CREATE TABLE IF NOT EXISTS `ef_pet_size_display` (
  `display` INT UNSIGNED NOT NULL COMMENT 'CreatureDisplayInfo',
  `size_milli` INT UNSIGNED NOT NULL COMMENT 'its side-on size at object scale 1',
  PRIMARY KEY (`display`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall companion size: each display''s own size';
