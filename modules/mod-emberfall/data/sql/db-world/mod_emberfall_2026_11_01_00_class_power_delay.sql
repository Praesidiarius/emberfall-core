-- mod-emberfall: a class power's out-of-combat regeneration starts this long after the fight (Emberfall feedback #294:
-- the Hunter's Focus, 5 s; the fight's rate until then). Schema only: Emberfall's compiler writes the rows (world.sql).
ALTER TABLE `ef_class_kit_class_power`
  ADD COLUMN `regen_out_of_combat_delay_ms` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'the fight''s rate goes on so long after the fight';
