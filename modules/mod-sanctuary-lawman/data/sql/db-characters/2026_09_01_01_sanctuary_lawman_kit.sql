--
-- mod-sanctuary-lawman - remembering that the shackle kit was handed over.
--
-- The Writ of Accusation is soulbound, so re-granting it at every login costs nothing. The
-- shackles and key are deliberately tradeable - passing them on is the point - which makes
-- "give them away, relog, get another" a farm. So the row remembers, and only a game master
-- can replace a set that was genuinely lost.
--
-- A plain ADD COLUMN rather than a guarded one: MySQL 8 has no ADD COLUMN IF NOT EXISTS
-- (that is MariaDB), and the core's updater applies each file exactly once by hash, so
-- there is nothing to guard against.
--

ALTER TABLE `sanctuary_lawman`
  ADD COLUMN `kit_issued` TINYINT UNSIGNED NOT NULL DEFAULT 0
  COMMENT 'shackles and key have been handed over once';
