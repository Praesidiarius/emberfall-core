-- mod-emberfall: the profession rework (Emberfall docs/design/professions.md §28): the crafts' specialisations at the
-- last rank and the names the trainer's lines use in the client's language. Schema only; the rows come from Emberfall's
-- content compiler (dist/server/world.sql, profspecs/sql.py), which empties and refills both tables. The talents'
-- per-profession budget (points from rank 3) is ef_prof_meta rows (`points_from:<skill>`, `points_per_skill:<skill>`),
-- the unlearn price ladder `spec_unlearn_copper`.

CREATE TABLE IF NOT EXISTS `ef_prof_specialisation` (
  `spell` INT UNSIGNED NOT NULL COMMENT 'the passive spell the pick teaches; every last-rank recipe of it asks its aura (Spell.dbc CasterAuraSpell)',
  `skill_line` SMALLINT UNSIGNED NOT NULL,
  `sort` TINYINT UNSIGNED NOT NULL DEFAULT 1,
  `back_to` SMALLINT UNSIGNED NOT NULL COMMENT 'the skill an unlearn sets the craft back to: the last rank''s start',
  `needs_cap` SMALLINT UNSIGNED NOT NULL COMMENT 'the craft''s cap a pick needs: the last rank learned',
  `key` VARCHAR(32) NOT NULL DEFAULT '',
  PRIMARY KEY (`spell`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall professions: the crafts'' specialisations';

CREATE TABLE IF NOT EXISTS `ef_prof_text` (
  `kind` TINYINT UNSIGNED NOT NULL COMMENT '1 a talent tree''s name, 2 a specialisation''s name, 3 what a specialisation makes',
  `id` INT UNSIGNED NOT NULL COMMENT 'the tree''s ID or the specialisation''s spell',
  `locale` VARCHAR(4) NOT NULL COMMENT 'enUS, deDE, frFR',
  `text` VARCHAR(1024) NOT NULL DEFAULT '',
  PRIMARY KEY (`kind`, `id`, `locale`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall professions: the trainer lines'' names by language';
