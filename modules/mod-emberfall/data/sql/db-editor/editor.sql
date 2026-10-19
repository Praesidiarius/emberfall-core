-- mod-emberfall: the world editor's own database (Emberfall docs/design/world-editor.md §5.4), `build_editor` on the
-- build realm (`Emberfall.Editor.Database`). Not a dbimport database: tools/gameserver/realms.sh (and
-- tools/server/ef_server.sh editor, locally) creates it and applies this file, which is idempotent. `load-world` never
-- touches it. The module writes every table; the account site reads them and writes only the review columns of
-- ef_changeset (`state`, `mr_*`, `merge_sha`, `thread_id`), ef_op.final_id and ef_audit rows of its own.

CREATE TABLE IF NOT EXISTS `ef_changeset` (
  `id` INT UNSIGNED NOT NULL COMMENT 'cs-<id> (4 digits); allocated by the module',
  `owner` INT UNSIGNED NOT NULL COMMENT 'auth account.id',
  `owner_name` VARCHAR(32) NOT NULL DEFAULT '' COMMENT 'the character that opened it',
  `title` VARCHAR(80) NOT NULL,
  `description` TEXT NOT NULL,
  `zone` VARCHAR(32) NOT NULL COMMENT 'editor.yaml zone key',
  `state` VARCHAR(20) NOT NULL DEFAULT 'draft' COMMENT 'draft submitted in_review changes_requested accepted released rejected withdrawn conflict',
  `base_release` VARCHAR(32) NULL,
  `created_at` DATETIME NOT NULL,
  `submitted_at` DATETIME NULL,
  `mr_iid` INT UNSIGNED NULL,
  `mr_url` VARCHAR(255) NULL,
  `merge_sha` VARCHAR(64) NULL,
  `thread_id` VARCHAR(32) NULL,
  `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  KEY `state` (`state`),
  KEY `owner` (`owner`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall world editor: change sets';

CREATE TABLE IF NOT EXISTS `ef_op` (
  `changeset_id` INT UNSIGNED NOT NULL,
  `seq` INT UNSIGNED NOT NULL,
  `op` VARCHAR(8) NOT NULL COMMENT 'add move set delete',
  `kind` CHAR(1) NOT NULL COMMENT 'c creature spawn, n NPC',
  `target_id` INT UNSIGNED NOT NULL COMMENT 'stable id (GUID, NPC id) or the temporary id of an add',
  `temp_id` INT UNSIGNED NULL COMMENT 'an add: the realm''s temporary id (9000001+)',
  `final_id` INT UNSIGNED NULL COMMENT 'an add: the GUID the bot gave it in the merge request',
  `from_json` TEXT NOT NULL,
  `to_json` TEXT NOT NULL,
  `created_at` DATETIME NOT NULL,
  `undone` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`changeset_id`, `seq`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall world editor: operations';

CREATE TABLE IF NOT EXISTS `ef_lock` (
  `kind` CHAR(1) NOT NULL,
  `target_id` INT UNSIGNED NOT NULL,
  `changeset_id` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`kind`, `target_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall world editor: one open set per object';

CREATE TABLE IF NOT EXISTS `ef_audit` (
  `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `at` DATETIME NOT NULL,
  `account` INT UNSIGNED NOT NULL,
  `character_guid` INT UNSIGNED NOT NULL DEFAULT 0,
  `action` VARCHAR(16) NOT NULL,
  `result` VARCHAR(16) NOT NULL,
  `detail_json` TEXT NOT NULL,
  PRIMARY KEY (`id`),
  KEY `account_at` (`account`, `at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall world editor: every request, accepted or refused';
