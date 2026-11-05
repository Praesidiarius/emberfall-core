-- mod-emberfall: hunter taming, Beastcraft's Drowse and Trust (src/emberfall_taming.cpp; Emberfall
-- docs/design/hunter-taming.md §14.3). The schemas only: Emberfall's compiler writes the rows (content/data/taming/
-- families.yaml over content/data/encounters/item_groups.yaml, which fill ef_enc_item_group). Numbers are integers;
-- `permil` columns are x1000.

CREATE TABLE IF NOT EXISTS `ef_tame_rule` (
  `id` TINYINT UNSIGNED NOT NULL COMMENT 'always 1',
  `drowse_base` INT UNSIGNED NOT NULL DEFAULT 20,
  `drowse_per_level` INT UNSIGNED NOT NULL DEFAULT 4,
  `trust_base` INT UNSIGNED NOT NULL DEFAULT 100,
  `trust_per_level` INT UNSIGNED NOT NULL DEFAULT 0,
  `drowse_elite` INT UNSIGNED NOT NULL DEFAULT 2500 COMMENT 'rank multipliers, permil',
  `drowse_rare` INT UNSIGNED NOT NULL DEFAULT 4000,
  `drowse_rare_elite` INT UNSIGNED NOT NULL DEFAULT 6000,
  `trust_elite` INT UNSIGNED NOT NULL DEFAULT 2000,
  `trust_rare` INT UNSIGNED NOT NULL DEFAULT 3000,
  `trust_rare_elite` INT UNSIGNED NOT NULL DEFAULT 4000,
  `out_cold_ms` INT UNSIGNED NOT NULL DEFAULT 60000,
  `eat_every_ms` INT UNSIGNED NOT NULL DEFAULT 4000,
  `decay_after_ms` INT UNSIGNED NOT NULL DEFAULT 6000,
  `decay_permil` INT UNSIGNED NOT NULL DEFAULT 20 COMMENT 'of the Drowse max, a second',
  `draught_under_ms` INT UNSIGNED NOT NULL DEFAULT 15000,
  `sleep_cap_ms` INT UNSIGNED NOT NULL DEFAULT 120000,
  `above_level_permil` INT UNSIGNED NOT NULL DEFAULT 250 COMMENT 'an arrow or draught on a beast above its level',
  `band_step_permil` INT UNSIGNED NOT NULL DEFAULT 500 COMMENT 'food a band below the beast: x this per band',
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall taming: the bars and timers';

CREATE TABLE IF NOT EXISTS `ef_tame_family` (
  `family` INT UNSIGNED NOT NULL COMMENT 'CreatureFamily',
  `other_permil` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'food no like names (0 refuses)',
  PRIMARY KEY (`family`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall taming: the families';

CREATE TABLE IF NOT EXISTS `ef_tame_like` (
  `family` INT UNSIGNED NOT NULL,
  `ord` TINYINT UNSIGNED NOT NULL COMMENT 'the first like whose group holds the item counts',
  `group_name` VARCHAR(32) NOT NULL,
  `permil` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'a share of a normal beast''s Trust bar (0 refuses)',
  PRIMARY KEY (`family`, `ord`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall taming: what each family eats';

CREATE TABLE IF NOT EXISTS `ef_tame_arrow` (
  `item` INT UNSIGNED NOT NULL,
  `drowse` INT UNSIGNED NOT NULL DEFAULT 0,
  `max_level` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `skill` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`item`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall taming: the drowse arrows';

CREATE TABLE IF NOT EXISTS `ef_tame_draught` (
  `item` INT UNSIGNED NOT NULL,
  `sleep_ms` INT UNSIGNED NOT NULL DEFAULT 0,
  `max_level` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`item`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall taming: the drowse draughts';
