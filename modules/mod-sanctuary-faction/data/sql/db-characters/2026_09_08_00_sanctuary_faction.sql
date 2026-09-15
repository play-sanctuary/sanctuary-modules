--
-- mod-sanctuary-faction - the factions themselves
--
-- WHY THE CHARACTERS DATABASE, not the world one.
--
-- Everything else this module could be mistaken for lives in db-world: strongbox
-- placements, item templates, the spell rows. Those are *content*, shipped with the
-- module, and a world reimport is supposed to restore them exactly as written.
--
-- A faction is not content. It is created at runtime by a game master typing
-- `.faction create`, it belongs to the players in it, and it must survive the world
-- database being dropped and rebuilt from the module SQL - which is a thing that happens
-- on this realm every time the migrations are re-run against a fresh world.
--
-- That is the same reason the core keeps `guild` in the characters database rather than
-- the world one, and factions are the same kind of object. Putting them in db-world would
-- work perfectly until the first world reimport silently deleted every faction on the
-- realm while leaving every membership row pointing at nothing.
--
-- `name` is ours, not the client's. Unlike a title (ChrTitles.dbc, client-side, capped at
-- 192 bits and needing a patch per title) or a spell name, a faction name is a string this
-- module owns and sends to the addon. There is no id budget and no patch: names are free,
-- unlimited, and editable on the spot.
--
-- Not to be confused with `Faction.dbc` / `FactionTemplate.dbc`, which decide who may
-- attack whom. Nothing in this module touches those. See README.md.
--

CREATE TABLE IF NOT EXISTS `sanctuary_faction` (
  `id`          INT UNSIGNED    NOT NULL AUTO_INCREMENT,
  `name`        VARCHAR(48)     NOT NULL COMMENT 'shown to players; unique so it can be typed at a command',
  `description` VARCHAR(255)    NOT NULL DEFAULT '' COMMENT 'a line of flavour for the roster window',
  `founded`     INT UNSIGNED    NOT NULL DEFAULT 0 COMMENT 'unix time',
  `founder`     INT UNSIGNED    NOT NULL DEFAULT 0 COMMENT 'character guid, 0 when a game master made it',
  PRIMARY KEY (`id`),
  UNIQUE KEY `name` (`name`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_general_ci;
