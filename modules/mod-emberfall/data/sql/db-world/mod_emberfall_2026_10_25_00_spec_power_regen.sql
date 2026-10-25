-- mod-emberfall: a class kit spec power's regeneration, start and fight markers' delay (Emberfall
-- docs/design/classes/warrior-corsair.md §3: the Corsair's Energy, 10 a second in and out of a fight, full at login and
-- the spec choice; Flow's markers go 10 s after the fight). Schema only: Emberfall's compiler writes the rows (world.sql).
-- src/classkit/EfClassKit.h (SpecPower) says what the module does with them.
CREATE TABLE IF NOT EXISTS `ef_class_kit_spec_power_regen` (
  `spec` SMALLINT UNSIGNED NOT NULL COMMENT 'ef_spec id (a row of ef_class_kit_spec_power)',
  `regen_per_second` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'stored units (runic power x 10) a second, in and out of a fight, times the slot''s regen-percent auras',
  `start_full` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '1: full at login and the spec choice (0: empty)',
  `markers_delay_ms` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'out of combat this long before the kit''s fight markers go (0: when the fight ends)',
  PRIMARY KEY (`spec`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall class kits: a spec power''s regeneration';
