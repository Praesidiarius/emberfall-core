-- mod-emberfall: the Hunter's "When tamed" choice (src/emberfall_beasts.cpp; Emberfall docs/design/hunter-beasts.md
-- §2.2): 0 the new beast comes out with the hunter (the old one goes to the stable), 1 it goes straight to the stable.
-- A hunter without a row: 0.
CREATE TABLE IF NOT EXISTS `ef_character_beast_pref` (
  `guid` INT UNSIGNED NOT NULL COMMENT 'characters.guid',
  `tame_to` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall hunters: where a new tame goes';
