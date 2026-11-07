-- Emberfall content 2026-11-07 (Nathanael 2026-11-06: "lets do some more hair styles for the dwarf and human females"):
-- five new hair styles each for the human female and the stoneborn female (content/data/characters.yaml
-- CharHairGeosets), added before bald, which stays the last style. Every other style keeps its index; bald moves:
-- the human female's from 7 to 12, the stoneborn female's from 4 to 9. Runs once (the module's update tracking).
UPDATE `characters` SET `hairStyle` = 12 WHERE `race` = 1 AND `gender` = 1 AND `hairStyle` = 7;
UPDATE `characters` SET `hairStyle` = 9 WHERE `race` = 13 AND `gender` = 1 AND `hairStyle` = 4;
