-- mod-emberfall: a class kit spec's own power (Emberfall docs/design/classes/druid-stormbringer.md §3: the Stormbringer's
-- Static). Schema only: Emberfall's compiler writes the rows (world.sql). src/classkit/EfClassKit.h (SpecPower) says
-- what the module does with them.
CREATE TABLE IF NOT EXISTS `ef_class_kit_spec_power` (
  `spec` SMALLINT UNSIGNED NOT NULL COMMENT 'ef_spec id',
  `class` TINYINT UNSIGNED NOT NULL COMMENT 'engine class ID',
  `power` TINYINT UNSIGNED NOT NULL COMMENT 'the power slot (Powers: 6 runic power) the spec uses instead of the class''s',
  `max_power` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'stored units (runic power x 10)',
  `drain_delay_ms` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'out of combat this long before it drains',
  `drain_per_second` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'stored units drained a second out of combat',
  PRIMARY KEY (`spec`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall class kits: a spec''s own power';
