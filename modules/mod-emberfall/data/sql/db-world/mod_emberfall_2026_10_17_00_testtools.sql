-- mod-emberfall: help texts of the test tools (src/emberfall_testtools.cpp; off unless Emberfall.TestTools.Enable = 1).
DELETE FROM `command` WHERE `name` IN ('eftest', 'eftest level', 'eftest foe', 'eftest dummy', 'eftest clear', 'eftest refresh');
INSERT INTO `command` (`name`, `security`, `help`) VALUES
('eftest', 0, 'Syntax: .eftest [$subcommand]\r\nEmberfall test tools (a test realm only): level, foe, dummy, clear, refresh. Without a subcommand: the list.'),
('eftest level', 0, 'Syntax: .eftest level #level\r\nSets your own level (1 to the realm''s maximum), XP 0, full health and mana.'),
('eftest foe', 0, 'Syntax: .eftest foe [#level] [#count] [idle] [hp#factor]\r\nSummons 1-5 training foes of that level (default: yours) in front of you. They attack you (idle: they stand at your feet until engaged; hp1.6: 1.6 x their health) and are gone after 5 minutes or when dead.'),
('eftest dummy', 0, 'Syntax: .eftest dummy [#level]\r\nSummons a training dummy of that level (default: yours). It stands still, never hits back and never dies; gone after 10 minutes.'),
('eftest clear', 0, 'Syntax: .eftest clear\r\nYour training foes and dummies are gone.'),
('eftest refresh', 0, 'Syntax: .eftest refresh\r\nFull health and mana; your spell cooldowns end.');
