-- mod-emberfall: hunter taming for every tameable beast (src/emberfall_taming.cpp; Emberfall
-- docs/design/hunter-taming.md §15). Each creature's beast family (wolves, bears, herons ...: content/data/taming/
-- families.yaml), which ef_tame_family and ef_tame_like are keyed by since 2026-11-06 (101 and up, never a
-- CreatureFamily). The schema only: Emberfall's compiler writes the rows.

CREATE TABLE IF NOT EXISTS `ef_tame_beast` (
  `entry` INT UNSIGNED NOT NULL COMMENT 'creature_template',
  `family` INT UNSIGNED NOT NULL COMMENT 'ef_tame_family',
  PRIMARY KEY (`entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall taming: each beast''s family (its tastes)';

ALTER TABLE `ef_tame_family` MODIFY `family` INT UNSIGNED NOT NULL COMMENT 'the beast family (ef_tame_beast)';
