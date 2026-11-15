-- mod-emberfall: the respawn floor of the enclosed places (src/emberfall_respawn_floor.cpp; Emberfall feedback #593,
-- docs/design/caves.md §13). The realm's dynamic respawn never brings a listed spawn back sooner than its floor after
-- its death. Emberfall's compiler writes the rows (every unnamed creature inside a cave, a mine or a cellar), emptied
-- and refilled by world.sql, which creates the table the same way when it is missing.
CREATE TABLE IF NOT EXISTS `ef_respawn_floor` (
  `guid` INT UNSIGNED NOT NULL COMMENT 'creature.guid: a spawn inside an enclosed place',
  `floor_secs` INT UNSIGNED NOT NULL COMMENT 'it never respawns sooner than this after its death (plus its corpse)',
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall respawn floor: spawns the dynamic respawn may not bring back sooner';
