--
-- Which hour each character last chose to stand in.
--
-- The core keeps no phase column on `characters`, so a phase mask lives only in memory and
-- is lost at logout. Without this table a player who asked Zidormi for the present day
-- would find themselves back in the past next session, with nothing on screen to say why.
--
-- One row per character, only for those who have actually spoken to her. Absence means
-- "never asked", which is the default phase, so there is nothing to write for most people.
--

CREATE TABLE IF NOT EXISTS `sanctuary_timeline` (
  `guid`       INT UNSIGNED NOT NULL COMMENT 'character guid',
  `phase_mask` INT UNSIGNED NOT NULL COMMENT 'the mask Zidormi last set for them',
  `changed`    INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'unix time of that choice',
  PRIMARY KEY (`guid`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_general_ci;
