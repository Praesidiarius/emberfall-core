-- mod-emberfall: the companion's fight, fully automatic (src/emberfall_beasts.cpp, the core's PATCHES.md #46; Emberfall
-- docs/design/hunter-beasts.md §3). The schema only: Emberfall's compiler writes the rows (content/data/pets.yaml
-- `combat`, tools/py/emberfall_content/pet_combat.py). No rows: the core's autocast (the toggles, a random pick).

CREATE TABLE IF NOT EXISTS `ef_pet_priority` (
  `family` INT UNSIGNED NOT NULL COMMENT 'CreatureFamily: the companion kind',
  `ord` TINYINT UNSIGNED NOT NULL COMMENT '0: tried first',
  `spell` INT UNSIGNED NOT NULL,
  `reserve` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'power that must stay after the cast',
  `taunt` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '1: cast only while the player keeps it toggled on',
  PRIMARY KEY (`family`, `ord`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall companions: each kind''s spells, best first';
