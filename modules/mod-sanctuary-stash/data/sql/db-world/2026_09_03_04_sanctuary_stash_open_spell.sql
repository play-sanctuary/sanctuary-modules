--
-- mod-sanctuary-stash - opening a strongbox takes two seconds
--
-- A window that appears the instant a box is clicked reads as a menu. Two seconds of the
-- kneel-and-work animation reads as somebody opening a chest, which is what is happening -
-- and it gives anybody watching a moment to notice that it is happening.
--
-- The spell carries three things at once: the delay, the animation, and the interrupt. That
-- last one matters most - InterruptFlags 17 is cancel-on-movement plus complete-interrupt-
-- on-damage, so walking away or being hit cancels the opening without the module watching
-- for either. Everything is still re-checked when the cast lands, because two seconds is
-- long enough to step out of range, to die, or to have the key lifted from a pocket.
--
-- Visual 180 is what stock spell 3365 "Opening" casts with, and the name matches for the
-- same reason every other spell here does: the cast bar shows it, the client reads it from
-- its own Spell.dbc, and nothing the server sends can change the word.
--
--   81011  Opening   visual 180, 2000ms, interruptible
--
-- The client's copy is in patch-enUS-4.MPQ. Without it the cast bar reads "Unknown" and no
-- animation plays, though the box still opens.
--

DELETE FROM `spell_dbc` WHERE `ID` = 81011;

INSERT INTO `spell_dbc` (
  `ID`, `Attributes`, `CastingTimeIndex`, `InterruptFlags`, `DurationIndex`, `RangeIndex`,
  `EquippedItemClass`, `Effect_1`, `ImplicitTargetA_1`,
  `SpellVisualID_1`, `SpellIconID`, `SchoolMask`,
  `Name_Lang_enUS`, `Name_Lang_Mask`,
  `Description_Lang_enUS`, `Description_Lang_Mask`,
  `AuraDescription_Lang_enUS`, `AuraDescription_Lang_Mask`
) VALUES (
  81011,
  0,
  5,            -- the 2000ms row in SpellCastTimes
  17,           -- 0x01 cancel on movement | 0x10 complete interrupt on damage
  0,
  1,
  -1,           -- requires no weapon. 0 would mean one is required.
  3,            -- SPELL_EFFECT_DUMMY: the module does the work when the cast lands
  1,            -- TARGET_UNIT_CASTER
  180,          -- spell 3365 "Opening" - the kneel-and-work animation for chests
  4400,
  1,
  'Opening', 16712190,
  'Work the lid of a strongbox open.', 16712190,
  'Work the lid of a strongbox open.', 16712190
);
