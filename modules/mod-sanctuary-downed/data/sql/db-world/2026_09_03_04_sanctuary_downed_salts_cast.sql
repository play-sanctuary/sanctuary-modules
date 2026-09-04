--
-- mod-sanctuary-downed - the smelling salts get a cast bar, and both revives get an icon
--
-- WHY: the salts revived instantly from an ItemScript, and an instant script cannot have a
-- cast bar. Ten seconds of holding a vial under somebody's nose has to be a real cast, so
-- the item casts a spell of its own and the script is gone.
--
-- Ten against Resuscitate's five is the whole point of having both: fumbling a vial out of a
-- bag while somebody bleeds is not the same as being trained for it. Either can be broken by
-- movement or a hit, so neither is free in the middle of a fight.
--
-- Both now wear SpellIcon 4401, INV_Potion_01 - the icon the vial already carries in the
-- bag - so the spellbook entry and the item read as the same idea rather than borrowing the
-- lawman chain.
--
-- Range is the client's shortest, two yards, because there is no shorter row in
-- SpellRange.dbc. The real half yard lives in SanctuaryDowned.Downed.ReviveRange and is
-- checked by the module with bounding radii, exactly as the carry's own reach is.
--

-- Resuscitate: potion icon, and brought in from thirty yards to arm's length.
UPDATE `spell_dbc` SET `SpellIconID` = 4401, `RangeIndex` = 96 WHERE `ID` = 81004;

DELETE FROM `spell_dbc` WHERE `ID` = 81005;

INSERT INTO `spell_dbc` (
  `ID`, `Attributes`, `CastingTimeIndex`, `DurationIndex`, `RangeIndex`, `InterruptFlags`,
  `EquippedItemClass`, `Effect_1`, `ImplicitTargetA_1`,
  `SpellVisualID_1`, `SpellIconID`, `SchoolMask`,
  `Name_Lang_enUS`, `Name_Lang_Mask`,
  `Description_Lang_enUS`, `Description_Lang_Mask`,
  `AuraDescription_Lang_enUS`, `AuraDescription_Lang_Mask`
) VALUES (
  81005,
  0,
  7,            -- the 10000ms row in SpellCastTimes
  0,
  96,           -- 2 yards, the shortest the client has; the module enforces half of that
  17,           -- INTERRUPT_ON_MOVE | INTERRUPT_ON_DAMAGE
  -1,
  3,            -- SPELL_EFFECT_DUMMY - the module does the work and spends the vial
  25,           -- TARGET_UNIT_TARGET_ANY, because a downed player goes neutral once their
                -- few seconds of grace expire and a friendly-only spell would refuse them
  1008,         -- the skinning animation: crouched over a body at arm's length
  4401,
  1,
  'Smelling Salts', 16712190,
  'Held under the nose of someone who has stopped listening.', 16712190,
  'Held under the nose of someone who has stopped listening.', 16712190
);

--
-- The item casts it rather than being handled by a script. ScriptName is cleared for the
-- same reason: an ItemScript that returns "handled" would swallow the cast before it began.
--
UPDATE `item_template`
   SET `spellid_1` = 81005,
       `spelltrigger_1` = 0,
       `ScriptName` = ''
 WHERE `entry` = 990004;
