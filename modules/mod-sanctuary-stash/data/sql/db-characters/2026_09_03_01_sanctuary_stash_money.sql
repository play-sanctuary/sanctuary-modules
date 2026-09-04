--
-- mod-sanctuary-stash - coin in the box
--
-- Its own table rather than a column on `sanctuary_stash`, because that one is the
-- DEFINITION of a strongbox - where it stands, what opens it, how big it is - and lives in
-- the world database with the rest of the world's furniture. What is inside a box is state,
-- it changes every time somebody reaches in, and it belongs beside the items in the
-- characters database. Mixing the two would mean a world-database row that players write to.
--
-- Copper, as everywhere else in the core. Player money is a uint32 and the core caps it at
-- 0x7FFFFFFE, so a box can hold as much as a player can carry and no more - the deposit
-- path refuses anything that would take it past that rather than wrapping.
--

CREATE TABLE IF NOT EXISTS `sanctuary_stash_money` (
  `stash` INT UNSIGNED NOT NULL COMMENT 'gameobject spawn guid of the strongbox',
  `money` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'copper held',
  PRIMARY KEY (`stash`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_general_ci;
