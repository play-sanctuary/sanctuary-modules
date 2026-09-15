--
-- mod-sanctuary-lawman - the irons take ten seconds, the key takes five
--
-- Both were wrong in opposite directions. Shackling was five seconds, which is quick for
-- something done to an unwilling person and gave bystanders almost nothing to react to.
-- Unshackling was instant and, worse, never cast at all: the key's item script answered
-- OnUse and returned true, so a rescue was silent and could not be interrupted.
--
-- Both halves, as always. The cast bar the player watches comes from the castTime the
-- server writes into SMSG_SPELL_START, which is this row - but the client's own Spell.dbc
-- carries a copy, and the two disagreeing is how a bar ends up out of step with what the
-- server will accept. patch-enUS-4.MPQ carries the same change; make-patch.py is the
-- source for it.
--
-- Indices 6 and 7 are stock rows in SpellCastTimes.dbc (5000ms and 10000ms), so nothing
-- new is needed in that table on either side.
--

--
-- Shackling: five seconds -> ten, and interrupted by movement only.
--
-- InterruptFlags was 17, movement and damage. Nothing forbade casting it in combat, but a
-- ten second cast that breaks on the first hit could never finish in one - so it was
-- castable in a fight in name only, which is worse than an honest refusal. 1 is movement
-- alone: walking away still stops it, being hit does not.
--
UPDATE `spell_dbc`
   SET `CastingTimeIndex` = 7,
       `InterruptFlags` = 1
 WHERE `ID` = 81001;

--
-- Unshackling: instant -> five seconds.
--
-- InterruptFlags 1 is movement only, deliberately NOT 17 (movement and damage) the way
-- Shackling is. Freeing somebody in the middle of a fight is the case this exists for, so
-- a stray hit must not undo it; walking away still does. Nothing in Attributes blocks
-- casting in combat, so being in combat does not prevent it either.
--
UPDATE `spell_dbc`
   SET `CastingTimeIndex` = 6,
       `InterruptFlags` = 1
 WHERE `ID` = 81008;
