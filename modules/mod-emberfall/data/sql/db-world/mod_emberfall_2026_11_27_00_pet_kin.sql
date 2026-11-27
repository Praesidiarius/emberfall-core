-- mod-emberfall: the Hunter's beasts, part 2: the archetypes and the beast families' specials (src/emberfall_beasts_kin.cpp;
-- Emberfall docs/design/hunter-beasts.md §4-§5). The schema only: Emberfall's compiler writes the rows (content/data/pets.yaml
-- `archetypes`, taming/families.yaml `archetype` / `special` / `special_rule`; tools/py/emberfall_content/pet_kin.py). No rows:
-- no archetype passives and no family specials.

CREATE TABLE IF NOT EXISTS `ef_pet_archetype` (
  `id` TINYINT UNSIGNED NOT NULL COMMENT 'the rule the module runs: 1 Packrunner .. 6 Shellback',
  `passive` INT UNSIGNED NOT NULL COMMENT 'the hunter''s aura while a companion of it is out',
  `helper1` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'a spell the rule casts beside it',
  `helper2` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall companions: the archetypes'' passives';

CREATE TABLE IF NOT EXISTS `ef_pet_family_spell` (
  `family` INT UNSIGNED NOT NULL COMMENT 'the beast family (ef_tame_family)',
  `archetype` TINYINT UNSIGNED NOT NULL COMMENT 'ef_pet_archetype.id',
  `special` INT UNSIGNED NOT NULL COMMENT 'the family''s special attack, taught at summon',
  `rule` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '1 busy, 2 slowed, 3 water, 4 on cast, 5 no boss, 6 pierce',
  `rule_value` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'the rule''s percent',
  `name` VARCHAR(32) NOT NULL DEFAULT '' COMMENT 'the family''s key (taming/families.yaml): .efpet make <name>',
  PRIMARY KEY (`family`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall companions: each beast family''s archetype and special';
