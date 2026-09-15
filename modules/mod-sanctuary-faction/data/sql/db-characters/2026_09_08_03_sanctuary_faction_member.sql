--
-- mod-sanctuary-faction - who belongs to what
--
-- ONE FACTION PER CHARACTER, and the primary key on `guid` is what says so. It is not an
-- incidental choice of key: it is the rule, written where it cannot be forgotten, because
-- the reconciliation in SanctuaryFaction.cpp computes "the spells this character should
-- have" from a single rank. Two memberships would make that question ambiguous at exactly
-- the moment it has to be answered, at login, with no player around to ask.
--
-- A character may hold a guild AND a faction at the same time - they are unrelated systems
-- and the core's guild tables are untouched by this module. That is the whole reason this
-- is not built on guilds.
--
-- Membership is per CHARACTER, not per account. An alt is a different person on this realm;
-- that is the premise mod-sanctuary-identity is built on and this follows it.
--
-- No foreign key to `characters`. Deliberate: the module cleans up after a deleted
-- character itself, and a constraint here would make a routine character delete fail
-- against a table the core knows nothing about. The faction constraint stays, because
-- disbanding a faction genuinely should take its roster with it.
--

CREATE TABLE IF NOT EXISTS `sanctuary_faction_member` (
  `guid`    INT UNSIGNED     NOT NULL COMMENT 'character guid',
  `faction` INT UNSIGNED     NOT NULL,
  `rank_id` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `joined`  INT UNSIGNED     NOT NULL DEFAULT 0 COMMENT 'unix time',
  PRIMARY KEY (`guid`),
  KEY `faction` (`faction`),
  CONSTRAINT `fk_faction_member_faction` FOREIGN KEY (`faction`)
    REFERENCES `sanctuary_faction` (`id`) ON DELETE CASCADE
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_general_ci;
