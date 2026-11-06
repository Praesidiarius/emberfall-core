-- mod-emberfall: the hunters' beast compendium (src/emberfall_taming.cpp, Emberfall docs/design/hunter-taming.md §16):
-- every item a hunter has fed a beast family while taming (eaten or refused). The module sends the list after the
-- client's EFN HELLO (KNOWN) and the compendium shows what each family made of it, on any machine; the taste itself
-- is the world data's (ef_tame_like), worked out when sent.
CREATE TABLE IF NOT EXISTS `ef_character_tame_known` (
  `guid` INT UNSIGNED NOT NULL,
  `family` INT UNSIGNED NOT NULL COMMENT 'ef_tame_family',
  `item` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`guid`, `family`, `item`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall taming: what each hunter has fed each family';
