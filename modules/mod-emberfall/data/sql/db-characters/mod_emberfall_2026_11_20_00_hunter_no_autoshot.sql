-- mod-emberfall: the Hunter after ankarna's Marksman notes of 2026-11-19 (Emberfall docs/design/classes/hunter-marksman.md
-- §40). Two things every existing character loses, with nothing left behind on a button:
--
-- 1. Loose, the ranged auto shot (spell 75), is gone for the whole class, every level and spec (the melee auto attack
--    stays). New hunters never learn it (its SkillLineAbility row on the Bows line and its button 2 are gone from the
--    data); here the known spell goes, every action button that held it (the active bars, character_action, and the other
--    specs' bars, ef_character_actions) is emptied, and the Ranger's record of it (ef_character_granted source 3: Loose
--    taken away while Retreat's node replaced it, which the talent module would teach back once no node replaces it) goes.
-- 2. Growl (93262), the companions' taunt, is gone: the companions forget it (pet_spell); the core empties the pet bar's
--    button of a spell the companion no longer knows when it loads (Pet::CleanupActionBar).

DELETE FROM `character_spell` WHERE `spell` = 75;
DELETE FROM `character_action` WHERE `action` = 75 AND `type` = 0;
DELETE FROM `ef_character_actions` WHERE `action` = 75 AND `type` = 0;
DELETE FROM `ef_character_granted` WHERE `spell` = 75;
DELETE FROM `pet_spell` WHERE `spell` = 93262;
