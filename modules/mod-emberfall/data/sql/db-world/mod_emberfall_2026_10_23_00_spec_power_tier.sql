-- mod-emberfall: the tiers of a class kit spec's own power (Emberfall docs/design/classes/mage-pyromancer.md: the
-- Pyromancer's Heat, Blazing at 30, Inferno at 70, Maximum at 100). Schema only: Emberfall's compiler writes the rows
-- (world.sql). src/classkit/EfClassKit.h (SpecPower::Tier) says what the module does with them.
CREATE TABLE IF NOT EXISTS `ef_class_kit_spec_power_tier` (
  `spec` SMALLINT UNSIGNED NOT NULL COMMENT 'ef_spec id (a row of ef_class_kit_spec_power)',
  `at_power` INT UNSIGNED NOT NULL COMMENT 'stored units (runic power x 10) from which the tier holds',
  `aura` INT UNSIGNED NOT NULL COMMENT 'the aura the player carries while the tier holds',
  `requires_spell` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'a spell the player must know for the tier (0: none)',
  PRIMARY KEY (`spec`, `aura`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall class kits: the tiers of a spec''s own power';
