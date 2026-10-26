-- Emberfall talents (docs/design/talents.md §5.3, 2026-10-26, Nathanael): the class layout's strip is the player's to
-- buy, one point a slot in order (the parents), so the server buys nothing by itself any more: ef_talent_fill
-- (2026_10_26_00) goes. Emberfall's compiler no longer writes it.
DROP TABLE IF EXISTS `ef_talent_fill`;
