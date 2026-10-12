-- Emberfall talents WP M6 (docs/design/talents.md §1.4): replacing nodes. A spell the core taught (a trainer, a skill
-- line) that an invested replacing node replaces is removed by the module and kept here with source 3, so it is
-- learned back when the node is refunded or the spec changes. Only the column's comment changes.
ALTER TABLE `ef_character_granted`
  MODIFY COLUMN `source` TINYINT UNSIGNED NOT NULL DEFAULT 2 COMMENT '1 baseline, 2 node, 3 replaced (taken by a replacing node, M6)';
