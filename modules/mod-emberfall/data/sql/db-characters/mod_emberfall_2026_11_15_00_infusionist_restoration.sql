-- mod-emberfall: the Infusionist's update of 2026-11-14 (Emberfall docs/design/classes/mage-infusionist.md §9d).
-- Restorative Flask (94301) is gone; Arcane Restoration (94304), until then its button's face while its potions could not
-- be paid, is the spec's own main heal in its place (the tree's node replaces Mana Surge, as the Flask's did). A button
-- that held the Flask holds Arcane Restoration, on the active bars (the core's character_action) and the inactive specs'
-- (ef_character_actions), and a character that knew the Flask knows Arcane Restoration before its bars load (the core
-- drops a button whose spell the character does not know), granted by the talent module as the Flask was (source 2, a
-- node's spell), so a spec change takes it back as any node's spell.

UPDATE `character_action` SET `action` = 94304 WHERE `action` = 94301 AND `type` = 0;
UPDATE `ef_character_actions` SET `action` = 94304 WHERE `action` = 94301 AND `type` = 0;
UPDATE IGNORE `character_spell` SET `spell` = 94304 WHERE `spell` = 94301;
DELETE FROM `character_spell` WHERE `spell` = 94301;
UPDATE IGNORE `ef_character_granted` SET `spell` = 94304 WHERE `spell` = 94301;
DELETE FROM `ef_character_granted` WHERE `spell` = 94301;
