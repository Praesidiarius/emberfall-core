-- Emberfall talents (docs/design/talents.md §3.7, 2026-11-20, the Corsair's rework): a node's `carries`. When the login
-- re-validates a stored loadout under new talent data, the rank it holds on `from_node` (a retired node, or one granted
-- now and so never stored) moves onto `to_node`, unless the loadout holds that one already. Schema only; Emberfall's
-- compiler writes the rows (talents/sql.py), which empties and refills the table. Optional: the module reads it when it
-- exists.
CREATE TABLE IF NOT EXISTS `ef_talent_carry` (
  `from_node` INT UNSIGNED NOT NULL COMMENT 'the node a stored rank was on',
  `to_node` INT UNSIGNED NOT NULL COMMENT 'the node it moves onto',
  PRIMARY KEY (`from_node`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall talents: stored ranks that move to another node at a data change';
