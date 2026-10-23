-- mod-emberfall: a class kit's own power (Emberfall docs/design/classes/hunter-marksman.md: the Hunter's Focus, which the
-- core never regenerates for a player). Schema only: Emberfall's compiler writes the rows (world.sql).
-- src/classkit/EfClassKit.h (ClassPowerRule) says what the module does with them.
CREATE TABLE IF NOT EXISTS `ef_class_kit_class_power` (
  `class` TINYINT UNSIGNED NOT NULL COMMENT 'engine class ID',
  `power` TINYINT UNSIGNED NOT NULL COMMENT 'the power slot (Powers: 2 focus) the module runs for the class',
  `max_power` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'the maximum before max-power auras',
  `regen_per_second` FLOAT NOT NULL DEFAULT 0 COMMENT 'points a second in a fight (times haste and the regen-percent auras)',
  `regen_out_of_combat` FLOAT NOT NULL DEFAULT 0 COMMENT 'points a second out of a fight',
  PRIMARY KEY (`class`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall class kits: a class''s own power';
