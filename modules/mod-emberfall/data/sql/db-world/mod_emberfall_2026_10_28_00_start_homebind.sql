-- mod-emberfall: a race's hearth bound away from its start at the first login (src/emberfall_homebind.cpp; Emberfall
-- docs/design/stoneborn.md §2.4). Emberfall's compiler writes the rows (player.yaml `start.<race>.hearth`), emptied and
-- refilled by world.sql.
CREATE TABLE IF NOT EXISTS `ef_start_homebind` (
  `race` TINYINT UNSIGNED NOT NULL,
  `map` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `area` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'AreaTable: the bind''s area (the hearth tooltip)',
  `position_x` FLOAT NOT NULL DEFAULT 0,
  `position_y` FLOAT NOT NULL DEFAULT 0,
  `position_z` FLOAT NOT NULL DEFAULT 0,
  PRIMARY KEY (`race`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall: a race''s hearth at its first login, away from its start';
