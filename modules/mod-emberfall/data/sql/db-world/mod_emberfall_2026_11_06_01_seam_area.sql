-- mod-emberfall: the seam guard (src/emberfall_seam.cpp; Emberfall docs/design/kindred-map-split.md §3.4 item 4). The
-- Kindred's two maps (1 north, 754 south) each draw a one-tile margin of the other half; its terrain carries a "beyond
-- the seam" area, and a player who enters it is put back on their own side. Emberfall's compiler writes the rows,
-- emptied and refilled by world.sql.
CREATE TABLE IF NOT EXISTS `ef_seam_area` (
  `area_id` INT UNSIGNED NOT NULL COMMENT 'AreaTable: a "beyond the seam" area (zone or subarea)',
  `map_id` SMALLINT UNSIGNED NOT NULL COMMENT 'the map whose margin it is',
  PRIMARY KEY (`area_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Emberfall seam guard: the areas beyond the seam, per map';
