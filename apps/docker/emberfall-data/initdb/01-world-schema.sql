-- Emberfall own-data profile (docker-compose.emberfall-data.yml): create the world schema empty
-- on the first start of ef-database. dbimport (auth + characters only) and the worldserver open it;
-- Emberfall's tools/server/make_clean_world.sh fills it.
CREATE DATABASE IF NOT EXISTS `acore_world_ef` DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
