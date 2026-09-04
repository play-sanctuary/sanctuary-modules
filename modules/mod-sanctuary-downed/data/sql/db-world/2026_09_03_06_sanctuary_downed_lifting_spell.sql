--
-- mod-sanctuary-downed - the carry gets a spell of its own
--
-- WHY: lifting somebody has been casting 56992, a stock inscription recipe reshaped at load -
-- cast time forced, channel flags stripped, effect replaced, and every reagent and cost
-- cleared. That last part was added only after the borrowed spell demanded Moonglow Ink in
-- front of a player: a requirement invisible in the cast time and channel flags that had been
-- checked when choosing it. Borrowing a spell inherits everything about it, not just the
-- parts somebody looked at.
--
-- The module can ship spells of its own now - there are four already - so the borrowing and
-- the whole reshaping hook are gone, and this is the replacement.
--
--   81006  Lifting   5s cast, cancelled by movement or damage
--
-- Cast on the CARRIER themselves, which is why it has no range. The half yard to whoever is
-- being picked up is checked by the module in Check(), the same value both revives use, and
-- measured with bounding radii so it means "stood over them" rather than "somewhere nearby".
--
-- The effect is a dummy: the lift happens in the module when the bar finishes.
--
DELETE FROM `spell_dbc` WHERE `ID` = 81006;

INSERT INTO `spell_dbc` (
  `ID`, `Attributes`, `CastingTimeIndex`, `DurationIndex`, `RangeIndex`, `InterruptFlags`,
  `EquippedItemClass`, `Effect_1`, `ImplicitTargetA_1`,
  `SpellVisualID_1`, `SpellIconID`, `SchoolMask`,
  `Name_Lang_enUS`, `Name_Lang_Mask`,
  `Description_Lang_enUS`, `Description_Lang_Mask`,
  `AuraDescription_Lang_enUS`, `AuraDescription_Lang_Mask`
) VALUES (
  81006,
  0,            -- no combat restriction: hauling somebody out of a fight is half the point
  6,            -- the 5000ms row in SpellCastTimes
  0,
  1,            -- self only; the reach that matters is the module's
  1,            -- INTERRUPT_ON_MOVE only. Movement has to be here or the bar stops on the
                -- client, which predicts the cancel locally, while the server carries on and
                -- lifts them anyway when the time is up. Damage is deliberately NOT included:
                -- dragging somebody clear while being shot at is the situation the carry
                -- exists for, and a stray hit should not undo five seconds of it.
  -1,
  3,            -- SPELL_EFFECT_DUMMY
  1,            -- TARGET_UNIT_CASTER
  1008,         -- the skinning animation: crouched over a body at arm's length
  454,          -- Ability_Hunter_BeastSoothe, the icon Revive Pet wears
  1,
  'Lifting', 16712190,
  'Pick up someone over your shoulder', 16712190,
  'Pick up someone over your shoulder', 16712190
);
