--
-- mod-sanctuary-lawman
--
-- One row per character holding the office; absence means they hold none. Nothing here is
-- saved by the core - the tabard is a visible-item field the client rebuilds from the real
-- equipment on every login, and duty is not a concept the core has - so this table is the
-- only thing that survives a logout and the module reapplies everything from it.
--

CREATE TABLE IF NOT EXISTS `sanctuary_lawman` (
  `guid`    INT UNSIGNED NOT NULL COMMENT 'character guid',
  `granted` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'unix time the office was given',
  `on_duty` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'wearing the tabard and carrying the writ',
  PRIMARY KEY (`guid`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_general_ci;
