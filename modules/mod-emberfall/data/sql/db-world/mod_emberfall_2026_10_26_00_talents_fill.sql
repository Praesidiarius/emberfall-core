-- Emberfall talents (docs/design/talents.md §5.3, 2026-10-26): nodes the server buys by itself, in `ord` order, as far as
-- the rules and the class budget allow (the class layout's strip: the Paladin's class tree, each level's point to the
-- next slot; EfTalentModel::Fill). Schema only; Emberfall's compiler writes the rows (talents/sql.py).
CREATE TABLE IF NOT EXISTS `ef_talent_fill` (
  `node` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`node`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Emberfall talents: nodes the server fills in order';
