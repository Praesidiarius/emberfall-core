-- mod-emberfall: a player's melee auto attack by level (Emberfall docs/design/classes.md §4.7, feedback #168: below 40 the
-- white swing made the melee specs kill twice as fast as the ranged ones). Schema only: Emberfall's compiler writes the
-- rows (world.sql, from formulas.yaml auto_attack). src/classkit/EfClassKit.h (AutoAttackPct) says what the module does.
CREATE TABLE IF NOT EXISTS `ef_class_kit_auto_attack` (
  `level` TINYINT UNSIGNED NOT NULL COMMENT 'the player''s level',
  `pct` FLOAT NOT NULL DEFAULT 100 COMMENT 'percent of its damage a kit player''s white swing (main and off hand) deals',
  PRIMARY KEY (`level`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall class kits: a player''s melee auto attack by level';
