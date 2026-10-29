-- PATCHES.md #27 (2026-10-29): the companions tamed before learned their family abilities with autocast off and the
-- server never turned it on, so nobody chose "off": turn the basic attacks (Snap, Raking Paw, Tusk Rake, Stoop, Scald
-- Bite) and Scald Spit on, in the spell book (pet_spell.active 129 -> 193, ACT_DISABLED -> ACT_ENABLED) and on the pet
-- bar (character_pet.abdata: "<state> <action> " pairs). Runs once (the module's update tracking).
UPDATE `pet_spell` SET `active` = 193 WHERE `active` = 129 AND `spell` IN (93252, 93253, 93254, 93255, 93259, 93260);
UPDATE `character_pet` SET `abdata` =
    REPLACE(REPLACE(REPLACE(REPLACE(REPLACE(REPLACE(`abdata`,
        '129 93252 ', '193 93252 '), '129 93253 ', '193 93253 '), '129 93254 ', '193 93254 '),
        '129 93255 ', '193 93255 '), '129 93259 ', '193 93259 '), '129 93260 ', '193 93260 ');
