-- Emberfall: account-wide achievements (src/emberfall_account_achievements.cpp). The achievements with Emberfall's
-- account flag (Achievement.dbc flags 0x8000) an account has earned with any of its characters; kept when that
-- character is deleted, granted to every character of the account.
CREATE TABLE IF NOT EXISTS `ef_account_achievement` (
    `account` INT UNSIGNED NOT NULL COMMENT 'account.id (auth)',
    `achievement` SMALLINT UNSIGNED NOT NULL COMMENT 'Achievement.dbc',
    `date` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'unix time it was first earned',
    PRIMARY KEY (`account`, `achievement`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall: account-wide achievements';

-- Emberfall's first achievements (levels, exploration): every character checks all its achievement criteria once at
-- its next login (AT_LOGIN_CHECK_ACHIEVS, 0x400), so the levels reached and the areas explored before count.
UPDATE `characters` SET `at_login` = `at_login` | 0x400;
