-- mod-emberfall: help texts of the GM commands as of WPs M4/M5 (Emberfall docs/design/talents.md §3.2), so that
-- `.help ef` and `.ef` list them; replaces the rows of mod_emberfall_2026_10_11_00_talents.sql.
DELETE FROM `command` WHERE `name` IN ('ef', 'ef talents', 'ef talents dump', 'ef talents reset', 'ef talents reload', 'ef spec', 'ef send');
INSERT INTO `command` (`name`, `security`, `help`) VALUES
('ef', 2, 'Syntax: .ef $subcommand\r\nEmberfall commands. Type .ef to see the list of possible subcommands or .help ef $subcommand to see info on subcommands.'),
('ef talents', 2, 'Syntax: .ef talents $subcommand\r\nEmberfall specs and talents: dump, reset, reload.'),
('ef talents dump', 2, 'Syntax: .ef talents dump\r\nEmberfall talents: the selected player''s (or your) spec, loadouts, granted spells, the auras of talent and spec spells, stored bars and a running switch.'),
('ef talents reset', 2, 'Syntax: .ef talents reset\r\nEmberfall talents: reset the active loadout of the selected player''s (or your) active spec.'),
('ef talents reload', 3, 'Syntax: .ef talents reload\r\nEmberfall talents: reload the ef_* world tables; online players are re-validated at their next request.'),
('ef spec', 2, 'Syntax: .ef spec #specId\r\nEmberfall talents: switch the selected player (or you) to #specId at once, without the switch cast (spells and action bars as a switch; cancels a running switch cast).'),
('ef send', 2, 'Syntax: .ef send $body\r\nEmberfall talents: handle $body as an EFT addon message from your own client (test aid).');
