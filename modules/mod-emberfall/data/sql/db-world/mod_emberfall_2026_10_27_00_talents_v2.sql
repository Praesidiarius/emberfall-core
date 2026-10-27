-- Emberfall talents (docs/design/talents.md §5.3, 2026-10-27): the trees are v2 only (every class tree in the class
-- layout, every spec tree in rows). A node has at most one parent, the node before it in its row, which opens it once
-- full; free nodes per spec, several parents and edges that open with part of a parent's points are gone. So
-- ef_talent_gate (folded into each node's `gate`), ef_talent_edge (the node's `parent`), ef_talent_free (no free nodes)
-- and ef_talent_pick (the node's `grp`) go, and ef_talent_node is made anew without `row` and `col` (layout only).
-- Schema only; Emberfall's compiler writes the rows (talents/sql.py), which empties and refills every table.
DROP TABLE IF EXISTS `ef_talent_gate`, `ef_talent_edge`, `ef_talent_free`, `ef_talent_pick`;
DROP TABLE IF EXISTS `ef_talent_node`;

CREATE TABLE IF NOT EXISTS `ef_talent_node` (
  `id` INT UNSIGNED NOT NULL,
  `tree` SMALLINT UNSIGNED NOT NULL,
  `ord` SMALLINT UNSIGNED NOT NULL COMMENT 'wire order within the tree, 0..n-1',
  `kind` TINYINT UNSIGNED NOT NULL COMMENT '1 passive, 2 active, 3 choice',
  `max_rank` TINYINT UNSIGNED NOT NULL DEFAULT 1,
  `required_level` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `replaces` INT UNSIGNED NOT NULL DEFAULT 0,
  `gate` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'points spent in the tree first (the node''s own or its row gate, the higher)',
  `parent` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'the node before it in its row that opens it once full, 0 = none',
  `grp` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'pick group, 1-based in the tree, 0 = none',
  PRIMARY KEY (`id`),
  UNIQUE KEY `tree_ord` (`tree`, `ord`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall talents: nodes';
