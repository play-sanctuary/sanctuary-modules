--
-- Short in-character descriptions.
--
-- Deliberately four fixed columns rather than a free-form blob: the shape of a profile is
-- part of the design, and a schema that cannot hold a biography is the cheapest way to
-- keep it that way. Lengths are the module's own cap plus headroom for a config raise.
--
CREATE TABLE IF NOT EXISTS `character_profiles` (
  `guid`       INT UNSIGNED NOT NULL,
  `appearance` VARCHAR(200) NOT NULL DEFAULT '',
  `injuries`   VARCHAR(200) NOT NULL DEFAULT '',
  `manner`     VARCHAR(200) NOT NULL DEFAULT '',
  `detail`     VARCHAR(200) NOT NULL DEFAULT '',
  `updated_at` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
