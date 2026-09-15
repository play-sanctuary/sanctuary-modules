--
-- mod-sanctuary-faction - the ranks inside a faction
--
-- RANK 0 IS THE LOWEST AND HIGHER IS SENIOR. This is deliberately the OPPOSITE of the
-- core's guild ranks, where 0 is the guild master, and the divergence is worth the
-- confusion it costs:
--
-- The whole point of the system is that a rank grants spells, and grants are cumulative
-- upward. "Everything ranks 2 and above may do" is a sentence that reads correctly and
-- compiles to `rank_id >= 2`. With the guild ordering the same sentence is `rank_id <= 2`,
-- and every query, every comparison and every future reader has to invert it in their
-- head. Getting that backwards once hands a recruit the captain's spells, and it would
-- look like it worked.
--
-- So: promote raises the number. `sanctuary_faction_rank_spell` is read as "this rank and
-- above", and May() is a `>=` test.
--
-- Ranks are per-faction and dense from 0. Nothing enforces density in SQL because a game
-- master editing ranks live will briefly leave gaps; the module tolerates them by treating
-- a missing rank as "no name, no powers" rather than refusing to load the faction, and
-- promotion moves to the next DEFINED rung rather than to rank + 1.
--
--
-- `permissions` is what a rank may do to the FACTION - invite, kick, promote, demote. It
-- is read only inside this module, and it is what makes a faction able to run its own
-- roster while a game master keeps the ladder. The bit meanings live in
-- SanctuaryFaction.h and are NOT duplicated here, because two copies of a bitmask drift
-- and the header is the one the compiler reads.
--
-- There is deliberately no second mask. An earlier draft of this table had a
-- `capabilities` column beside it, granting powers the other modules check for - shackling
-- without irons, opening any strongbox - and it was cut: what a faction gives its members
-- is a spell. One kind of reward is easier to balance and to explain than two, and a spell
-- in the spellbook is something the player can see, whereas a capability is an invisible
-- boolean that quietly changes what a door does.
--

CREATE TABLE IF NOT EXISTS `sanctuary_faction_rank` (
  `faction`     INT UNSIGNED     NOT NULL,
  `rank_id`     TINYINT UNSIGNED NOT NULL COMMENT '0 is the lowest; higher is senior',
  `name`        VARCHAR(32)      NOT NULL COMMENT 'Recruit, Bruiser, Underboss - the faction chooses',
  `permissions` INT UNSIGNED     NOT NULL DEFAULT 0 COMMENT 'SanctuaryFaction::Permission bits',
  PRIMARY KEY (`faction`, `rank_id`),
  CONSTRAINT `fk_faction_rank_faction` FOREIGN KEY (`faction`)
    REFERENCES `sanctuary_faction` (`id`) ON DELETE CASCADE
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_general_ci;

-- Removes the capabilities column from any database that got the first draft of this file.
-- Written as a conditional rather than a plain ALTER because a fresh install never had the
-- column, and an ALTER that fails there would stop the whole update run.
SET @drop := (
  SELECT COUNT(*) FROM `information_schema`.`COLUMNS`
  WHERE `TABLE_SCHEMA` = DATABASE()
    AND `TABLE_NAME` = 'sanctuary_faction_rank'
    AND `COLUMN_NAME` = 'capabilities'
);

SET @sql := IF(@drop > 0,
  'ALTER TABLE `sanctuary_faction_rank` DROP COLUMN `capabilities`',
  'DO 0');

PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
