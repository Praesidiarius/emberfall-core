-- Emberfall content 2026-10-31 (Nathanael): the human male's shaggy (style 2) and swept-back ponytail (style 6) read as
-- women's styles and went to the human female (content/data/characters.yaml CharHairGeosets). His styles above them
-- moved down: 0 swept-back top, 1 the top with its long back, 2 mohawk braid, 3 short crop, 4 topknot, 5 shoulder-length
-- waves, 6 short curls, 7 warrior braids, 8 undercut top, 9 bald. Men who wore the two styles get the nearest one left
-- (shaggy -> shoulder-length waves, the ponytail -> topknot); every other style keeps its look under its new index.
-- The human female gained the two as styles 5 and 6, so her bald (5) is now 7. Runs once (the module's update tracking).
UPDATE `characters` SET `hairStyle` = CASE `hairStyle`
    WHEN 2 THEN 5     -- shaggy -> shoulder-length waves
    WHEN 3 THEN 2     -- mohawk braid
    WHEN 4 THEN 3     -- short crop
    WHEN 5 THEN 4     -- topknot
    WHEN 6 THEN 4     -- swept-back ponytail -> topknot
    WHEN 7 THEN 5     -- shoulder-length waves
    WHEN 8 THEN 6     -- short curls
    WHEN 9 THEN 7     -- warrior braids
    WHEN 10 THEN 8    -- undercut top
    WHEN 11 THEN 9    -- bald
    ELSE `hairStyle` END
WHERE `race` = 1 AND `gender` = 0 AND `hairStyle` >= 2;
UPDATE `characters` SET `hairStyle` = 7 WHERE `race` = 1 AND `gender` = 1 AND `hairStyle` = 5;
