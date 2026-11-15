-- mod-emberfall: a character's specialisation per craft at the last rank (Emberfall docs/design/professions.md §28).
-- Written only by the module (src/profspecs/EfProfSpecs.cpp: the pick and the unlearn at a trainer); deleted with the
-- character (OnPlayerDeleteFromDB). `unlearns` counts the times it was given up (the price ladder: 1, 10, 100, 1,000,
-- then 10,000 gold) and stays when the craft itself is unlearned. The one-time talents reset of the rework is an
-- ef_character_migration row `prof_talents_v2` (mod_emberfall_2026_11_06_00_one_profession.sql).

CREATE TABLE IF NOT EXISTS `ef_character_prof_spec` (
  `guid` INT UNSIGNED NOT NULL,
  `skill_line` SMALLINT UNSIGNED NOT NULL,
  `spell` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'the active specialisation (ef_prof_specialisation.spell); 0 none',
  `unlearns` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`guid`, `skill_line`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall professions: specialisations';
