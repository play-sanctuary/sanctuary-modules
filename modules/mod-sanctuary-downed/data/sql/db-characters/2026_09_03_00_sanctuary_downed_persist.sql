--
-- mod-sanctuary-downed - the clock survives a logout
--
-- Leaving used to end the downed state outright: OnPlayerLogout stood the player up and
-- forgot them. And because going down calls CombatStop, a downed player is out of combat -
-- so there was not even the twenty-second logout timer to sit through. Lose a fight, quit,
-- come back on your feet. Every other rule in this module is an argument about who gets to
-- decide when you get up, and that one had the player deciding on their own.
--
-- One row, written at logout and taken straight back out at login. It is a handover between
-- two sessions rather than a record of anything: left behind, it would put somebody back on
-- the floor every time they logged in, however they got up the first time.
--
-- The auras are NOT the source of truth. They are removed at logout like everything else
-- and rebuilt from these numbers at login, so the icon and the module cannot drift apart -
-- and a row that is missing after a crash means the auras found on login are leftovers, to
-- be stripped, which is exactly what already happened.
--
-- The timer does not run while they are offline. There is nobody to rescue them and nobody
-- to finish them, and logging in to a corpse is a worse answer than logging in to the same
-- ten minutes. Waiting it out is no longer an escape either way: they come back down.
--

CREATE TABLE IF NOT EXISTS `sanctuary_downed` (
  `guid`          INT UNSIGNED NOT NULL COMMENT 'character guid',
  `remaining_ms`  INT UNSIGNED NOT NULL COMMENT 'bleed-out time left when they logged out',
  `immune_ms`     INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'grace left; almost always 0',
  `downed_health` INT UNSIGNED NOT NULL DEFAULT 1 COMMENT 'the health they are pinned to',
  -- Only a player is kept. A creature guid means nothing after a restart, and crediting
  -- the wolf that mauled somebody an hour ago is not worth the lookup.
  `feller`        INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'character guid of whoever put them down, 0 if none',
  PRIMARY KEY (`guid`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_general_ci;
