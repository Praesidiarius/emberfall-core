-- Emberfall talents WP T6 (docs/design/talents.md §1.3, §8 Decisions 2 and 6): per-node gates ("points spent in this
-- tree first"), parents that open a node with some of their points, and granted nodes that rank up by themselves.
-- Schema only; Emberfall's compiler writes the rows (talents/sql.py).
ALTER TABLE `ef_talent_node`
  ADD COLUMN `gate` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'points spent in the tree first (the node''s own or its row gate, the higher)';
ALTER TABLE `ef_talent_edge`
  ADD COLUMN `min_rank` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'points the parent needs to open the child, 0 = full';
CREATE TABLE IF NOT EXISTS `ef_talent_auto` (
  `node` INT UNSIGNED NOT NULL,
  `rank` TINYINT UNSIGNED NOT NULL,
  `at` TINYINT UNSIGNED NOT NULL COMMENT 'points spent in the tree',
  PRIMARY KEY (`node`, `rank`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall talents: granted nodes (rank at points spent)';
