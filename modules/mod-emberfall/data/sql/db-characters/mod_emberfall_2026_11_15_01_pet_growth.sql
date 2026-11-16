-- mod-emberfall: the hunter companion's growth (src/emberfall_petsize.cpp; Emberfall docs/design/hunter-taming.md §18,
-- feedback #572): the seconds each companion has been out with its hunter (counted while it is out and the hunter
-- online); its stage follows from them (the world's ef_pet_size_stage). A companion without a row is as tamed.
CREATE TABLE IF NOT EXISTS `ef_character_pet_growth` (
  `pet` INT UNSIGNED NOT NULL COMMENT 'character_pet.id',
  `owner` INT UNSIGNED NOT NULL COMMENT 'characters.guid',
  `seconds` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`pet`),
  KEY `owner` (`owner`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall companions: time out with the hunter';
