-- mod-emberfall: the Class Journeys (src/emberfall_journeys.cpp; Emberfall docs/design/class-journeys/README.md). The
-- schemas only: Emberfall's compiler writes the rows (content/data/journeys/journeys.yaml and the journey quests,
-- tools/py/emberfall_content/journeys.py).

CREATE TABLE IF NOT EXISTS `ef_journey_step` (
  `class` TINYINT UNSIGNED NOT NULL COMMENT 'ChrClasses',
  `step` TINYINT UNSIGNED NOT NULL COMMENT '1..: the journey''s order',
  `level` TINYINT UNSIGNED NOT NULL COMMENT 'offered from this level',
  `spell` INT UNSIGNED NOT NULL COMMENT 'the step''s mark: known means done; taught by its last quest',
  `spell2` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'a second ability the step teaches (0: none)',
  `gate` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'the Hunter''s taming: 1 elite, 2 rare, 3 rare elite beasts need the mark',
  `existing` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'characters from before the journey, once: 1 at or above its level, 2 a hunter with Beastcraft or a companion',
  PRIMARY KEY (`class`, `step`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall Class Journeys: each class''s steps';

CREATE TABLE IF NOT EXISTS `ef_journey_quest` (
  `quest` INT UNSIGNED NOT NULL,
  `class` TINYINT UNSIGNED NOT NULL,
  `step` TINYINT UNSIGNED NOT NULL,
  `last` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '1: rewarding it teaches the step',
  PRIMARY KEY (`quest`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall Class Journeys: every journey quest''s step';

CREATE TABLE IF NOT EXISTS `ef_journey_cast` (
  `quest` INT UNSIGNED NOT NULL,
  `spell` INT UNSIGNED NOT NULL COMMENT 'cast with the quest in the log',
  `area` INT UNSIGNED NOT NULL COMMENT 'AreaTable: inside this zone or area',
  `credit` INT UNSIGNED NOT NULL COMMENT 'the kill credit it gives (a quest objective)',
  PRIMARY KEY (`quest`, `credit`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall Class Journeys: "use it there" objectives';

CREATE TABLE IF NOT EXISTS `ef_journey_tame_credit` (
  `creature` INT UNSIGNED NOT NULL COMMENT 'the kill credit a tame gives',
  `ranks` TINYINT UNSIGNED NOT NULL COMMENT 'bits: 1 normal, 2 elite, 4 rare, 8 rare elite',
  `min_level` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'a beast of this level or more (0: any)',
  PRIMARY KEY (`creature`, `ranks`, `min_level`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall Class Journeys: the Hunter''s tame credits by rank';
