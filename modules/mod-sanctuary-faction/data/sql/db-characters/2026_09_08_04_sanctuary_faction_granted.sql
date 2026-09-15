--
-- mod-sanctuary-faction - the receipt for every spell this module handed out
--
-- This table exists because Player::learnSpell is permanent. The spell goes into
-- `character_spell` and stays there through logout, through demotion, through the faction
-- being disbanded - the core has no idea it was conditional. Something has to remember.
--
-- The obvious alternative is to remember nothing and, at revoke time, look up what the
-- rank used to grant and unlearn that. It is wrong in both directions and both are silent:
--
--   * Edit a rank's spells, then demote somebody who held the OLD list, and the module
--     unlearns the new list. The spell they should have lost is kept forever, and one they
--     were never given is taken from them.
--
--   * Somebody learns a faction spell legitimately - a class ability, a trainer, a drop -
--     and later leaves the faction. Deriving the revoke set from the rank strips a spell
--     they earned. To the player that is theft with no explanation.
--
-- With a receipt neither can happen. Revoke reads THIS table, never the rank table: it
-- unlearns exactly what it gave, and if it never gave a spell it never takes it. If a
-- player already knew a spell before the faction offered it, the grant records nothing and
-- the revoke therefore takes nothing - which is checked at the call site in
-- Reconcile(), not here.
--
-- Rows are removed when the spell is taken back, so the table stays roughly the size of
-- "faction spells currently held on this realm" rather than growing forever.
--

CREATE TABLE IF NOT EXISTS `sanctuary_faction_granted` (
  `guid`  INT UNSIGNED NOT NULL COMMENT 'character guid',
  `spell` INT UNSIGNED NOT NULL COMMENT 'taught by this module, and therefore takeable by it',
  PRIMARY KEY (`guid`, `spell`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_general_ci;
