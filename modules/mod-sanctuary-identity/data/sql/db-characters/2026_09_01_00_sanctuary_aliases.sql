--
-- The name a stranger is known by.
--
-- Allocated once, on first login, and never changed: players refer to each other by these
-- ("the Hooded Orc was asking about you"), so an alias that drifted between sessions would
-- be worse than no alias at all.
--
-- UNIQUE on alias is load-bearing rather than tidiness. Two strangers sharing a name would
-- be indistinguishable to the voice speaking indicator, which can only match a nameplate by
-- its text because a 3.3.5a plate carries no unit token.
--
CREATE TABLE IF NOT EXISTS `character_aliases` (
  `guid`  INT UNSIGNED NOT NULL,
  `alias` VARCHAR(48)  NOT NULL,
  PRIMARY KEY (`guid`),
  UNIQUE KEY `uk_alias` (`alias`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
