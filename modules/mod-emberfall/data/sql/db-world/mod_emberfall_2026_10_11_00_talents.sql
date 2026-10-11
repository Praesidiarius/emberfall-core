-- mod-emberfall: the spec and talent tree tables (Emberfall docs/design/talents.md §2.6, WP T3).
-- Schema only; the rows come from Emberfall's content compiler (dist/server/world.sql, or
-- dist/test-talents/talents.sql for the generated stock test trees), which empties and refills every
-- table. Enumerations: kind 1 passive / 2 active / 3 choice; role 1 tank / 2 healer / 3 damage.

CREATE TABLE IF NOT EXISTS `ef_talent_meta` (
  `name` VARCHAR(32) NOT NULL COMMENT 'data_hash, schema, spec_level, max_rank, max_loadouts, switch_cast_ms, switch_spell',
  `value` VARCHAR(64) NOT NULL DEFAULT '',
  PRIMARY KEY (`name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall talents: global values';

CREATE TABLE IF NOT EXISTS `ef_talent_points` (
  `class` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 = the default schedule',
  `level` TINYINT UNSIGNED NOT NULL,
  `class_points` SMALLINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'cumulative budget at this level',
  `spec_points` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`class`, `level`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall talents: point schedule';

CREATE TABLE IF NOT EXISTS `ef_spec` (
  `id` SMALLINT UNSIGNED NOT NULL,
  `class` TINYINT UNSIGNED NOT NULL,
  `spec_key` VARCHAR(32) NOT NULL DEFAULT '',
  `role` TINYINT UNSIGNED NOT NULL DEFAULT 3,
  `sort` TINYINT UNSIGNED NOT NULL DEFAULT 1 COMMENT 'order within the class; free_spec_mask bit sort - 1',
  `available_level` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `skill_line` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`),
  KEY `class` (`class`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall talents: specializations';

CREATE TABLE IF NOT EXISTS `ef_spec_spell` (
  `spec` SMALLINT UNSIGNED NOT NULL,
  `spell` INT UNSIGNED NOT NULL,
  `level` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`spec`, `spell`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall talents: baseline spec spells';

CREATE TABLE IF NOT EXISTS `ef_talent_tree` (
  `id` SMALLINT UNSIGNED NOT NULL,
  `class` TINYINT UNSIGNED NOT NULL,
  `spec` SMALLINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 = the class tree',
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall talents: trees';

CREATE TABLE IF NOT EXISTS `ef_talent_gate` (
  `tree` SMALLINT UNSIGNED NOT NULL,
  `row` TINYINT UNSIGNED NOT NULL COMMENT 'nodes in rows >= row need points spent on rows < row',
  `points` SMALLINT UNSIGNED NOT NULL,
  PRIMARY KEY (`tree`, `row`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall talents: gates';

CREATE TABLE IF NOT EXISTS `ef_talent_node` (
  `id` INT UNSIGNED NOT NULL,
  `tree` SMALLINT UNSIGNED NOT NULL,
  `ord` SMALLINT UNSIGNED NOT NULL COMMENT 'wire order within the tree, 0..n-1',
  `row` TINYINT UNSIGNED NOT NULL,
  `col` FLOAT NOT NULL DEFAULT 1,
  `kind` TINYINT UNSIGNED NOT NULL COMMENT '1 passive, 2 active, 3 choice',
  `max_rank` TINYINT UNSIGNED NOT NULL DEFAULT 1,
  `required_level` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `replaces` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`),
  UNIQUE KEY `tree_ord` (`tree`, `ord`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall talents: nodes';

CREATE TABLE IF NOT EXISTS `ef_talent_node_spell` (
  `node` INT UNSIGNED NOT NULL,
  `option` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 for a normal node, 1-3 for a choice option',
  `rank` TINYINT UNSIGNED NOT NULL DEFAULT 1 COMMENT '1-9',
  `spell` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`node`, `option`, `rank`, `spell`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall talents: the spells of each rank or option';

CREATE TABLE IF NOT EXISTS `ef_talent_edge` (
  `parent` INT UNSIGNED NOT NULL,
  `child` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`parent`, `child`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall talents: edges (parent row above child row)';

CREATE TABLE IF NOT EXISTS `ef_talent_free` (
  `node` INT UNSIGNED NOT NULL,
  `spec` SMALLINT UNSIGNED NOT NULL,
  PRIMARY KEY (`node`, `spec`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall talents: class-tree nodes free for a spec';

-- Help texts of the module's GM commands (src/emberfall_talents.cpp).
DELETE FROM `command` WHERE `name` IN ('ef', 'ef talents', 'ef talents dump', 'ef talents reset', 'ef talents reload', 'ef spec', 'ef send');
INSERT INTO `command` (`name`, `security`, `help`) VALUES
('ef talents dump', 2, 'Syntax: .ef talents dump\r\nEmberfall talents: the selected player''s (or your) spec, loadouts and granted spells.'),
('ef talents reset', 2, 'Syntax: .ef talents reset\r\nEmberfall talents: reset the active loadout of the selected player''s (or your) active spec.'),
('ef talents reload', 3, 'Syntax: .ef talents reload\r\nEmberfall talents: reload the ef_* world tables.'),
('ef spec', 2, 'Syntax: .ef spec #specId\r\nEmberfall talents: make #specId the selected player''s (or your) active spec at once (no switch cast).'),
('ef send', 2, 'Syntax: .ef send $body\r\nEmberfall talents: handle $body as an EFT addon message from your own client (test aid).');
