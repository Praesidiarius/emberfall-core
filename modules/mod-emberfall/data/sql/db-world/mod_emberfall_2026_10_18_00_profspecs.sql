-- mod-emberfall: the profession specialisation tables (Emberfall docs/design/professions.md §10.3).
-- Schema only; the rows come from Emberfall's content compiler (dist/server/world.sql, profspecs/sql.py), which empties
-- and refills every table. Effects (ef_prof_node.effect): 0 none, 1 yield, 2 minimap_range, 3 rarest_first,
-- 4 gather_speed, 5 craft_extra, 6 craft_batch, 7 duration, 8 discovery. Families: 1 potion, 2 tincture, 3 steeping,
-- 4 solution, 5 ingot.

CREATE TABLE IF NOT EXISTS `ef_prof_meta` (
  `name` VARCHAR(32) NOT NULL COMMENT 'data_hash, points_per_skill, reset_fee_per_point',
  `value` VARCHAR(64) NOT NULL DEFAULT '',
  PRIMARY KEY (`name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall profession specialisations: global values';

CREATE TABLE IF NOT EXISTS `ef_prof_tree` (
  `id` SMALLINT UNSIGNED NOT NULL,
  `skill_line` SMALLINT UNSIGNED NOT NULL,
  `sort` TINYINT UNSIGNED NOT NULL DEFAULT 1 COMMENT '1-3 within the profession: the order of the rank strings',
  `name` VARCHAR(64) NOT NULL DEFAULT '' COMMENT 'for the trainer''s reset line',
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall profession specialisations: trees';

CREATE TABLE IF NOT EXISTS `ef_prof_node` (
  `id` SMALLINT UNSIGNED NOT NULL,
  `tree` SMALLINT UNSIGNED NOT NULL,
  `ord` TINYINT UNSIGNED NOT NULL COMMENT 'the node''s digit in its tree''s rank string',
  `row` TINYINT UNSIGNED NOT NULL,
  `max_rank` TINYINT UNSIGNED NOT NULL DEFAULT 1,
  `gate` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'points spent in the tree on the rows above first',
  `effect` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `per_rank` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `extra` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `active` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '1: the effect works with today''s content',
  PRIMARY KEY (`id`),
  KEY `tree` (`tree`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall profession specialisations: nodes';

CREATE TABLE IF NOT EXISTS `ef_prof_node_spell` (
  `node` SMALLINT UNSIGNED NOT NULL,
  `rank` TINYINT UNSIGNED NOT NULL,
  `spell` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`node`, `rank`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall profession specialisations: rank spells';

CREATE TABLE IF NOT EXISTS `ef_prof_edge` (
  `parent` SMALLINT UNSIGNED NOT NULL,
  `child` SMALLINT UNSIGNED NOT NULL,
  PRIMARY KEY (`parent`, `child`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall profession specialisations: a full parent opens its child';

CREATE TABLE IF NOT EXISTS `ef_prof_node_target` (
  `node` SMALLINT UNSIGNED NOT NULL,
  `kind` TINYINT UNSIGNED NOT NULL COMMENT '1 item, 2 family, 3 spell',
  `value` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`node`, `kind`, `value`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall profession specialisations: what an effect applies to';

CREATE TABLE IF NOT EXISTS `ef_prof_recipe` (
  `spell` INT UNSIGNED NOT NULL,
  `skill_line` SMALLINT UNSIGNED NOT NULL,
  `family` TINYINT UNSIGNED NOT NULL,
  `item` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`spell`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall profession specialisations: recipe families';

CREATE TABLE IF NOT EXISTS `ef_prof_trainer` (
  `creature` INT UNSIGNED NOT NULL,
  `skill_line` SMALLINT UNSIGNED NOT NULL,
  PRIMARY KEY (`creature`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall profession specialisations: trainers that reset trees';

DELETE FROM `command` WHERE `name` IN ('efprof', 'efprof dump', 'efprof reset', 'efprof reload', 'efprof send');
INSERT INTO `command` (`name`, `security`, `help`) VALUES
('efprof', 2, 'Syntax: .efprof $subcommand\r\nEmberfall profession specialisations: dump, reset, reload, send.'),
('efprof dump', 2, 'Syntax: .efprof dump\r\nThe selected player''s (or your) profession specialisation points, ranks, effects and rank spells.'),
('efprof reset', 2, 'Syntax: .efprof reset [#skill]\r\nResets the selected player''s specialisations (one profession or all), free of charge.'),
('efprof reload', 3, 'Syntax: .efprof reload\r\nReloads the ef_prof_* tables.'),
('efprof send', 2, 'Syntax: .efprof send $body\r\nHandles an EFP body as if your own client had sent it (a test aid).');
