SET NAMES utf8mb4;
-- base/ai_playerbot_texts.sql (schema only)
DROP TABLE IF EXISTS `ai_playerbot_texts`;
CREATE TABLE IF NOT EXISTS `ai_playerbot_texts` (
  `id` smallint(20) NOT NULL AUTO_INCREMENT,
  `name` varchar(255) NOT NULL COMMENT 'name - used in strategies/code as filter',
  `text` varchar(1024) NOT NULL COMMENT 'text',
  `say_type` tinyint(3) NOT NULL DEFAULT '0' COMMENT '0 - say, 1 - yell',
  `reply_type` tinyint(3) NOT NULL DEFAULT '0' COMMENT 'if > 0 then can be filtered as a response to chat',
  `text_loc1` varchar(1024) NOT NULL DEFAULT '',
  `text_loc2` varchar(1024) NOT NULL DEFAULT '',
  `text_loc3` varchar(1024) NOT NULL DEFAULT '',
  `text_loc4` varchar(1024) NOT NULL DEFAULT '',
  `text_loc5` varchar(1024) NOT NULL DEFAULT '',
  `text_loc6` varchar(1024) NOT NULL DEFAULT '',
  `text_loc7` varchar(1024) NOT NULL DEFAULT '',
  `text_loc8` varchar(1024) NOT NULL DEFAULT '',
  PRIMARY KEY (`id`)
) ENGINE=InnoDB AUTO_INCREMENT=1 DEFAULT CHARSET=utf8 ROW_FORMAT=DYNAMIC;

-- base/ai_playerbot_texts_chance.sql (schema only)
DROP TABLE IF EXISTS `ai_playerbot_texts_chance`;
CREATE TABLE IF NOT EXISTS `ai_playerbot_texts_chance` (
  `id` bigint(20) NOT NULL AUTO_INCREMENT,
  `name` varchar(255) NOT NULL,
  `probability` bigint(20) NOT NULL,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB AUTO_INCREMENT=4 DEFAULT CHARSET=UTF8;

-- base/playerbots_account_keys.sql
DROP TABLE IF EXISTS `playerbots_account_keys`;
CREATE TABLE `playerbots_account_keys` (
    `account_id` INT PRIMARY KEY,
    `security_key` VARCHAR(255) NOT NULL,
    `created_at` TIMESTAMP DEFAULT CURRENT_TIMESTAMP
) ENGINE=INNODB DEFAULT CHARSET=latin1;

-- base/playerbots_account_links.sql
DROP TABLE IF EXISTS `playerbots_account_links`;
CREATE TABLE `playerbots_account_links` (
    `id` INT AUTO_INCREMENT PRIMARY KEY,
    `account_id` INT NOT NULL,
    `linked_account_id` INT NOT NULL,
    `created_at` TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    UNIQUE KEY `account_link` (`account_id`, `linked_account_id`)
) ENGINE=INNODB DEFAULT CHARSET=latin1;

-- base/playerbots_account_type.sql
DROP TABLE IF EXISTS `playerbots_account_type`;
CREATE TABLE `playerbots_account_type` (
    `account_id` int unsigned NOT NULL,
    `account_type` tinyint unsigned NOT NULL DEFAULT 0 COMMENT '0 = unassigned, 1 = RNDbot, 2 = AddClass',
    `assignment_date` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`account_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Playerbot account type assignments';

-- base/playerbots_custom_strategy.sql (schema only)
DROP TABLE IF EXISTS `playerbots_custom_strategy`;
CREATE TABLE `playerbots_custom_strategy` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `name` varchar(255) NOT NULL,
  `idx` int(11),
  `owner` int(11),
  `action_line` varchar(1024) NOT NULL,
  PRIMARY KEY (`id`),
  KEY `name` (`name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- base/playerbots_db_store.sql
DROP TABLE IF EXISTS `playerbots_db_store`;
CREATE TABLE `playerbots_db_store` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `guid` int(11) NOT NULL,
  `key` varchar(32) NOT NULL,
  `value` varchar(255) DEFAULT NULL,
  PRIMARY KEY (`id`),
  KEY `guid` (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- base/playerbots_dungeon_suggestion_abbrevation.sql (schema only)
DROP TABLE IF EXISTS `playerbots_dungeon_suggestion_abbrevation`;
CREATE TABLE `playerbots_dungeon_suggestion_abbrevation` (
    `id`              TINYINT(3)   NOT NULL AUTO_INCREMENT UNIQUE,
    `definition_slug` VARCHAR(255) NOT NULL,
    `abbrevation`     VARCHAR(16)  NOT NULL,

    PRIMARY KEY (`id`),
    KEY `definition_slug` (`definition_slug`)
)
ENGINE=MyISAM
DEFAULT CHARSET=utf8
ROW_FORMAT=FIXED
COMMENT='Playerbot dungeon suggestion abbrevations';

-- base/playerbots_dungeon_suggestion_definition.sql (schema only)
DROP TABLE IF EXISTS `playerbots_dungeon_suggestion_definition`;
CREATE TABLE `playerbots_dungeon_suggestion_definition` (
    `id`         TINYINT(3)   NOT NULL AUTO_INCREMENT UNIQUE,
    `slug`       VARCHAR(255) NOT NULL,
    `name`       VARCHAR(255) NOT NULL,
    `expansion`  TINYINT(1)   NOT NULL,
    `difficulty` TINYINT(1)   NOT NULL,
    `min_level`  TINYINT(3),
    `max_level`  TINYINT(3),
    `comment`    VARCHAR(255),

    PRIMARY KEY (`id`)
)
ENGINE=MyISAM
DEFAULT CHARSET=utf8
ROW_FORMAT=FIXED
COMMENT='Playerbot dungeon suggestion definitions';

-- base/playerbots_dungeon_suggestion_strategy.sql (schema only)
DROP TABLE IF EXISTS `playerbots_dungeon_suggestion_strategy`;
CREATE TABLE `playerbots_dungeon_suggestion_strategy` (
    `id`              TINYINT(3)   NOT NULL AUTO_INCREMENT UNIQUE,
    `definition_slug` VARCHAR(255) NOT NULL,
    `strategy`        VARCHAR(255) NOT NULL,
    `difficulty`      TINYINT(1)   NOT NULL,

    PRIMARY KEY (`id`),
    KEY `definition_slug` (`definition_slug`)
)
ENGINE=MyISAM
DEFAULT CHARSET=utf8
ROW_FORMAT=FIXED
COMMENT='Playerbot dungeon suggestion strategies';

-- base/playerbots_enchants.sql
DROP TABLE IF EXISTS `playerbots_enchants`;
CREATE TABLE `playerbots_enchants` (
  `class` TINYINT(3) NOT NULL,
  `spec` TINYINT(3) NOT NULL,
  `spellid` INT(11) NOT NULL,
  `slotid` TINYINT(3) DEFAULT 1,
  `name` VARCHAR(255) NOT NULL COMMENT 'name of the enchant',
  PRIMARY KEY (`class`,`spec`,`spellid`,`slotid`)
) ENGINE=INNODB DEFAULT CHARSET=latin1;
DELETE FROM `playerbots_enchants`;
INSERT INTO `playerbots_enchants` (`class`, `spec`, `spellid`, `slotid`, `name`) VALUES
    (1, 10, 20034, 15, 'Crusader            '),
    (1, 10, 22779, 17, '30 Hit            '),
    (1, 10, 27927, 10, '4 All Stats            '),
    (1, 10, 27927, 11, '4 All Stats             '),
    (1, 10, 27960, 4, '6 All Stats  '),
    (1, 10, 27984, 16, 'Mongoose            '),
    (1, 10, 29483, 2, '26 Attackpower 14 Crit            '),
    (1, 10, 33996, 9, '26 Attackpower            '),
    (1, 10, 34002, 8, '24 Attackpower               '),
    (1, 10, 34004, 14, '12 Agility          '),
    (1, 10, 34007, 7, 'Minor Speed 6 Agility'),
    (1, 10, 35452, 0, '34 Attackpower 16 Hit            '),
    (1, 10, 35490, 6, '50 Attackpower 12 Crit            '),
    (1, 11, 20034, 15, 'Crusader            '),
    (1, 11, 20034, 16, 'Crusader            '),
    (1, 11, 22779, 17, '30 Hit            '),
    (1, 11, 27927, 10, '4 All Stats            '),
    (1, 11, 27927, 11, '4 All Stats            '),
    (1, 11, 27960, 4, '6 All Stats  '),
    (1, 11, 29483, 2, '26 Attackpower 14 Crit            '),
    (1, 11, 33996, 9, '26 Attackpower            '),
    (1, 11, 34002, 8, '24 Attackpower               '),
    (1, 11, 34004, 14, '12 Agility          '),
    (1, 11, 34007, 7, 'Minor Speed 6 Agility'),
    (1, 11, 35452, 0, '34 Attackpower 16 Hit            '),
    (1, 11, 35490, 6, '50 Attackpower 12 Crit            '),
    (1, 12, 22779, 17, '30 Hit            '),
    (1, 12, 25072, 9, '2% Threat            '),
    (1, 12, 27906, 8, '12 Defense               '),
    (1, 12, 27927, 10, '4 All Stats            '),
    (1, 12, 27927, 11, '4 All Stats            '),
    (1, 12, 27954, 7, '5% Root/Snare Resist 10 Hit'),
    (1, 12, 27960, 4, '6 All Stats  '),
    (1, 12, 28004, 15, 'Battlemaster            '),
    (1, 12, 34009, 16, '18 Stamina            '),
    (1, 12, 35433, 2, '10 Dodge 15 Defense            '),
    (1, 12, 35443, 0, '16 Defense 17 Dodge            '),
    (1, 12, 35495, 6, '40 Stamina 12 Agility            '),
    (1, 12, 47051, 14, '12 Defense          '),
    (2, 20, 22779, 17, '30 Hit            '),
    (2, 20, 27926, 10, '20 Healing 7 Spelldamage           '),
    (2, 20, 27926, 11, '20 Healing 7 Spelldamage            '),
    (2, 20, 27945, 16, '12 Intellect            '),
    (2, 20, 27954, 7, '5% Root/Snare Resist 10 Hit'),
    (2, 20, 27960, 4, '6 All Stats  '),
    (2, 20, 29475, 2, '31 Healing 11 Spelldamage 5 mp5            '),
    (2, 20, 31370, 6, '66 Healing 22 Spelldamage 20 Stamina            '),
    (2, 20, 33999, 9, '35 Healing 12 Spelldamage            '),
    (2, 20, 34001, 8, '12 Intellect               '),
    (2, 20, 34003, 14, '20 Spell Penetration          '),
    (2, 20, 34010, 15, '81 Healing 27 Spelldamage            '),
    (2, 20, 35445, 0, '35 Healing 12 Spelldamage 7 mp5            '),
    (2, 21, 22779, 17, '30 Hit            '),
    (2, 21, 25072, 9, '2% Threat            '),
    (2, 21, 27906, 8, '12 Defense               '),
    (2, 21, 27927, 10, '4 All Stats            '),
    (2, 21, 27927, 11, '4 All Stats            '),
    (2, 21, 27954, 7, '5% Root/Snare Resist 10 Hit'),
    (2, 21, 27960, 4, '6 All Stats  '),
    (2, 21, 28004, 15, 'Battlemaster            '),
    (2, 21, 34009, 16, '18 Stamina            '),
    (2, 21, 35433, 2, '10 Dodge 15 Defense            '),
    (2, 21, 35443, 0, '16 Defense 17 Dodge            '),
    (2, 21, 35495, 6, '40 Stamina 12 Agility            '),
    (2, 21, 47051, 14, '12 Defense          '),
    (2, 22, 20034, 15, 'Crusader            '),
    (2, 22, 22779, 17, '30 Hit            '),
    (2, 22, 27899, 8, '12 Strength               '),
    (2, 22, 27927, 10, '4 All Stats            '),
    (2, 22, 27927, 11, '4 All Stats            '),
    (2, 22, 27960, 4, '6 All Stats  '),
    (2, 22, 29483, 2, '26 Attackpower 14 Crit            '),
    (2, 22, 33995, 6, '50 Attackpower 12 Crit            '),
    (2, 22, 33996, 9, '15 Strength            '),
    (2, 22, 34004, 14, '12 Agility          '),
    (2, 22, 34007, 7, 'Minor Speed 6 Agility'),
    (2, 22, 37891, 0, '17 Strength16 Intellect            '),
    (3, 30, 22779, 17, '30 Hit            '),
    (3, 30, 25080, 9, '15 Agility            '),
    (3, 30, 27927, 10, '4 All Stats            '),
    (3, 30, 27927, 11, '4 All Stats             '),
    (3, 30, 27951, 7, '12 Agility'),
    (3, 30, 27960, 4, '6 All Stats  '),
    (3, 30, 29483, 2, '26 Attackpower 14 Crit            '),
    (3, 30, 34002, 8, '24 Attackpower               '),
    (3, 30, 34004, 14, '12 Agility          '),
    (3, 30, 35452, 0, '34 Attackpower 16 Hit            '),
    (3, 30, 35495, 6, '40 Stamina 12 Agility            '),
    (3, 30, 42620, 15, 'Greater Agility            '),
    (3, 30, 42620, 16, 'Greater Agility            '),
    (4, 40, 22779, 17, '30 Hit            '),
    (4, 40, 25080, 9, '15 Agility            '),
    (4, 40, 27927, 10, '4 All Stats            '),
    (4, 40, 27927, 11, '4 All Stats             '),
    (4, 40, 27951, 7, '12 Agility'),
    (4, 40, 27960, 4, '6 All Stats  '),
    (4, 40, 27984, 15, 'Mongoose            '),
    (4, 40, 27984, 16, 'Mongoose            '),
    (4, 40, 29483, 2, '26 Attackpower 14 Crit            '),
    (4, 40, 34002, 8, '24 Attackpower               '),
    (4, 40, 34004, 14, '12 Agility          '),
    (4, 40, 35452, 0, '34 Attackpower 16 Hit            '),
    (4, 40, 35495, 6, '40 Stamina 12 Agility            '),
    (5, 50, 22779, 17, '30 Hit   '),
    (5, 50, 27926, 10, '20 Healing 7 Spelldamage            '),
    (5, 50, 27926, 11, '20 Healing 7 Spelldamage            '),
    (5, 50, 27945, 16, '12 Intellect            '),
    (5, 50, 27954, 7, '5% Root/Snare Resist 10 Hit'),
    (5, 50, 27960, 4, '6 All Stats  '),
    (5, 50, 29475, 2, '31 Healing 11 Spelldamage 5 mp5            '),
    (5, 50, 31370, 6, '66 Healing 22 Spelldamage 20 Stamina            '),
    (5, 50, 33999, 9, '35 Healing 12 Spelldamage            '),
    (5, 50, 34001, 8, '12 Intellect               '),
    (5, 50, 34003, 14, '20 Spell Penetration          '),
    (5, 50, 34010, 15, '81 Healing 27 Spelldamage            '),
    (5, 50, 35445, 0, '35 Healing 12 Spelldamage 7 mp5            '),
    (7, 70, 22779, 17, '30 Hit            '),
    (7, 70, 27926, 10, '20 Healing 7 Spelldamage           '),
    (7, 70, 27926, 11, '20 Healing 7 Spelldamage            '),
    (7, 70, 27945, 16, '12 Intellect            '),
    (7, 70, 27954, 7, '5% Root/Snare Resist 10 Hit'),
    (7, 70, 27960, 4, '6 All Stats  '),
    (7, 70, 31370, 6, '66 Healing 22 Spelldamage 20 Stamina            '),
    (7, 70, 33994, 9, '15 Spell Hit            '),
    (7, 70, 34001, 8, '12 Intellect               '),
    (7, 70, 34003, 14, '20 Spell Penetration          '),
    (7, 70, 34010, 15, '81 Healing 27 Spelldamage            '),
    (7, 70, 35406, 2, '18 Spelldamage 10 Crit            '),
    (7, 70, 35447, 0, '22 Spelldamage 14 Hit            '),
    (7, 71, 22779, 17, '30 Hit            '),
    (7, 71, 25080, 9, '15 Agility            '),
    (7, 71, 27927, 10, '4 All Stats            '),
    (7, 71, 27927, 11, '4 All Stats            '),
    (7, 71, 27951, 7, '12 Agility'),
    (7, 71, 27960, 4, '6 All Stats  '),
    (7, 71, 27977, 15, '35 Agility            '),
    (7, 71, 27984, 16, 'Mongoose            '),
    (7, 71, 29483, 2, '26 Attackpower 14 Crit            '),
    (7, 71, 34002, 8, '24 Attackpower               '),
    (7, 71, 34004, 14, '12 Agility          '),
    (7, 71, 35452, 0, '34 Attackpower 16 Hit            '),
    (7, 71, 35495, 6, '40 Stamina 12 Agility            '),
    (7, 72, 22779, 17, '30 Hit            '),
    (7, 72, 27926, 10, '20 Healing 7 Spelldamage           '),
    (7, 72, 27926, 11, '20 Healing 7 Spelldamage            '),
    (7, 72, 27945, 16, '12 Intellect            '),
    (7, 72, 27954, 7, '5% Root/Snare Resist 10 Hit'),
    (7, 72, 27960, 4, '6 All Stats  '),
    (7, 72, 29475, 2, '31 Healing 11 Spelldamage 5 mp5            '),
    (7, 72, 31370, 6, '66 Healing 22 Spelldamage 20 Stamina            '),
    (7, 72, 33999, 9, '35 Healing 12 Spelldamage            '),
    (7, 72, 34001, 8, '12 Intellect               '),
    (7, 72, 34003, 14, '20 Spell Penetration          '),
    (7, 72, 34010, 15, '81 Healing 27 Spelldamage            '),
    (7, 72, 35445, 0, '35 Healing 12 Spelldamage 7 mp5            '),
    (8, 80, 22779, 17, '30 Hit            '),
    (8, 80, 27927, 10, '4 All Stats            '),
    (8, 80, 27927, 11, '4 All Stats            '),
    (8, 80, 27945, 16, '12 Intellect            '),
    (8, 80, 27954, 7, '5% Root/Snare Resist 10 Hit'),
    (8, 80, 27960, 4, '6 All Stats  '),
    (8, 80, 27975, 15, '40 Spelldamage            '),
    (8, 80, 31372, 6, '35 Spelldamage 20 Stamina            '),
    (8, 80, 33994, 9, '15 Spell Hit            '),
    (8, 80, 34001, 8, '12 Intellect               '),
    (8, 80, 34003, 14, '20 Spell Penetration          '),
    (8, 80, 35406, 2, '18 Spelldamage 10 Crit            '),
    (8, 80, 35447, 0, '22 Spelldamage 14 Hit            '),
    (9, 90, 22779, 17, '30 Hit            '),
    (9, 90, 27924, 11, '12 Spelldamage            '),
    (9, 90, 27927, 10, '4 All Stats            '),
    (9, 90, 27945, 16, '12 Intellect            '),
    (9, 90, 27954, 7, '5% Root/Snare Resist 10 Hit'),
    (9, 90, 27960, 4, '6 All Stats  '),
    (9, 90, 27975, 15, '40 Spelldamage            '),
    (9, 90, 31372, 6, '35 Spelldamage 20 Stamina            '),
    (9, 90, 33994, 9, '15 Spell Hit            '),
    (9, 90, 34001, 8, '12 Intellect               '),
    (9, 90, 34003, 14, '20 Spell Penetration          '),
    (9, 90, 35406, 2, '18 Spelldamage 10 Crit            '),
    (9, 90, 35447, 0, '22 Spelldamage 14 Hit            '),
    (11, 110, 22779, 17, '30 Hit            '),
    (11, 110, 27926, 10, '20 Healing 7 Spelldamage            '),
    (11, 110, 27926, 11, '20 Healing 7 Spelldamage            '),
    (11, 110, 27945, 16, '12 Intellect            '),
    (11, 110, 27954, 7, '5% Root/Snare Resist 10 Hit'),
    (11, 110, 27960, 4, '6 All Stats  '),
    (11, 110, 31370, 6, '66 Healing 22 Spelldamage 20 Stamina            '),
    (11, 110, 33994, 9, '15 Spell Hit            '),
    (11, 110, 34001, 8, '12 Intellect               '),
    (11, 110, 34003, 14, '20 Spell Penetration          '),
    (11, 110, 34010, 15, '81 Healing 27 Spelldamage            '),
    (11, 110, 35406, 2, '18 Spelldamage 10 Crit            '),
    (11, 110, 35447, 0, '22 Spelldamage 14 Hit            '),
    (11, 111, 22779, 17, '30 Hit            '),
    (11, 111, 25080, 9, '15 Agility            '),
    (11, 111, 27927, 10, '4 All Stats            '),
    (11, 111, 27927, 11, '4 All Stats           '),
    (11, 111, 27951, 7, '12 Agility'),
    (11, 111, 27960, 4, '6 All Stats  '),
    (11, 111, 29483, 2, '26 Attackpower 14 Crit            '),
    (11, 111, 34002, 8, '24 Attackpower               '),
    (11, 111, 34004, 14, '12 Agility          '),
    (11, 111, 35452, 0, '34 Attackpower 16 Hit            '),
    (11, 111, 35495, 6, '40 Stamina 12 Agility            '),
    (11, 111, 42620, 15, 'Greater Agility            '),
    (11, 111, 42620, 16, 'Greater Agility            '),
    (11, 112, 22779, 17, '30 Hit            '),
    (11, 112, 27926, 10, '20 Healing 7 Spelldamage            '),
    (11, 112, 27926, 11, '20 Healing 7 Spelldamage            '),
    (11, 112, 27945, 16, '12 Intellect            '),
    (11, 112, 27954, 7, '5% Root/Snare Resist 10 Hit'),
    (11, 112, 27960, 4, '6 All Stats  '),
    (11, 112, 29475, 2, '31 Healing 11 Spelldamage 5 mp5            '),
    (11, 112, 31370, 6, '66 Healing 22 Spelldamage 20 Stamina            '),
    (11, 112, 33999, 9, '35 Healing 12 Spelldamage            '),
    (11, 112, 34001, 8, '12 Intellect               '),
    (11, 112, 34003, 14, '20 Spell Penetration          '),
    (11, 112, 34010, 15, '81 Healing 27 Spelldamage            '),
    (11, 112, 35445, 0, '35 Healing 12 Spelldamage 7 mp5            ');

-- base/playerbots_equip_cache.sql (schema only)
DROP TABLE IF EXISTS `playerbots_equip_cache`;
CREATE TABLE `playerbots_equip_cache` (
  `id` INT(11) auto_increment,
  `clazz` TINYINT(3) NOT NULL,
  `lvl` INT(11) NOT NULL,
  `slot` TINYINT(8) NOT NULL,
  `quality` INT(11) NOT NULL,
  `item` INT(11) NOT NULL,
PRIMARY KEY (`id`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8 ROW_FORMAT=FIXED COMMENT='Playerbots equip Cache';

-- base/playerbots_guild_tasks.sql
DROP TABLE IF EXISTS `playerbots_guild_tasks`;
CREATE TABLE `playerbots_guild_tasks` (
  `id` INT(11) NOT NULL AUTO_INCREMENT,
  `owner` INT(11) NOT NULL,
  `guildid` INT(11) NOT NULL,
  `time` INT(11) NOT NULL,
  `validIn` INT(11) DEFAULT NULL,
  `type` varchar(45) DEFAULT NULL,
  `value` INT(11) DEFAULT NULL,
  `data` varchar(255) DEFAULT NULL,
  PRIMARY KEY (`id`),
  KEY `owner` (`owner`),
  KEY `guildid` (`guildid`),
  KEY `type` (`type`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- base/playerbots_item_info_cache.sql (schema only)
DROP TABLE IF EXISTS `playerbots_item_info_cache`;
CREATE TABLE IF NOT EXISTS `playerbots_item_info_cache`
(
    `id`             int(11) NOT NULL,
    `quality`        int(11)    DEFAULT NULL,
    `slot`           int(11)    DEFAULT NULL,
    `source`         bigint(20) DEFAULT NULL,
    `sourceId`       bigint(20) DEFAULT NULL,
    `team`           bigint(20) DEFAULT NULL,
    `faction`        bigint(20) DEFAULT NULL,
    `factionRepRank` bigint(20) DEFAULT NULL,
    `minLevel`       bigint(20) DEFAULT NULL,
    `scale_1`        bigint(20) DEFAULT NULL,
    `scale_2`        bigint(20) DEFAULT NULL,
    `scale_3`        bigint(20) DEFAULT NULL,
    `scale_4`        bigint(20) DEFAULT NULL,
    `scale_5`        bigint(20) DEFAULT NULL,
    `scale_6`        bigint(20) DEFAULT NULL,
    `scale_7`        bigint(20) DEFAULT NULL,
    `scale_8`        bigint(20) DEFAULT NULL,
    `scale_9`        bigint(20) DEFAULT NULL,
    `scale_10`       bigint(20) DEFAULT NULL,
    `scale_11`       bigint(20) DEFAULT NULL,
    `scale_12`       bigint(20) DEFAULT NULL,
    `scale_13`       bigint(20) DEFAULT NULL,
    `scale_14`       bigint(20) DEFAULT NULL,
    `scale_15`       bigint(20) DEFAULT NULL,
    `scale_16`       bigint(20) DEFAULT NULL,
    `scale_17`       bigint(20) DEFAULT NULL,
    `scale_18`       bigint(20) DEFAULT NULL,
    `scale_19`       bigint(20) DEFAULT NULL,
    `scale_20`       bigint(20) DEFAULT NULL,
    `scale_21`       bigint(20) DEFAULT NULL,
    `scale_22`       bigint(20) DEFAULT NULL,
    `scale_23`       bigint(20) DEFAULT NULL,
    `scale_24`       bigint(20) DEFAULT NULL,
    `scale_25`       bigint(20) DEFAULT NULL,
    `scale_26`       bigint(20) DEFAULT NULL,
    `scale_27`       bigint(20) DEFAULT NULL,
    `scale_28`       bigint(20) DEFAULT NULL,
    `scale_29`       bigint(20) DEFAULT NULL,
    `scale_30`       bigint(20) DEFAULT NULL,
    `scale_31`       bigint(20) DEFAULT NULL,
    `scale_32`       bigint(20) DEFAULT NULL,
    PRIMARY KEY (`id`)
) ENGINE = MyISAM
  DEFAULT CHARSET = utf8
  ROW_FORMAT = FIXED COMMENT ='PlayerbotAI item info Cache';

-- base/playerbots_preferred_mounts.sql
DROP TABLE IF EXISTS `playerbots_preferred_mounts`;
CREATE TABLE `playerbots_preferred_mounts` (
  `id` INT(11) NOT NULL AUTO_INCREMENT,
  `guid` INT(11) NOT NULL,
  `type` TINYINT(3) NOT NULL COMMENT '0: Ground, 1: Flying',
  `spellid` INT(11) NOT NULL,
  PRIMARY KEY (`id`),
  KEY `guid` (`guid`),
  KEY `type` (`type`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- base/playerbots_random_bots.sql
DROP TABLE IF EXISTS `playerbots_random_bots`;
CREATE TABLE `playerbots_random_bots` (
  `id` INT(11) NOT NULL AUTO_INCREMENT,
  `owner` INT(11) NOT NULL,
  `bot` INT(11) NOT NULL,
  `time` INT(11) NOT NULL,
  `validIn` INT(11) DEFAULT NULL,
  `event` varchar(45) DEFAULT NULL,
  `value` INT(11) DEFAULT NULL,
  `data` varchar(255) DEFAULT NULL,
  PRIMARY KEY (`id`),
  KEY `owner` (`owner`),
  KEY `bot` (`bot`),
  KEY `event` (`event`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- base/playerbots_rarity_cache.sql (schema only)
DROP TABLE IF EXISTS `playerbots_rarity_cache`;
CREATE TABLE `playerbots_rarity_cache` (
  `id` INT(11) auto_increment,
  `item` INT(11) NOT NULL,
  `rarity` float NOT NULL,
  PRIMARY KEY (`id`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8 ROW_FORMAT=FIXED COMMENT='Playerbots Rarity Cache';

-- base/playerbots_rnditem_cache.sql (schema only)
DROP TABLE IF EXISTS `playerbots_rnditem_cache`;
CREATE TABLE `playerbots_rnditem_cache` (
  `id` INT(11) auto_increment,
  `lvl` INT(11) NOT NULL,
  `type` INT(11) NOT NULL,
  `item` INT(11) NOT NULL,
PRIMARY KEY (`id`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8 ROW_FORMAT=FIXED COMMENT='Playerbots Random Item Cache';

-- base/playerbots_speech.sql (schema only)
DROP TABLE IF EXISTS `playerbots_speech`;
CREATE TABLE `playerbots_speech` (
  `id` INT(11) NOT NULL AUTO_INCREMENT,
  `name` varchar(255) NOT NULL,
  `text` varchar(1024) NOT NULL,
  `type` varchar(10) NOT NULL,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- base/playerbots_speech_probability.sql (schema only)
DROP TABLE IF EXISTS `playerbots_speech_probability`;
CREATE TABLE `playerbots_speech_probability` (
  `id` INT(11) NOT NULL AUTO_INCREMENT,
  `name` varchar(255) NOT NULL,
  `probability` INT(11) NOT NULL,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- base/playerbots_tele_cache.sql (schema only)
DROP TABLE IF EXISTS `playerbots_tele_cache`;
CREATE TABLE `playerbots_tele_cache` (
  `id` INT(11) auto_increment,
  `level` TINYINT(3) NOT NULL,
  `map_id` mediumint(8) NOT NULL,
  `x` float(8) NOT NULL,
  `y` float(8) NOT NULL,
  `z` float(8) NOT NULL,
PRIMARY KEY (`id`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8 ROW_FORMAT=FIXED COMMENT='Playerbots Tele Cache';

-- base/playerbots_travelnode.sql (schema only)
DROP TABLE IF EXISTS `playerbots_travelnode`;
CREATE TABLE IF NOT EXISTS `playerbots_travelnode` (
`id` mediumint(8) NOT NULL,
`name` varchar(1024) NOT NULL,
`map_id` mediumint(8) NOT NULL,
`x` float NOT NULL,
`y` float NOT NULL,
`z` float NOT NULL,
`linked` tinyint(2) DEFAULT NULL,
PRIMARY KEY (`id`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8 ROW_FORMAT=FIXED COMMENT='PlayerbotAI Travel Node';

-- base/playerbots_travelnode_link.sql (schema only)
DROP TABLE IF EXISTS `playerbots_travelnode_link`;
CREATE TABLE IF NOT EXISTS `playerbots_travelnode_link` (
`node_id` mediumint(8) NOT NULL,
`to_node_id` mediumint(8) NOT NULL,
`type` tinyint(3) NOT NULL,
`object` mediumint(8) NOT NULL,
`distance` float NOT NULL,
`swim_distance` float NOT NULL,
`extra_cost` float NOT NULL,
`calculated` tinyint(1) NOT NULL,
`max_creature_0` tinyint(2) NOT NULL,
`max_creature_1` tinyint(2) NOT NULL,
`max_creature_2` tinyint(2) NOT NULL,
PRIMARY KEY (`node_id`,`to_node_id`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8 ROW_FORMAT=FIXED COMMENT='PlayerbotAI Travel Node link';

-- base/playerbots_travelnode_path.sql (schema only)
DROP TABLE IF EXISTS `playerbots_travelnode_path`;
CREATE TABLE IF NOT EXISTS `playerbots_travelnode_path` (
`node_id` mediumint(8) NOT NULL,
`to_node_id` mediumint(8) NOT NULL,
`nr` mediumint(8) NOT NULL,
`map_id` mediumint(8) NOT NULL,
`x` float NOT NULL,
`y` float NOT NULL,
`z` float NOT NULL,
PRIMARY KEY (`node_id`,`to_node_id`,`nr`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8 ROW_FORMAT=FIXED COMMENT='PlayerbotAI Travel Node path';

-- base/playerbots_weightscale_data.sql
DROP TABLE IF EXISTS `playerbots_weightscale_data`;
CREATE TABLE IF NOT EXISTS `playerbots_weightscale_data` (
  `id` int(32) NOT NULL,
  `field` varchar(18) NOT NULL,
  `val` smallint(6) unsigned NOT NULL,
  KEY `id` (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;
INSERT INTO `playerbots_weightscale_data` (`id`, `field`, `val`) VALUES
    (1, 'exprtng', 100),
    (1, 'str', 82),
    (1, 'critstrkrtng', 66),
    (1, 'agi', 53),
    (1, 'armorpenrtng', 52),
    (1, 'hitrtng', 48),
    (1, 'hastertng', 36),
    (1, 'atkpwr', 31),
    (1, 'armor', 5),
    (2, 'exprtng', 100),
    (2, 'str', 82),
    (2, 'critstrkrtng', 66),
    (2, 'agi', 53),
    (2, 'armorpenrtng', 52),
    (2, 'hitrtng', 48),
    (2, 'hastertng', 36),
    (2, 'atkpwr', 31),
    (2, 'armor', 5),
    (3, 'sta', 100),
    (3, 'dodgertng', 90),
    (3, 'defrtng', 86),
    (3, 'block', 81),
    (3, 'agi', 67),
    (3, 'parryrtng', 67),
    (3, 'blockrtng', 48),
    (3, 'str', 48),
    (3, 'exprtng', 19),
    (3, 'hitrtng', 10),
    (3, 'armorpenrtng', 10),
    (3, 'critstrkrtng', 7),
    (3, 'armor', 6),
    (3, 'hastertng', 1),
    (3, 'atkpwr', 1),
    (4, 'int', 100),
    (4, 'manargn', 88),
    (4, 'splpwr', 58),
    (4, 'critstrkrtng', 46),
    (4, 'hastertng', 35),
    (5, 'sta', 100),
    (5, 'dodgertng', 94),
    (5, 'block', 86),
    (5, 'defrtng', 86),
    (5, 'exprtng', 79),
    (5, 'agi', 76),
    (5, 'parryrtng', 76),
    (5, 'hitrtng', 58),
    (5, 'blockrtng', 52),
    (5, 'str', 50),
    (5, 'armor', 6),
    (5, 'atkpwr', 6),
    (5, 'splpwr', 4),
    (5, 'critstrkrtng', 3),
    (6, 'mledps', 470),
    (6, 'hitrtng', 100),
    (6, 'str', 80),
    (6, 'exprtng', 66),
    (6, 'critstrkrtng', 40),
    (6, 'atkpwr', 34),
    (6, 'agi', 32),
    (6, 'hastertng', 30),
    (6, 'armorpenrtng', 22),
    (6, 'splpwr', 9),
    (7, 'rgddps', 213),
    (7, 'hitrtng', 100),
    (7, 'agi', 58),
    (7, 'critstrkrtng', 40),
    (7, 'int', 37),
    (7, 'atkpwr', 30),
    (7, 'armorpenrtng', 28),
    (7, 'hastertng', 21),
    (8, 'rgddps', 379),
    (8, 'hitrtng', 100),
    (8, 'agi', 74),
    (8, 'critstrkrtng', 57),
    (8, 'armorpenrtng', 40),
    (8, 'int', 39),
    (8, 'atkpwr', 32),
    (8, 'hastertng', 24),
    (9, 'rgddps', 181),
    (9, 'hitrtng', 100),
    (9, 'agi', 76),
    (9, 'critstrkrtng', 42),
    (9, 'int', 35),
    (9, 'hastertng', 31),
    (9, 'atkpwr', 29),
    (9, 'armorpenrtng', 26),
    (10, 'mledps', 170),
    (10, 'agi', 100),
    (10, 'exprtng', 87),
    (10, 'hitrtng', 83),
    (10, 'critstrkrtng', 81),
    (10, 'atkpwr', 65),
    (10, 'armorpenrtng', 65),
    (10, 'hastertng', 64),
    (10, 'str', 55),
    (11, 'mledps', 220),
    (11, 'armorpenrtng', 100),
    (11, 'agi', 100),
    (11, 'exprtng', 82),
    (11, 'hitrtng', 80),
    (11, 'critstrkrtng', 75),
    (11, 'hastertng', 73),
    (11, 'str', 55),
    (11, 'atkpwr', 50),
    (12, 'mledps', 228),
    (12, 'exprtng', 100),
    (12, 'agi', 100),
    (12, 'hitrtng', 80),
    (12, 'armorpenrtng', 75),
    (12, 'critstrkrtng', 75),
    (12, 'hastertng', 75),
    (12, 'str', 55),
    (12, 'atkpwr', 50),
    (13, 'splpwr', 100),
    (13, 'manargn', 67),
    (13, 'int', 65),
    (13, 'hastertng', 59),
    (13, 'critstrkrtng', 48),
    (13, 'spi', 22),
    (14, 'manargn', 100),
    (14, 'int', 69),
    (14, 'splpwr', 60),
    (14, 'spi', 52),
    (14, 'critstrkrtng', 38),
    (14, 'hastertng', 31),
    (15, 'hitrtng', 100),
    (15, 'shasplpwr', 76),
    (15, 'splpwr', 76),
    (15, 'critstrkrtng', 54),
    (15, 'hastertng', 50),
    (15, 'spi', 16),
    (15, 'int', 16),
    (16, 'mledps', 360),
    (16, 'armorpenrtng', 100),
    (16, 'str', 99),
    (16, 'hitrtng', 91),
    (16, 'exprtng', 90),
    (16, 'critstrkrtng', 57),
    (16, 'hastertng', 55),
    (16, 'atkpwr', 36),
    (16, 'armor', 1),
    (17, 'mledps', 337),
    (17, 'hitrtng', 100),
    (17, 'str', 97),
    (17, 'exprtng', 81),
    (17, 'armorpenrtng', 61),
    (17, 'critstrkrtng', 45),
    (17, 'atkpwr', 35),
    (17, 'hastertng', 28),
    (17, 'armor', 1),
    (18, 'mledps', 419),
    (18, 'parryrtng', 100),
    (18, 'hitrtng', 97),
    (18, 'str', 96),
    (18, 'defrtng', 85),
    (18, 'exprtng', 69),
    (18, 'dodgertng', 61),
    (18, 'agi', 61),
    (18, 'sta', 61),
    (18, 'critstrkrtng', 49),
    (18, 'atkpwr', 41),
    (18, 'armorpenrtng', 31),
    (18, 'armor', 5),
    (19, 'mledps', 209),
    (19, 'str', 100),
    (19, 'hitrtng', 66),
    (19, 'exprtng', 51),
    (19, 'hastertng', 48),
    (19, 'critstrkrtng', 45),
    (19, 'atkpwr', 34),
    (19, 'armorpenrtng', 32),
    (19, 'armor', 1),
    (20, 'hitrtng', 100),
    (20, 'splpwr', 60),
    (20, 'hastertng', 56),
    (20, 'critstrkrtng', 40),
    (20, 'int', 11),
    (21, 'mledps', 135),
    (21, 'hitrtng', 100),
    (21, 'exprtng', 84),
    (21, 'agi', 55),
    (21, 'int', 55),
    (21, 'critstrkrtng', 55),
    (21, 'hastertng', 42),
    (21, 'str', 35),
    (21, 'atkpwr', 32),
    (21, 'splpwr', 29),
    (21, 'armorpenrtng', 26),
    (22, 'manargn', 100),
    (22, 'int', 85),
    (22, 'splpwr', 77),
    (22, 'critstrkrtng', 62),
    (22, 'hastertng', 35),
    (23, 'hitrtng', 100),
    (23, 'hastertng', 54),
    (23, 'arcsplpwr', 49),
    (23, 'splpwr', 49),
    (23, 'critstrkrtng', 37),
    (23, 'int', 34),
    (23, 'frosplpwr', 24),
    (23, 'firsplpwr', 24),
    (23, 'spi', 14),
    (24, 'hitrtng', 100),
    (24, 'hastertng', 53),
    (24, 'firsplpwr', 46),
    (24, 'splpwr', 46),
    (24, 'critstrkrtng', 43),
    (24, 'frosplpwr', 23),
    (24, 'arcsplpwr', 23),
    (24, 'int', 13),
    (25, 'hitrtng', 100),
    (25, 'hastertng', 42),
    (25, 'frosplpwr', 39),
    (25, 'splpwr', 39),
    (25, 'arcsplpwr', 19),
    (25, 'firsplpwr', 19),
    (25, 'critstrkrtng', 19),
    (25, 'int', 6),
    (26, 'hitrtng', 100),
    (26, 'shasplpwr', 72),
    (26, 'splpwr', 72),
    (26, 'hastertng', 61),
    (26, 'critstrkrtng', 38),
    (26, 'firsplpwr', 36),
    (26, 'spi', 34),
    (26, 'int', 15),
    (27, 'hitrtng', 100),
    (27, 'hastertng', 50),
    (27, 'firsplpwr', 45),
    (27, 'shasplpwr', 45),
    (27, 'splpwr', 45),
    (27, 'critstrkrtng', 31),
    (27, 'spi', 29),
    (27, 'int', 13),
    (28, 'hitrtng', 100),
    (28, 'firsplpwr', 47),
    (28, 'splpwr', 47),
    (28, 'hastertng', 46),
    (28, 'spi', 26),
    (28, 'shasplpwr', 23),
    (28, 'critstrkrtng', 16),
    (28, 'int', 13),
    (29, 'hitrtng', 100),
    (29, 'splpwr', 66),
    (29, 'hastertng', 54),
    (29, 'critstrkrtng', 43),
    (29, 'spi', 22),
    (29, 'int', 22),
    (30, 'agi', 100),
    (30, 'sta', 75),
    (30, 'dodgertng', 65),
    (30, 'defrtng', 60),
    (30, 'exprtng', 16),
    (30, 'str', 10),
    (30, 'armor', 10),
    (30, 'hitrtng', 8),
    (30, 'hastertng', 5),
    (30, 'atkpwr', 4),
    (30, 'feratkpwr', 4),
    (30, 'critstrkrtng', 3),
    (31, 'splpwr', 100),
    (31, 'manargn', 73),
    (31, 'hastertng', 57),
    (31, 'int', 51),
    (31, 'spi', 32),
    (31, 'critstrkrtng', 11),
    (32, 'agi', 100),
    (32, 'armorpenrtng', 90),
    (32, 'str', 80),
    (32, 'critstrkrtng', 55),
    (32, 'exprtng', 50),
    (32, 'hitrtng', 50),
    (32, 'feratkpwr', 40),
    (32, 'atkpwr', 40),
    (32, 'hastertng', 35);

-- base/playerbots_weightscales.sql
DROP TABLE IF EXISTS `playerbots_weightscales`;
CREATE TABLE IF NOT EXISTS `playerbots_weightscales` (
  `id` int(32) NOT NULL AUTO_INCREMENT,
  `name` varchar(32) NOT NULL,
  `class` tinyint(3) unsigned NOT NULL DEFAULT '0',
  PRIMARY KEY (`id`)
) ENGINE=InnoDB AUTO_INCREMENT=33 DEFAULT CHARSET=utf8 ROW_FORMAT=COMPACT;
INSERT INTO `playerbots_weightscales` (`id`, `name`, `class`) VALUES
    (1, 'arms', 1),
    (2, 'fury', 1),
    (3, 'prot', 1),
    (4, 'holy', 2),
    (5, 'prot', 2),
    (6, 'retrib', 2),
    (7, 'beast', 3),
    (8, 'marks', 3),
    (9, 'surv', 3),
    (10, 'assas', 4),
    (11, 'combat', 4),
    (12, 'subtle', 4),
    (13, 'disc', 5),
    (14, 'holy', 5),
    (15, 'shadow', 5),
    (16, 'blooddps', 6),
    (17, 'frostdps', 6),
    (18, 'frosttank', 6),
    (19, 'unholydps', 6),
    (20, 'elem', 7),
    (21, 'enhance', 7),
    (22, 'resto', 7),
    (23, 'arcane', 8),
    (24, 'fire', 8),
    (25, 'frost', 8),
    (26, 'afflic', 9),
    (27, 'demo', 9),
    (28, 'destro', 9),
    (29, 'balance', 11),
    (30, 'feraltank', 11),
    (31, 'resto', 11),
    (32, 'feraldps', 11);
-- updates/2024_08_07_00.sql (schema only)
DROP TABLE IF EXISTS `ai_playerbot_texts`;
CREATE TABLE IF NOT EXISTS `ai_playerbot_texts` (
  `id` smallint(20) NOT NULL AUTO_INCREMENT,
  `name` varchar(255) NOT NULL COMMENT 'name - used in strategies/code as filter',
  `text` varchar(1024) NOT NULL COMMENT 'text',
  `say_type` tinyint(3) NOT NULL DEFAULT '0' COMMENT '0 - say, 1 - yell',
  `reply_type` tinyint(3) NOT NULL DEFAULT '0' COMMENT 'if > 0 then can be filtered as a response to chat',
  `text_loc1` varchar(1024) NOT NULL DEFAULT '',
  `text_loc2` varchar(1024) NOT NULL DEFAULT '',
  `text_loc3` varchar(1024) NOT NULL DEFAULT '',
  `text_loc4` varchar(1024) NOT NULL DEFAULT '',
  `text_loc5` varchar(1024) NOT NULL DEFAULT '',
  `text_loc6` varchar(1024) NOT NULL DEFAULT '',
  `text_loc7` varchar(1024) NOT NULL DEFAULT '',
  `text_loc8` varchar(1024) NOT NULL DEFAULT '',
  PRIMARY KEY (`id`)
) ENGINE=InnoDB AUTO_INCREMENT=1 DEFAULT CHARSET=utf8 ROW_FORMAT=DYNAMIC;
DROP TABLE IF EXISTS `ai_playerbot_texts_chance`;
CREATE TABLE IF NOT EXISTS `ai_playerbot_texts_chance` (
  `id` bigint(20) NOT NULL AUTO_INCREMENT,
  `name` varchar(255) NOT NULL,
  `probability` bigint(20) NOT NULL,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB AUTO_INCREMENT=4 DEFAULT CHARSET=UTF8;

-- updates/2024_11_25_00.sql (schema only)


-- updates/2025_02_24_00.sql (schema only)


-- updates/2025_03_03_00.sql (schema only)


-- updates/2025_04_26_00.sql (schema only)
SET @index_exists := (
  SELECT COUNT(1)
  FROM INFORMATION_SCHEMA.STATISTICS
  WHERE TABLE_SCHEMA = DATABASE()
    AND TABLE_NAME = 'playerbots_random_bots'
    AND INDEX_NAME = 'idx_owner_bot_event'
);
SET @ddl := IF(@index_exists = 0,
  'ALTER TABLE `playerbots_random_bots` ADD INDEX `idx_owner_bot_event` (`owner`, `bot`, `event`);',
  'SELECT "Index idx_owner_bot_event already exists.";'
);

-- updates/2025_05_05_00_accountlinking.sql (schema only)
DROP TABLE IF EXISTS `playerbot_account_links`;
CREATE TABLE `playerbot_account_links` (
    `id` INT AUTO_INCREMENT PRIMARY KEY,
    `account_id` INT NOT NULL,
    `linked_account_id` INT NOT NULL,
    `created_at` TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    UNIQUE KEY `account_link` (`account_id`, `linked_account_id`)
) ENGINE=INNODB DEFAULT CHARSET=latin1;
DROP TABLE IF EXISTS `playerbot_account_keys`;
CREATE TABLE `playerbot_account_keys` (
    `account_id` INT PRIMARY KEY,
    `security_key` VARCHAR(255) NOT NULL,
    `created_at` TIMESTAMP DEFAULT CURRENT_TIMESTAMP
) ENGINE=INNODB DEFAULT CHARSET=latin1;

-- updates/2025_05_09_00.sql (schema only)
DROP TABLE IF EXISTS `playerbot_account_keys`;
CREATE TABLE IF NOT EXISTS `playerbots_account_keys` (
    `account_id` INT PRIMARY KEY,
    `security_key` VARCHAR(255) NOT NULL,
    `created_at` TIMESTAMP DEFAULT CURRENT_TIMESTAMP
) ENGINE=INNODB DEFAULT CHARSET=latin1;
DROP TABLE IF EXISTS `playerbot_account_links`;
CREATE TABLE IF NOT EXISTS `playerbots_account_links` (
    `id` INT AUTO_INCREMENT PRIMARY KEY,
    `account_id` INT NOT NULL,
    `linked_account_id` INT NOT NULL,
    `created_at` TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    UNIQUE KEY `account_link` (`account_id`, `linked_account_id`)
) ENGINE=INNODB DEFAULT CHARSET=latin1;

-- updates/2025_07_01_00_account_type.sql (schema only)
DROP TABLE IF EXISTS `playerbots_account_type`;
CREATE TABLE `playerbots_account_type` (
    `account_id` int unsigned NOT NULL,
    `account_type` tinyint unsigned NOT NULL DEFAULT 0 COMMENT '0 = unassigned, 1 = RNDbot, 2 = AddClass',
    `assignment_date` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`account_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Playerbot account type assignments';

-- updates/2025_09_16_00.sql (schema only)


-- updates/2025_10_09_00_ai_playerbot_texts_fix.sql (schema only)
DROP TABLE IF EXISTS `ai_playerbot_texts`;
CREATE TABLE IF NOT EXISTS `ai_playerbot_texts` (
  `id` smallint(20) NOT NULL AUTO_INCREMENT,
  `name` varchar(255) NOT NULL COMMENT 'name - used in strategies/code as filter',
  `text` varchar(1024) NOT NULL COMMENT 'text',
  `say_type` tinyint(3) NOT NULL DEFAULT '0' COMMENT '0 - say, 1 - yell',
  `reply_type` tinyint(3) NOT NULL DEFAULT '0' COMMENT 'if > 0 then can be filtered as a response to chat',
  `text_loc1` varchar(1024) NOT NULL DEFAULT '',
  `text_loc2` varchar(1024) NOT NULL DEFAULT '',
  `text_loc3` varchar(1024) NOT NULL DEFAULT '',
  `text_loc4` varchar(1024) NOT NULL DEFAULT '',
  `text_loc5` varchar(1024) NOT NULL DEFAULT '',
  `text_loc6` varchar(1024) NOT NULL DEFAULT '',
  `text_loc7` varchar(1024) NOT NULL DEFAULT '',
  `text_loc8` varchar(1024) NOT NULL DEFAULT '',
  PRIMARY KEY (`id`)
) ENGINE=InnoDB AUTO_INCREMENT=1 DEFAULT CHARSET=utf8 ROW_FORMAT=DYNAMIC;

-- updates/2026_04_28_00_playerbots_bis_gear.sql (schema only)
DROP TABLE IF EXISTS `playerbots_bis`;
DROP TABLE IF EXISTS `playerbots_bis_gear`;
CREATE TABLE `playerbots_bis_gear` (
    `class`      TINYINT UNSIGNED NOT NULL,
    `tab`        TINYINT UNSIGNED NOT NULL,
    `slot`       TINYINT UNSIGNED NOT NULL,
    `faction`    TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `auto_gear_score_limit` SMALLINT UNSIGNED NOT NULL,
    `item_id`       INT UNSIGNED NOT NULL,
    `phase`      VARCHAR(32) NOT NULL DEFAULT '',
    `class_name`   VARCHAR(16) NOT NULL,
    `spec_name`    VARCHAR(32) NOT NULL,
    `slot_name`    VARCHAR(16) NOT NULL,
    `faction_name` VARCHAR(8)  NOT NULL DEFAULT 'Both',
    `item_name`    VARCHAR(96) NOT NULL,
    PRIMARY KEY (`class`, `tab`, `slot`, `faction`, `auto_gear_score_limit`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- updates/2026_06_13_00_ai_playerbot_missing_group_buff_reagent.sql (schema only)


-- updates/2026_06_19_00_ai_playerbot_cache_cleanup.sql (schema only)


-- updates/2026_09_21_00_playerbots_speech.sql (schema only)
DROP TABLE IF EXISTS `playerbots_speech`;
DROP TABLE IF EXISTS `playerbots_speech_probability`;

TRUNCATE TABLE `ai_playerbot_texts`;
TRUNCATE TABLE `ai_playerbot_texts_chance`;
