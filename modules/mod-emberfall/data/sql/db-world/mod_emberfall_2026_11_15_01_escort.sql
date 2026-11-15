-- mod-emberfall: the escorts (src/emberfall_escorts.cpp; Emberfall docs/contributing/quests.md "Escorts", feedback #564).
-- An escort is its taker's: taking the quest from the escortee's spawn summons a copy of the escortee for the player,
-- which walks the path; the spawn stays at its place. Emberfall's compiler (tools/py/emberfall_content/quests/escorts.py)
-- writes the rows, emptied and refilled by world.sql.
CREATE TABLE IF NOT EXISTS `ef_escort` (
  `quest` INT UNSIGNED NOT NULL COMMENT 'quest_template: the escort quest',
  `npc` INT UNSIGNED NOT NULL COMMENT 'creature_template: the escortee (the quest''s giver)',
  PRIMARY KEY (`quest`),
  UNIQUE KEY `npc` (`npc`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall escorts: each escort quest and its escortee';
