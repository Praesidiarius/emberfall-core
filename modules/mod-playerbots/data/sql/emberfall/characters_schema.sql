-- Emberfall (the playerbots pilot, Emberfall docs/design/playerbots-pilot.md): the module's three name tables in the
-- characters database, empty (Emberfall mode takes its bot names from ef_bot_name; no stock guilds or arena teams). The
-- final schema of the module's characters/base and characters/updates, written idempotent: Emberfall's realms.sh
-- applies it at every release. Not in a folder dbimport reads (its name holds no "characters").
SET NAMES utf8mb4;

CREATE TABLE IF NOT EXISTS `playerbots_arena_team_names` (
  `name_id` mediumint(8) NOT NULL AUTO_INCREMENT UNIQUE,
  `name` varchar(24) NOT NULL UNIQUE,
  `name_koKR` varchar(24) NULL, `name_frFR` varchar(24) NULL, `name_deDE` varchar(24) NULL, `name_zhCN` varchar(24) NULL,
  `name_zhTW` varchar(24) NULL, `name_esES` varchar(24) NULL, `name_esMX` varchar(24) NULL, `name_ruRU` varchar(24) NULL,
  `type` TINYINT(3) NOT NULL,
  PRIMARY KEY (`name_id`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8 COMMENT='Playerbot arena team names';

CREATE TABLE IF NOT EXISTS `playerbots_guild_names` (
  `name_id` INT(11) NOT NULL AUTO_INCREMENT UNIQUE,
  `name` varchar(24) NOT NULL UNIQUE,
  `name_koKR` varchar(24) NULL, `name_frFR` varchar(24) NULL, `name_deDE` varchar(24) NULL, `name_zhCN` varchar(24) NULL,
  `name_zhTW` varchar(24) NULL, `name_esES` varchar(24) NULL, `name_esMX` varchar(24) NULL, `name_ruRU` varchar(24) NULL,
  PRIMARY KEY (`name_id`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8 COMMENT='Playerbot guild names';

CREATE TABLE IF NOT EXISTS `playerbots_names` (
  `name_id` INT(11) NOT NULL UNIQUE,
  `name` varchar(12) NOT NULL,
  `gender` tinyint(3) unsigned NOT NULL,
  PRIMARY KEY (`name_id`),
  UNIQUE INDEX `name` (`name`, `gender`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8 COMMENT='Playerbot RandomBot names';
