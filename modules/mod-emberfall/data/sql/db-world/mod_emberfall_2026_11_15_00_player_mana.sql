-- mod-emberfall: the stat rules (Emberfall docs/design/classes.md §4.10, Nathanael 2026-11-15; the core's PATCHES.md #40):
-- a player's mana pool is a figure of its class and level, never of Intellect or gear, and its mana comes back at a flat
-- rate a second, in and out of a fight. Schema only: Emberfall's compiler writes the rows (world.sql, from formulas.yaml
-- player_stats.mana_pool and regen.mana_pct_of_pool). src/emberfall_stat_rules.cpp says what the module does.
CREATE TABLE IF NOT EXISTS `ef_player_mana` (
  `class` TINYINT UNSIGNED NOT NULL COMMENT 'the player''s class',
  `level` TINYINT UNSIGNED NOT NULL COMMENT 'the player''s level',
  `pool` FLOAT NOT NULL COMMENT 'the mana pool before talents (in place of create mana and Intellect''s bonus)',
  `regen` FLOAT NOT NULL DEFAULT 0 COMMENT 'mana a second, in and out of a fight, casting or not',
  PRIMARY KEY (`class`, `level`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall stat rules: the mana pool and its regen by class and level';
