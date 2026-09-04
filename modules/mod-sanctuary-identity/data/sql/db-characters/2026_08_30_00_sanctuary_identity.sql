-- mod-sanctuary-identity: who has been introduced to whom
--
-- One row means `knower` may see `known`'s real name. Introductions are one way: telling somebody
-- your name does not tell you theirs, so a mutual acquaintance is two rows.
--
-- Kept out of the core `characters` table so the module can be removed without touching it.

CREATE TABLE IF NOT EXISTS `character_introductions` (
  `knower_guid` INT UNSIGNED NOT NULL COMMENT 'character who may see the name',
  `known_guid`  INT UNSIGNED NOT NULL COMMENT 'character whose name may be seen',
  `introduced`  TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`knower_guid`, `known_guid`),
  KEY `idx_knower` (`knower_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;
