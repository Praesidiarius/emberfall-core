-- mod-emberfall: account friends (Emberfall docs/design/account-friends.md). In the auth database because a friend is an
-- account, the same on every realm (character_social, per character, stays in each realm's characters database).
-- Written and read only by the worldserver (src/emberfall_accountfriends.cpp).

-- One row per accepted pair, account_lo < account_hi; each side's own name for the other.
CREATE TABLE IF NOT EXISTS `ef_account_friend` (
  `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `account_lo` INT UNSIGNED NOT NULL COMMENT 'account.id, the smaller of the two',
  `account_hi` INT UNSIGNED NOT NULL COMMENT 'account.id, the larger of the two',
  `label_lo` VARCHAR(96) NOT NULL DEFAULT '' COMMENT 'what account_lo calls account_hi (the character added through, or a note)',
  `label_hi` VARCHAR(96) NOT NULL DEFAULT '' COMMENT 'what account_hi calls account_lo',
  `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  UNIQUE KEY `pair` (`account_lo`, `account_hi`),
  KEY `hi` (`account_hi`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall account friends: accepted pairs';

-- A request waiting for its answer; goes when answered, withdrawn, or after 30 days (at worldserver start).
CREATE TABLE IF NOT EXISTS `ef_account_friend_request` (
  `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `from_account` INT UNSIGNED NOT NULL,
  `to_account` INT UNSIGNED NOT NULL,
  `from_name` VARCHAR(48) NOT NULL DEFAULT '' COMMENT 'the character it was sent from (what the receiver sees)',
  `to_name` VARCHAR(48) NOT NULL DEFAULT '' COMMENT 'the character it was sent to (what the sender typed)',
  `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  UNIQUE KEY `pair` (`from_account`, `to_account`),
  KEY `to_account` (`to_account`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall account friends: open requests';

-- When an account's last character left the world (an account friend's "last seen").
CREATE TABLE IF NOT EXISTS `ef_account_seen` (
  `account_id` INT UNSIGNED NOT NULL,
  `seen_at` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'unix time of the last logout',
  PRIMARY KEY (`account_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall account friends: last seen';
