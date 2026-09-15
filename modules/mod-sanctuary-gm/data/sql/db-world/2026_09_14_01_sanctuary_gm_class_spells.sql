--
-- The game master class's two forms: spells 81014 "True Visage" and 81015 "Winged Visage".
--
-- The client half is a row apiece in patch-enUS-4.MPQ, written by tools/assets/make-patch.py
-- (see SPELLS there); this is the half the server casts from. Editing one without the other
-- desynchronises them - the name and icon come from the client, the effects from here.
--
-- Both are instant, self-cast and last forever, so each is a buff the holder can cancel by
-- right-clicking it. mod-sanctuary-gm watches the aura go on and come off and does the morph
-- and the scale then (SanctuaryGmClass.cpp): a DBC aura cannot morph to a DISPLAY, only to a
-- creature entry, and these are displays - 19373, the bronze dragon the Steward of Time
-- wears, and 25852, a drake, both at their own size.
--
-- THE SPEED IS HERE, not in the module, and that is deliberate. Run and flight speed are
-- recalculated from auras whenever anything touches them, so a rate set by hand survives
-- only until the next aura, mount or shapeshift and then quietly reverts. As aura effects
-- they are what the recalculation reads.
--
--   aura 31  MOD_INCREASE_SPEED         +100%   an epic ground mount
--   aura 206 MOD_INCREASE_FLIGHT_SPEED  +280%   an epic flying mount
--
-- Both are the UNMOUNTED auras, which is what a game master using `.gm fly` is. The mounted
-- equivalents (32 and 207) are read only while actually mounted and would do nothing.
--
-- With no die sides an effect's value is its base points exactly, so 100 means +100% - the
-- stock rows store one less than they mean because they roll 1..1 on top.
--

DELETE FROM `spell_dbc` WHERE `ID` IN (81014, 81015);

INSERT INTO `spell_dbc` (
  `ID`, `Attributes`, `CastingTimeIndex`, `InterruptFlags`, `DurationIndex`, `RangeIndex`,
  `EquippedItemClass`,
  `Effect_1`, `EffectAura_1`, `EffectBasePoints_1`, `ImplicitTargetA_1`,
  `Effect_2`, `EffectAura_2`, `EffectBasePoints_2`, `ImplicitTargetA_2`,
  `SpellVisualID_1`, `SpellIconID`, `SchoolMask`,
  `Name_Lang_enUS`, `Name_Lang_Mask`,
  `Description_Lang_enUS`, `Description_Lang_Mask`,
  `AuraDescription_Lang_enUS`, `AuraDescription_Lang_Mask`
) VALUES
(
  81014,
  0,
  1,            -- instant
  0,
  21,           -- the -1 row in SpellDuration: until cancelled
  1,            -- self
  -1,           -- requires no weapon
  6, 4, 0, 1,   -- APPLY_AURA / DUMMY on the caster: the module answers this one
  0, 0, 0, 0,
  0,
  1701,         -- INV_Misc_Head_Dragon_Bronze
  1,
  'True Visage', 16712190,
  'Your true form, the dragon beneath the mortal shape.', 16712190,
  'Your true form, at a size that fits indoors.', 16712190
),
(
  81015,
  0,
  1,            -- instant
  0,
  21,           -- until cancelled
  1,            -- self
  -1,
  6, 31, 100, 1,    -- APPLY_AURA / MOD_INCREASE_SPEED +100% on the caster
  6, 206, 280, 1,   -- APPLY_AURA / MOD_INCREASE_FLIGHT_SPEED +280% on the caster
  0,
  3440,         -- Ability_Mount_Drake_Bronze
  1,
  'Winged Visage', 16712190,
  'A drake''s form, with the ground and flight speed of an epic mount. Flight needs .gm fly, which this turns on.', 16712190,
  'A drake''s form, with the ground and flight speed of an epic mount. Flight needs .gm fly, which this turns on.', 16712190
);
