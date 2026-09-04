--
-- mod-sanctuary-outlaw
--
-- One row per outlawed character; absence means an ordinary citizen. The flags themselves
-- are not saved by the core - UNIT_FIELD_BYTES_2 is not a character column and the faction
-- is recomputed from the race on every login - so this table is the only thing that
-- survives a logout, and the module reapplies everything from it.
--
-- `release_at` is stored, but deliberately cleared on login rather than honoured: logging
-- out during the stand-down countdown and back in after it would be exactly the escape the
-- countdown exists to prevent.
--

CREATE TABLE IF NOT EXISTS `sanctuary_outlaw` (
  `guid`       INT UNSIGNED NOT NULL COMMENT 'character guid',
  `since`      INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'unix time they were first flagged',
  `expires`    INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'unix time the sentence ends, 0 = until they stand down',
  `release_at` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'unix time a requested stand-down takes effect, 0 = none',
  PRIMARY KEY (`guid`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_general_ci;
