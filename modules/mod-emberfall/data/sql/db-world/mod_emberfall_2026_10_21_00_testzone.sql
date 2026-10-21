-- mod-emberfall: `.eftest zone` (src/emberfall_testzone.cpp; off unless Emberfall.TestTools.Enable = 1). The levelling
-- route's schema: Emberfall's compiler writes the rows (tools/py/emberfall_content/zone_route.py, from the zone briefs,
-- human-journey.md §11, the quests, people, teleports, flights and item pools), emptied and refilled by world.sql.
CREATE TABLE IF NOT EXISTS `ef_test_zone` (
  `zone` VARCHAR(32) NOT NULL COMMENT 'the zone''s key (.eftest zone <key>)',
  `name` VARCHAR(64) NOT NULL DEFAULT '',
  `sort` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'route order: by level band',
  `area` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'AreaTable zone',
  `level_min` TINYINT UNSIGNED NOT NULL DEFAULT 1 COMMENT 'the entry level',
  `level_max` TINYINT UNSIGNED NOT NULL DEFAULT 1,
  `races` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'race mask of a start zone; 0: a zone every route shares',
  `map` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `x` FLOAT NOT NULL DEFAULT 0,
  `y` FLOAT NOT NULL DEFAULT 0,
  `z` FLOAT NOT NULL DEFAULT 0,
  `o` FLOAT NOT NULL DEFAULT 0,
  `hub` VARCHAR(64) NOT NULL DEFAULT '' COMMENT 'the main hub (a start zone: the race''s start)',
  PRIMARY KEY (`zone`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall .eftest zone: the levelling zones in route order';

CREATE TABLE IF NOT EXISTS `ef_test_zone_quest` (
  `quest` INT UNSIGNED NOT NULL,
  `zone` VARCHAR(32) NOT NULL DEFAULT '',
  `sort` INT UNSIGNED NOT NULL DEFAULT 0,
  `leads_to` VARCHAR(32) NOT NULL DEFAULT '' COMMENT 'the zone a breadcrumb leads into',
  `role` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 done, 1 breadcrumb into `leads_to`, 2 after such a breadcrumb',
  PRIMARY KEY (`quest`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall .eftest zone: each zone''s quests';

CREATE TABLE IF NOT EXISTS `ef_test_zone_taxi` (
  `zone` VARCHAR(32) NOT NULL,
  `node` INT UNSIGNED NOT NULL COMMENT 'TaxiNodes',
  PRIMARY KEY (`zone`, `node`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall .eftest zone: each zone''s flight nodes';

CREATE TABLE IF NOT EXISTS `ef_test_gear` (
  `class` TINYINT UNSIGNED NOT NULL,
  `slot` TINYINT UNSIGNED NOT NULL COMMENT 'EQUIPMENT_SLOT_*',
  `item` INT UNSIGNED NOT NULL,
  `required_level` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`class`, `slot`, `item`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall .eftest zone: the item pools'' greens per class and slot';

DELETE FROM `command` WHERE `name` IN ('eftest', 'eftest zone');
INSERT INTO `command` (`name`, `security`, `help`) VALUES
('eftest', 0, 'Syntax: .eftest [$subcommand]\r\nEmberfall test tools (a test realm only): level, zone, foe, dummy, clear, refresh. Without a subcommand: the list.'),
('eftest zone', 0, 'Syntax: .eftest zone $zone [#level] | .eftest zone list\r\nPuts you where a player arriving at that zone would be: its entry level (or #level), every quest of the zones before it on your route done (their breadcrumbs into it in your log), pool gear for the level, the flight nodes so far, some money, and you at its main hub. The zone''s own quests stay untouched. list: the zones in route order with their levels.');
