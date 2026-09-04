--
-- Disguises: a second alias a character can put on, hiding them even from people who have
-- already been introduced to them.
--
-- The disguise alias lives in `character_aliases` alongside the stranger one rather than in
-- a table of its own, so both share the single UNIQUE index on `alias`. That shared
-- namespace is the point: without it a disguise could be allocated the same "Hooded Orc" as
-- some other character's stranger alias, and two different people would read identically.
--
ALTER TABLE `character_aliases`
  ADD COLUMN `kind` TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER `guid`,
  DROP PRIMARY KEY,
  ADD PRIMARY KEY (`guid`, `kind`);

--
-- Presence is the whole state: a row means the disguise is currently up. Kept separate from
-- the alias so taking a disguise off and putting it back on returns the same identity rather
-- than minting a new one.
--
CREATE TABLE IF NOT EXISTS `character_disguised` (
  `guid` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
