-- Emberfall talents (docs/design/talents.md §1.3 rule 7, 2026-10-26): pick groups. A pick node is chosen for free, as many
-- of its group as the group's slots are open; slot k opens once the tree's points reach its threshold: the points the
-- character has earned for the tree (its budget at the level, `earned` 1: the Wrathbringer's row abilities, one of a
-- group at a time in any order, at fixed levels) or the points spent in it (`earned` 0). Schema only; Emberfall's compiler writes the rows
-- (talents/sql.py). Both tables or neither: the module reads them only when both exist.
CREATE TABLE IF NOT EXISTS `ef_talent_pick` (
  `node` INT UNSIGNED NOT NULL,
  `grp` TINYINT UNSIGNED NOT NULL COMMENT 'the pick group, 1-based in the node''s tree',
  PRIMARY KEY (`node`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall talents: pick nodes and their group';

CREATE TABLE IF NOT EXISTS `ef_talent_slot` (
  `tree` SMALLINT UNSIGNED NOT NULL,
  `grp` TINYINT UNSIGNED NOT NULL COMMENT 'the pick group, 1-based in the tree',
  `slot` TINYINT UNSIGNED NOT NULL COMMENT '1-based',
  `points` SMALLINT UNSIGNED NOT NULL COMMENT 'the tree''s points that open the slot',
  `earned` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '1: points earned for the tree (its budget at the level), 0: points spent in it',
  PRIMARY KEY (`tree`, `grp`, `slot`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall talents: the slot thresholds of the pick groups';
