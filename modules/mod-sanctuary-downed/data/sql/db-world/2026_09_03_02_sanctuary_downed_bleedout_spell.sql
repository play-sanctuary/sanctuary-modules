--
-- mod-sanctuary-downed - the bleed-out clock, as something the player can see
--
-- WHY: ten minutes is a long time to lie still with no idea how much of it is left. This is
-- the countdown, in the debuff tray, where a timer belongs.
--
-- It earns its keep twice. The SanctuaryDowned addon watches for this aura to decide when to
-- offer the "stop holding on" button, which means the popup needs no addon channel, no
-- server-to-client message, and nothing to keep in step - the thing that says how long you
-- have left is the same thing that says you are down.
--
-- The module overwrites the duration from SanctuaryDowned.Downed.BleedOutSeconds when it
-- applies the aura, and re-syncs it whenever the two drift by more than a second. That
-- matters because the clock STOPS while somebody is carrying you: without the re-sync the
-- icon would run out while you were in no danger at all.
--
DELETE FROM `spell_dbc` WHERE `ID` = 81003;

INSERT INTO `spell_dbc` (
  `ID`, `Attributes`, `CastingTimeIndex`, `DurationIndex`, `RangeIndex`,
  `EquippedItemClass`, `Effect_1`, `ImplicitTargetA_1`, `EffectAura_1`,
  `SpellIconID`, `SchoolMask`,
  `Name_Lang_enUS`, `Name_Lang_Mask`,
  `Description_Lang_enUS`, `Description_Lang_Mask`,
  `AuraDescription_Lang_enUS`, `AuraDescription_Lang_Mask`
) VALUES (
  81003,
  2214592512,   -- AURA_IS_DEBUFF | NO_AURA_CANCEL. Not cancellable: right-clicking away your
                -- own death timer would take the button with it.
  1,            -- instant; applied directly rather than cast
  6,            -- the 600000ms row in SpellDuration - ten minutes. Overwritten from config.
  1,
  -1,           -- no weapon required
  6,            -- SPELL_EFFECT_APPLY_AURA
  1,            -- TARGET_UNIT_CASTER - only ever on the player themselves
  4,            -- SPELL_AURA_DUMMY. Inert; the module owns the timer, this displays it.
  4400,
  1,
  'Bleeding Out', 16712190,
  'Time is running out, and only somebody else can stop it.', 16712190,
  'Time is running out, and only somebody else can stop it.', 16712190
);
