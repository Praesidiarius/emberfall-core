-- mod-emberfall: unstuck (src/emberfall_unstuck.cpp, Emberfall docs/design/unstuck.md). The town points' schema:
-- Emberfall's compiler writes the rows (world.yaml teleports with `town:`). `.unstuck` (the core's, cs_misc) is opened
-- to every account (the stock row says 2, which overrides the core's player permission); `.stuck` is the module's.
CREATE TABLE IF NOT EXISTS `ef_unstuck_point` (
  `id` INT UNSIGNED NOT NULL COMMENT 'the game_tele id of the place',
  `map` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `x` FLOAT NOT NULL DEFAULT 0,
  `y` FLOAT NOT NULL DEFAULT 0,
  `z` FLOAT NOT NULL DEFAULT 0,
  `o` FLOAT NOT NULL DEFAULT 0,
  `name` VARCHAR(100) NOT NULL DEFAULT '' COMMENT 'the town''s name, as the player is told',
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall unstuck: the towns .unstuck takes a player to (the nearest on the map)';

DELETE FROM `command` WHERE `name` IN ('unstuck', 'stuck');
INSERT INTO `command` (`name`, `security`, `help`) VALUES
('unstuck', 0, 'Syntax: .unstuck\r\nStuck in the world? After a 5 second cast (moving or damage breaks it) you are taken to the nearest town; in a dungeon to its entrance; dead, to your graveyard. Not in combat; once every 5 minutes.\r\nGame masters: .unstuck $playername [inn/graveyard/startzone]'),
('stuck', 0, 'Syntax: .stuck\r\nThe same as .unstuck: to the nearest town after a 5 second cast.');
