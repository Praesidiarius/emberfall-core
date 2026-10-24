-- mod-emberfall: nearby unstuck (src/emberfall_unstuck.cpp, src/unstuck/EfUnstuckNearby.h; Emberfall
-- docs/design/unstuck.md "Nearby"): `.stuck nearby` and `.stuck status` for every account; `.stuck` and `.unstuck` keep
-- going to the nearest town (their help no longer speaks of a 5 second cast: 7355 is instant since 2026-10-20).
DELETE FROM `command` WHERE `name` IN ('unstuck', 'stuck', 'stuck nearby', 'stuck status');
INSERT INTO `command` (`name`, `security`, `help`) VALUES
('unstuck', 0, 'Syntax: .unstuck\r\nStuck in the world? You are taken to the nearest town; in a dungeon to its entrance; dead, to your graveyard. Not in combat; once every 5 minutes. To be freed near where you are: .stuck nearby\r\nGame masters: .unstuck $playername [inn/graveyard/startzone]'),
('stuck', 0, 'Syntax: .stuck [nearby | status]\r\n.stuck: the same as .unstuck, to the nearest town. .stuck nearby: to the nearest free spot within 40 yards.'),
('stuck nearby', 0, 'Syntax: .stuck nearby\r\nFrees you to the nearest free standing spot within 40 yards (walkable ground, room around and above you, not under water, away from enemy camps). Not in combat, dead, on a transport, vehicle or flight, or in a battleground; once every 5 minutes. No free spot: to the nearest town as .unstuck.'),
('stuck status', 0, 'Syntax: .stuck status\r\nSends your client the time left on both unstucks (the game menu shows it).');
