-- mod-emberfall: Beastcraft points for a tame by the band of the hunter's skill (Emberfall docs/design/
-- secondary-professions.md §10.7, hunter-taming.md §3.1). Written by Emberfall's content build (taming.py, from
-- content/data/taming/families.yaml `tame_points` and secondary_ranks.stretched_points: the old 1 / 3 / 5 times each
-- band's stretch), read by src/emberfall_taming.cpp at startup and `.eftame reload`. A row holds for a skill up to
-- `up_to` (the band's cap); without rows the module gives 1 / 3 / 5.

CREATE TABLE IF NOT EXISTS `ef_tame_points` (
  `up_to` SMALLINT UNSIGNED NOT NULL COMMENT 'the band''s top skill',
  `normal` SMALLINT UNSIGNED NOT NULL DEFAULT 1,
  `elite` SMALLINT UNSIGNED NOT NULL DEFAULT 3,
  `rare` SMALLINT UNSIGNED NOT NULL DEFAULT 5 COMMENT 'a rare or rare elite beast',
  PRIMARY KEY (`up_to`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall taming: Beastcraft points per band';
