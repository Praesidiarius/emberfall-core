-- mod-emberfall: companions (Emberfall docs/design/companions.md). AI party members for testing, summoned by a GM with
-- `.efparty`. The archetypes are data: an archetype names its creature, role, level scaling and spells; our own classes
-- replace the stock stand-ins by adding archetypes (or rows) with their spells. The stock spells and displays below only
-- exist on the stock world (ef-classes, stock-test); Emberfall's clean world keeps the tables empty (make_clean_world.sh
-- dumps no rows of them), and `.efparty` stays off there.

CREATE TABLE IF NOT EXISTS `ef_companion_archetype` (
  `id` TINYINT UNSIGNED NOT NULL,
  `name` VARCHAR(24) NOT NULL COMMENT '.efparty add <role> <name>',
  `role` TINYINT UNSIGNED NOT NULL COMMENT '1 tank, 2 healer, 3 melee DPS, 4 ranged DPS',
  `entry` INT UNSIGNED NOT NULL COMMENT 'creature_template (ScriptName npc_ef_companion)',
  `unit_class` TINYINT UNSIGNED NOT NULL DEFAULT 1 COMMENT 'creature_classlevelstats class of health, armor and damage',
  `health_mod` FLOAT NOT NULL DEFAULT 1 COMMENT 'x the class base health of the level',
  `mana_mod` FLOAT NOT NULL DEFAULT 0 COMMENT 'mana pool = x the paladin base mana of the level (spell costs are % of that base)',
  `armor_mod` FLOAT NOT NULL DEFAULT 1 COMMENT 'x the class base armor',
  `damage_mod` FLOAT NOT NULL DEFAULT 1 COMMENT 'white melee DPS = x a normal creature of the level',
  `ranged_mod` FLOAT NOT NULL DEFAULT 0 COMMENT 'ranged weapon DPS (the shoot spell, every 2.5 s) = x a normal creature',
  `attack_ms` INT UNSIGNED NOT NULL DEFAULT 2000 COMMENT 'melee swing time',
  `threat_mod` FLOAT NOT NULL DEFAULT 1 COMMENT 'damage threat multiplier (the tank)',
  `free_cost` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '1: abilities cost nothing (rage/energy kits)',
  `uses_mana` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `range` FLOAT NOT NULL DEFAULT 0 COMMENT '0 melee, else the distance it fights from',
  `comment` VARCHAR(255) NOT NULL DEFAULT '',
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall companions: archetypes';

CREATE TABLE IF NOT EXISTS `ef_companion_spell` (
  `archetype` TINYINT UNSIGNED NOT NULL,
  `spell` INT UNSIGNED NOT NULL COMMENT 'the spell, or the first rank of a chain (the highest rank the level reaches is cast)',
  `kind` VARCHAR(16) NOT NULL COMMENT 'taunt, aoe_threat, threat, nuke, aoe, behind, shoot, dot, heal_fast, heal_big, heal_small, hot, shield, dispel, res',
  `priority` TINYINT UNSIGNED NOT NULL DEFAULT 1 COMMENT 'lower first (the offensive rotation runs every row in this order)',
  `min_level` TINYINT UNSIGNED NOT NULL DEFAULT 1,
  `max_level` TINYINT UNSIGNED NOT NULL DEFAULT 255,
  `cooldown_ms` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'on top of the spell''s own cooldowns',
  `min_targets` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'aoe / aoe_threat: enemies within 8 yd',
  `comment` VARCHAR(255) NOT NULL DEFAULT '',
  PRIMARY KEY (`archetype`, `spell`, `kind`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall companions: the archetypes'' abilities';

-- The stock stand-in set (companions.md §4 has the numbers): warrior tank, priest healer, mage and hunter ranged, rogue melee.
DELETE FROM `ef_companion_archetype` WHERE `id` BETWEEN 1 AND 5;
INSERT INTO `ef_companion_archetype` (`id`, `name`, `role`, `entry`, `unit_class`, `health_mod`, `mana_mod`, `armor_mod`, `damage_mod`, `ranged_mod`, `attack_ms`, `threat_mod`, `free_cost`, `uses_mana`, `range`, `comment`) VALUES
(1, 'warrior', 1, 990001, 1, 2.6, 0,   2.0, 1.5, 0,   2000, 2.5, 1, 0, 0,  'tank: sword and shield, Taunt, Thunder Clap, Demoralizing Shout, Sunder Armor, Heroic Strike, Cleave'),
(2, 'priest',  2, 990002, 2, 1.6, 4.0, 0.8, 0.5, 0,   2000, 1.0, 0, 1, 28, 'healer: Flash Heal/Lesser Heal, Heal/Greater Heal, Renew, Power Word: Shield, Dispel Magic, Cure Disease, Resurrection, Smite'),
(3, 'mage',    4, 990003, 8, 1.7, 2.5, 0.6, 0.5, 0,   2000, 1.0, 0, 1, 28, 'ranged caster: Fire Blast, Fireball, Frostbolt'),
(4, 'rogue',   3, 990004, 1, 1.7, 0,   1.0, 2.6, 0,   1700, 1.0, 1, 0, 0,  'melee: Backstab from behind, Sinister Strike'),
(5, 'hunter',  4, 990005, 2, 1.8, 2.0, 1.0, 0.6, 2.2, 2000, 1.0, 0, 1, 28, 'ranged physical: Shoot, Serpent Sting, Arcane Shot, Multi-Shot');

DELETE FROM `ef_companion_spell` WHERE `archetype` BETWEEN 1 AND 5;
INSERT INTO `ef_companion_spell` (`archetype`, `spell`, `kind`, `priority`, `min_level`, `max_level`, `cooldown_ms`, `min_targets`, `comment`) VALUES
-- warrior (tank)
(1, 355,  'taunt',      1, 10, 255, 8000,  0, 'Taunt'),
(1, 6343, 'aoe_threat', 2, 6,  255, 6000,  2, 'Thunder Clap'),
(1, 1160, 'aoe_threat', 3, 14, 255, 25000, 2, 'Demoralizing Shout'),
(1, 7386, 'threat',     4, 10, 255, 4500,  0, 'Sunder Armor'),
(1, 845,  'aoe',        5, 20, 255, 6000,  2, 'Cleave'),
(1, 78,   'threat',     6, 1,  255, 4000,  0, 'Heroic Strike'),
-- priest (healer)
(2, 2061, 'heal_fast',  1, 20, 255, 0, 0, 'Flash Heal'),
(2, 2050, 'heal_fast',  2, 1,  19,  0, 0, 'Lesser Heal (before Flash Heal)'),
(2, 2060, 'heal_big',   1, 40, 255, 0, 0, 'Greater Heal'),
(2, 2054, 'heal_big',   2, 16, 39,  0, 0, 'Heal'),
(2, 2050, 'heal_big',   3, 1,  15,  0, 0, 'Lesser Heal (before Heal)'),
(2, 2061, 'heal_small', 1, 20, 255, 0, 0, 'Flash Heal'),
(2, 2050, 'heal_small', 2, 1,  19,  0, 0, 'Lesser Heal'),
(2, 139,  'hot',        1, 8,  255, 0, 0, 'Renew'),
(2, 17,   'shield',     1, 6,  255, 0, 0, 'Power Word: Shield (Weakened Soul is the lockout)'),
(2, 527,  'dispel',     1, 18, 255, 0, 0, 'Dispel Magic'),
(2, 528,  'dispel',     2, 14, 255, 0, 0, 'Cure Disease'),
(2, 2006, 'res',        1, 10, 255, 0, 0, 'Resurrection'),
(2, 585,  'nuke',       1, 1,  255, 0, 0, 'Smite (idle with mana only)'),
-- mage (ranged caster)
(3, 2136, 'nuke',       1, 6,  255, 0, 0, 'Fire Blast'),
(3, 133,  'nuke',       2, 1,  255, 0, 0, 'Fireball'),
(3, 116,  'nuke',       3, 4,  255, 0, 0, 'Frostbolt'),
-- rogue (melee)
(4, 53,   'behind',     1, 4,  255, 4000, 0, 'Backstab'),
(4, 1752, 'nuke',       2, 1,  255, 3000, 0, 'Sinister Strike'),
-- hunter (ranged physical)
(5, 2643, 'aoe',        1, 18, 255, 0,    2, 'Multi-Shot'),
(5, 1978, 'dot',        2, 4,  255, 0,    0, 'Serpent Sting'),
(5, 3044, 'nuke',       3, 6,  255, 0,    0, 'Arcane Shot'),
(5, 3018, 'shoot',      4, 1,  255, 2500, 0, 'Shoot (ranged weapon)');

-- The creatures: stock trainer looks and weapons as stand-ins. Level, faction, stats and power come from the summon
-- (EfCompanions.cpp ApplyScaling); RegenHealth 0 (the AI eats between pulls).
DELETE FROM `creature_template` WHERE `entry` BETWEEN 990001 AND 990005;
INSERT INTO `creature_template` (`entry`, `name`, `subname`, `minlevel`, `maxlevel`, `exp`, `faction`, `npcflag`, `speed_walk`, `speed_run`,
  `rank`, `dmgschool`, `DamageModifier`, `BaseAttackTime`, `RangeAttackTime`, `BaseVariance`, `RangeVariance`, `unit_class`, `unit_flags`,
  `unit_flags2`, `type`, `AIName`, `MovementType`, `HealthModifier`, `ManaModifier`, `ArmorModifier`, `ExperienceModifier`, `RegenHealth`,
  `flags_extra`, `ScriptName`, `VerifiedBuild`) VALUES
(990001, 'Warrior Companion', 'Tank',   1, 1, 0, 35, 0, 1, 1.14286, 0, 0, 1, 2000, 2000, 1, 1, 1, 0, 2048, 7, '', 0, 1, 1, 1, 0, 0, 0, 'npc_ef_companion', 0),
(990002, 'Priest Companion',  'Healer', 1, 1, 0, 35, 0, 1, 1.14286, 0, 0, 1, 2000, 2000, 1, 1, 2, 0, 2048, 7, '', 0, 1, 1, 1, 0, 0, 0, 'npc_ef_companion', 0),
(990003, 'Mage Companion',    'Damage', 1, 1, 0, 35, 0, 1, 1.14286, 0, 0, 1, 2000, 2000, 1, 1, 8, 0, 2048, 7, '', 0, 1, 1, 1, 0, 0, 0, 'npc_ef_companion', 0),
(990004, 'Rogue Companion',   'Damage', 1, 1, 0, 35, 0, 1, 1.14286, 0, 0, 1, 1700, 2000, 1, 1, 1, 0, 2048, 7, '', 0, 1, 1, 1, 0, 0, 0, 'npc_ef_companion', 0),
(990005, 'Hunter Companion',  'Damage', 1, 1, 0, 35, 0, 1, 1.14286, 0, 0, 1, 2000, 2000, 1, 1, 2, 0, 2048, 7, '', 0, 1, 1, 1, 0, 0, 0, 'npc_ef_companion', 0);

DELETE FROM `creature_template_model` WHERE `CreatureID` BETWEEN 990001 AND 990005;
INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`) VALUES
(990001, 0, 3343, 1, 1, 0),  -- Llane Beshere
(990002, 0, 3344, 1, 1, 0),  -- Priestess Anetta
(990003, 0, 5001, 1, 1, 0),  -- Khelden Bremen
(990004, 0, 3351, 1, 1, 0),  -- Jorik Kerridan
(990005, 0, 3395, 1, 1, 0);  -- Thorgas Grimson

DELETE FROM `creature_equip_template` WHERE `CreatureID` BETWEEN 990001 AND 990005;
INSERT INTO `creature_equip_template` (`CreatureID`, `ID`, `ItemID1`, `ItemID2`, `ItemID3`, `VerifiedBuild`) VALUES
(990001, 1, 1896, 1961, 0, 0),
(990002, 1, 1907, 0, 0, 0),
(990003, 1, 1907, 0, 0, 0),
(990004, 1, 2184, 2184, 0, 0),
(990005, 1, 0, 0, 2552, 0);

DELETE FROM `command` WHERE `name` IN ('efparty', 'efparty add', 'efparty list', 'efparty remove', 'efparty stay', 'efparty follow',
  'efparty assist', 'efparty level', 'efparty stats', 'efparty threat', 'efparty reload');
INSERT INTO `command` (`name`, `security`, `help`) VALUES
('efparty', 2, 'Syntax: .efparty [$subcommand]\r\nEmberfall companions: AI party members for testing (add, list, remove, stay, follow, assist, level, stats, threat, reload). Without a subcommand: the list.'),
('efparty add', 2, 'Syntax: .efparty add tank|healer|dps|melee|ranged [$archetype] [$count]\r\nSummons companions of the role at your level (dps: the melee and ranged archetypes in turn). Archetypes: warrior (tank), priest (healer), mage and hunter (ranged), rogue (melee).'),
('efparty list', 2, 'Syntax: .efparty list\r\nYour companions: role, level, health, mana, state, target, damage and healing done.'),
('efparty remove', 2, 'Syntax: .efparty remove $n|all\r\nDismisses companion $n (as in .efparty list) or all of them.'),
('efparty stay', 2, 'Syntax: .efparty stay\r\nOut of combat the companions stay where they are.'),
('efparty follow', 2, 'Syntax: .efparty follow\r\nOut of combat the companions follow you in formation (the default).'),
('efparty assist', 2, 'Syntax: .efparty assist on|off\r\non (the default): the DPS attack your target, else the tank''s; off: they only fight what attacks them.'),
('efparty level', 2, 'Syntax: .efparty level\r\nScales every companion to your level again (a level change does it by itself).'),
('efparty stats', 2, 'Syntax: .efparty stats [reset]\r\nThe test numbers since the last reset: fights, kills, kill time, share of mob time on the tank, aggro losses, the tank''s threat lead, deaths, the healer''s lowest mana, damage and healing per companion.'),
('efparty threat', 2, 'Syntax: .efparty threat\r\nEvery engaged mob''s threat list (the tank marked *).'),
('efparty reload', 3, 'Syntax: .efparty reload\r\nReads the Emberfall.Companions.* config and the ef_companion tables again.');
