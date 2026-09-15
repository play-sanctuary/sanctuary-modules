--
-- mod-sanctuary-faction - what a rank teaches
--
-- One row per (faction, rank, spell). Read as "this rank AND ABOVE", so a spell put on
-- rank 1 is held by ranks 1, 2, 3... and does not have to be repeated up the ladder. That
-- is what makes editing a ladder survivable: raising a rank's entry moves the spell for
-- everyone at once instead of needing every higher row corrected too.
--
--
-- WHAT MAY GO IN `spell`, and the one thing that cannot.
--
-- Any spell the CLIENT already has a row for - which is every stock 3.3.5a spell, tens of
-- thousands of them. Player::learnSpell sends SMSG_LEARNED_SPELL and the client puts it in
-- the book immediately: no trainer, no relog, no item, and it persists in `character_spell`
-- by itself.
--
-- What cannot go here is a spell that does not exist client-side. Sanctuary's own spells
-- are 81000-81011 and all are allocated; a new one needs a row in patch-enUS-4.MPQ, which
-- is a client patch and a redownload. Nothing about this table needs one - that is the
-- point of drawing rank rewards from the stock list.
--
--
-- THE TRAP, which is placement rather than learning.
--
-- The client files a known spell into a spellbook tab through its SkillLineAbility row, and
-- Player::addSpell only grants the underlying skill line when AcquireMethod is
-- LEARNED_ON_SKILL_LEARN (Player.cpp:3378). A spell tied to a skill line the character does
-- not have can therefore end up genuinely known and castable from a macro, but never DRAWN
-- in the spellbook. Spells with no SkillLineAbility row at all land in General and are
-- always safe.
--
-- It is a property of the individual spell, so check each one before putting it on a rank:
-- `.learn <id>` on a NON-game-master character of the wrong class runs the identical code
-- path this module uses. If it shows up there it will show up here.
--
--
-- Revoking is NOT done from this table. See sanctuary_faction_granted for why.
--

CREATE TABLE IF NOT EXISTS `sanctuary_faction_rank_spell` (
  `faction` INT UNSIGNED     NOT NULL,
  `rank_id` TINYINT UNSIGNED NOT NULL COMMENT 'granted at this rank and every rank above it',
  `spell`   INT UNSIGNED     NOT NULL COMMENT 'must exist in the client, so: a stock spell',
  PRIMARY KEY (`faction`, `rank_id`, `spell`),
  CONSTRAINT `fk_faction_rank_spell_faction` FOREIGN KEY (`faction`)
    REFERENCES `sanctuary_faction` (`id`) ON DELETE CASCADE
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_general_ci;
