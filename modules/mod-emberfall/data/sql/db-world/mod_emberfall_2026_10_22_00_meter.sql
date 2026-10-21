-- mod-emberfall: the damage meter's commands (src/emberfall_meter.cpp; Emberfall docs/design/damage-meter.md §7).
DELETE FROM `command` WHERE `name` IN ('efmeter', 'efmeter status', 'efmeter reset');
INSERT INTO `command` (`name`, `security`, `help`) VALUES
('efmeter', 0, 'Syntax: .efmeter status | reset\r\nThe server''s damage meter of your group (or yourself).'),
('efmeter status', 0, 'Syntax: .efmeter status\r\nYour meter (group or solo), what your client was sent (bytes, bytes per second in fights), the current or last fight and overall per participant: damage, DPS, healing, overheal, absorbs, taken, received, interrupts, dispels, deaths, active time.'),
('efmeter reset', 0, 'Syntax: .efmeter reset\r\nClears the fights and overall of your meter. In a group only the leader may.');
