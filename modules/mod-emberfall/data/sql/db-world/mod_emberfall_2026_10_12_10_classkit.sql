-- mod-emberfall: class kits (Emberfall docs/design/class-kit-pipeline.md). Schema only: Emberfall's compiler writes
-- the rows (dist/classkit/classkit.sql for the ef-classes profile). src/classkit/EfClassKit.h says what the module does
-- with them.
CREATE TABLE IF NOT EXISTS `ef_class_kit` (
  `class` TINYINT UNSIGNED NOT NULL COMMENT 'engine class ID',
  `skill_line` SMALLINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'the kit''s class skill line (SkillLine, category class)',
  `stock_family` SMALLINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'SpellFamilyName of the stock kit the class replaces (its spells are taken away)',
  PRIMARY KEY (`class`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall class kits: the classes that play our kit';

CREATE TABLE IF NOT EXISTS `ef_class_kit_spell` (
  `class` TINYINT UNSIGNED NOT NULL,
  `spell` INT UNSIGNED NOT NULL,
  `level` TINYINT UNSIGNED NOT NULL DEFAULT 1 COMMENT 'learn level',
  `source` TINYINT UNSIGNED NOT NULL DEFAULT 2 COMMENT '1 start, 2 trainer, 3 spec baseline, 4 talent, 5 helper',
  PRIMARY KEY (`class`, `spell`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall class kits: every spell of a kit';

DELETE FROM `command` WHERE `name` IN ('efkit', 'efkit dump', 'efkit purge');
INSERT INTO `command` (`name`, `security`, `help`) VALUES
('efkit', 2, 'Syntax: .efkit [$subcommand]\r\nEmberfall class kits: the selected player''s (or your) class kit, its spells and whether they are known (dump, the default), or purge.'),
('efkit dump', 2, 'Syntax: .efkit dump\r\nEmberfall class kits: the selected player''s (or your) class kit, its spells and whether they are known, and how many stock-family spells are left.'),
('efkit purge', 2, 'Syntax: .efkit purge\r\nEmberfall class kits: take away what is left of the stock kit now (stock class skill lines, stock-family spells, buttons of unknown spells), as login and level changes do.');
