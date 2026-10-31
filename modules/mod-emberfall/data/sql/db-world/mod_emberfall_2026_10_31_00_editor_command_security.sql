-- mod-emberfall: GM level 1 runs no GM command (Emberfall docs/design/world-editor.md §19). Level 1 (SEC_MODERATOR) is
-- the world editor's contributor on the normal realm and must play exactly as a player. The core takes a command's
-- level from this table when it has a row, over the C++ table's (ChatCommand.cpp LoadCommandMap "Overriding", RBAC
-- permissions included), and AzerothCore's rows put 69 commands at 1 (.gm, .gm on, .go *, .appear, .gps, .lookup *,
-- .list *, .morph, .instance, .lfg, .whispers, ...): every one goes to 2 (game master). `.efedit` would stay; it has no
-- level-1 command today.
--
-- tests/command_security_check.py fails when any command (this table after every update, or a C++ SEC_MODERATOR
-- entry) ends at level 1: an upstream merge that adds one needs a new update like this one. mod-emberfall's
-- emberfall_editor.cpp also hides any level-1 command from level-1 accounts at run time.

UPDATE `command` SET `security` = 2 WHERE `security` = 1 AND `name` NOT LIKE 'efedit%';
