--
-- mod-sanctuary-lawman - the shackle cast gets a spell of its own
--
-- WHY: the same reason the chain did. The cast bar shows a spell's name, the client reads
-- that name out of its OWN Spell.dbc, and it never travels from the server - so borrowing
-- 62646 "Shackle" put the word "Shackle" on the bar and nothing server-side could change
-- it. The word wanted was "Shackling", and the only way to have it is to own the row.
--
-- Owning it settles four other things that were being corrected at load every startup:
--
--   * the cast time is 5000ms in the row itself, rather than an index forced on afterwards
--   * it is not channelled, so nothing has to clear the channel bits that left a captor
--     stuck in a casting animation with no duration to end it
--   * its effect is already an inert dummy aimed at the caster, so no targeting to undo
--   * InterruptFlags 17 - cancel on movement, and complete interrupt on damage - is in the
--     row, rather than added afterwards to a spell that shipped with none
--
-- SpellVisual 395 is what 666 blacksmithing recipes cast with, Copper Bracers among them,
-- so the cast plays the smith's crafting animation. Which is the right animation: this is
-- metalwork, performed on a person who would rather it were not.
--
-- The module's load-time corrections are kept even though this row needs none of them. They
-- cost nothing on a row that already agrees, and they are what makes it safe to point
-- SanctuaryLawman.Shackles.CastSpell at a retail spell again.
--
--   81001  Shackling   visual 395 (blacksmith crafting), icon 4400, 5s, interruptible
--
-- Both halves again: patch-enUS-4.MPQ carries the client's copy. Without it the cast bar
-- reads "Unknown" and no animation plays.
--

DELETE FROM `spell_dbc` WHERE `ID` = 81001;

INSERT INTO `spell_dbc` (
  `ID`, `Attributes`, `CastingTimeIndex`, `InterruptFlags`, `DurationIndex`, `RangeIndex`,
  `EquippedItemClass`, `Effect_1`, `ImplicitTargetA_1`,
  `SpellVisualID_1`, `SpellIconID`, `SchoolMask`,
  `Name_Lang_enUS`, `Name_Lang_Mask`,
  `Description_Lang_enUS`, `Description_Lang_Mask`,
  `AuraDescription_Lang_enUS`, `AuraDescription_Lang_Mask`
) VALUES (
  81001,
  0,
  6,            -- the 5000ms row in SpellCastTimes
  17,           -- 0x01 cancel on movement | 0x10 complete interrupt on damage
  0,            -- no duration: it applies nothing, the module does the work on completion
  1,
  -1,           -- requires no weapon. 0 would mean one is required.
  3,            -- SPELL_EFFECT_DUMMY - inert, and aimed at the caster
  1,            -- TARGET_UNIT_CASTER. Aiming it at the prisoner is what produced "Invalid
                -- Target": a positive spell run against somebody hostile fails assist
                -- checks, so the prisoner is read from the caster's selection instead.
  395,
  4400,
  1,
  'Shackling', 16712190,
  'Fitting irons to somebody who would rather you did not.', 16712190,
  'Fitting irons to somebody who would rather you did not.', 16712190
);

-- And the item casts it. This is the line that puts the new word on the cast bar.
UPDATE `item_template` SET `spellid_1` = 81001 WHERE `entry` = 990001;
