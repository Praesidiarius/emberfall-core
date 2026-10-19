-- mod-emberfall: world editor access (Emberfall docs/design/world-editor.md §3.2). One row per game account that may use
-- the editor on the build realm. Written by the account site (its Game module, with a grant of the build channel and
-- `toa editor:grant`); the site's MySQL user has INSERT, UPDATE, DELETE on this table only. Read by the module at login,
-- every Emberfall.Editor.RefreshSeconds and on `.efedit access <id>`. Empty (and unread) on every other realm.

CREATE TABLE IF NOT EXISTS `ef_editor_access` (
  `account_id` INT UNSIGNED NOT NULL COMMENT 'account.id of the game account (TO<site id>)',
  `level` TINYINT UNSIGNED NOT NULL DEFAULT 1 COMMENT '1 contributor, 2 reviewer, 3 maintainer',
  `frozen` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '1: editor closed, open sets reverted and not replayed',
  `licence_version` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `granted_by` VARCHAR(64) NOT NULL DEFAULT '',
  `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`account_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall world editor: who may edit (the site keeps it)';
