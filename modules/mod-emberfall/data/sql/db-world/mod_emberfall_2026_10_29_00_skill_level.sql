-- mod-emberfall: a skill a class learns at a level, not at creation (src/emberfall_skill_levels.cpp; Emberfall
-- docs/design/gear/item-pools.md §15: the Hunter's mail at 40). Emberfall's compiler writes the rows (player.yaml
-- `skills_at_level`), emptied and refilled by world.sql.
CREATE TABLE IF NOT EXISTS `ef_skill_level` (
  `class` TINYINT UNSIGNED NOT NULL,
  `skill` SMALLINT UNSIGNED NOT NULL COMMENT 'SkillLine: needs a SkillRaceClassInfo row for the class',
  `level` TINYINT UNSIGNED NOT NULL COMMENT 'learned at this level; taken away (and its items unequipped) below it',
  `spell` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'the proficiency spell the skill teaches (its item subclass mask)',
  PRIMARY KEY (`class`, `skill`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall: skills a class learns at a level';
