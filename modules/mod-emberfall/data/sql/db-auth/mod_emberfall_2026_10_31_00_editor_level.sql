-- mod-emberfall: the world editor on the normal realm (Emberfall docs/design/world-editor.md §19). The editor's role is
-- the account's GM level for the realm (account_access): 1 (SEC_MODERATOR) the contributor, 2 and up the maintainer.
-- Level 1 must play exactly as a player everywhere outside the editor:
--
-- 1. Its RBAC role is the player's (195, "Role: Sec Level Player") instead of the moderator's (194: instant logout,
--    skip queue, no realm firsts, GM trade log, staff badge, GM list, see all security levels, ignore lower security,
--    two-side chat/mail/trade/who, the moderator commands' role 198, ...). Levels 2 and 3 keep 193 and 192 (which link
--    194 themselves).
-- 2. ef_editor_access (phase A's table, now only the site's freeze and licence record) gives its accounts the GM level:
--    every editor level 1; phase A's maintainer (3) level 2. Phase A's reviewers (2) become contributors (1): GM level
--    2 has GM commands, so a maintainer is made by hand (`account set gmlevel <name> 2 <realm id>`). RealmID -1: every
--    realm's auth database is its own (tools/gameserver). An account that already has a GM level keeps it.
--
-- The GM commands at level 1: data/sql/db-world/mod_emberfall_2026_10_31_00_editor_command_security.sql.

DELETE FROM `rbac_default_permissions` WHERE `secId` = 1;
INSERT INTO `rbac_default_permissions` (`secId`, `permissionId`, `realmId`) VALUES (1, 195, -1);

CREATE TABLE IF NOT EXISTS `ef_editor_access` (
  `account_id` INT UNSIGNED NOT NULL COMMENT 'account.id of the game account (TO<site id>)',
  `level` TINYINT UNSIGNED NOT NULL DEFAULT 1 COMMENT 'the site''s record (1 contributor, 2 reviewer, 3 maintainer); the role is account_access',
  `frozen` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '1: editor closed, open sets reverted and not replayed',
  `licence_version` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `granted_by` VARCHAR(64) NOT NULL DEFAULT '',
  `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`account_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall world editor: who may edit (the site keeps it)';

INSERT INTO `account_access` (`id`, `gmlevel`, `RealmID`, `comment`)
SELECT e.`account_id`, IF(e.`level` >= 3, 2, 1), -1, 'world editor (ef_editor_access)'
FROM `ef_editor_access` e
JOIN `account` a ON a.`id` = e.`account_id`
WHERE NOT EXISTS (SELECT 1 FROM `account_access` x WHERE x.`id` = e.`account_id`);
