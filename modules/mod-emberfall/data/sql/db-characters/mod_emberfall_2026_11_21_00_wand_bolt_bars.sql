-- mod-emberfall: Wand Bolt for every existing Mage and Warlock (Emberfall content/data/classes/mage.yaml 94177,
-- warlock.yaml 95170; Nathanael 2026-11-21: "all casters should get that button so they can use wands").
--
-- The spell is a start ability on the class kit's skill line: the core teaches it to every character of the class at the
-- next login (Player::_LoadSkills -> learnSkillRewardedSpells, before _LoadActions), and a new character finds it on
-- button 2 of its first bar (playercreateinfo_action, the kit's start_bar). An existing character has no button for it:
-- this update puts it once on the first free button of the first bar (buttons 0-11) of the active bars, never over a
-- button the player placed, and not at all when a button holds it already or the first bar is full (then it waits in
-- the spellbook's class tab). The other specs' stored bars (ef_character_actions) stay as they are.

INSERT INTO `character_action` (`guid`, `spec`, `button`, `action`, `type`)
SELECT c.`guid`, c.`activeTalentGroup`, MIN(b.`n`), IF(c.`class` = 8, 94177, 95170), 0
FROM `characters` c
JOIN (SELECT 0 AS `n` UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4 UNION ALL SELECT 5
      UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 UNION ALL SELECT 9 UNION ALL SELECT 10 UNION ALL SELECT 11) b
LEFT JOIN `character_action` a ON a.`guid` = c.`guid` AND a.`spec` = c.`activeTalentGroup` AND a.`button` = b.`n`
WHERE c.`class` IN (8, 9) AND a.`guid` IS NULL
  AND NOT EXISTS (SELECT 1 FROM `character_action` x WHERE x.`guid` = c.`guid` AND x.`type` = 0 AND x.`action` IN (94177, 95170))
GROUP BY c.`guid`, c.`activeTalentGroup`, c.`class`;
