-- Emberfall talents (docs/design/talents.md §1.3 rule 9, 2026-11-15, feedback #584: Solenne's Mercy only for a paladin
-- who chose Sunmend): a node may need a node of its class tree or of its own tree held, or one option of a choice node.
-- `req_node` the node it needs (0 = none), `req_option` that choice node's option (1-4; 0 = the node at any rank).
-- Schema only; Emberfall's compiler writes the rows (talents/sql.py), which empties and refills the table.
ALTER TABLE `ef_talent_node` ADD COLUMN `req_node` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'the node it needs held, 0 = none';
ALTER TABLE `ef_talent_node` ADD COLUMN `req_option` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'that choice node''s option, 0 = any rank';
