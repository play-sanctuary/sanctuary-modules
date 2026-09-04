--
-- mod-sanctuary-downed - Resuscitate, the spell that replaces the resurrection hijack
--
-- WHY: retail resurrection spells cannot be made to work on a downed player, and the reason
-- is not fixable from here. The client decides for itself whether a rez has a valid target,
-- it wants a corpse, and it decides BEFORE sending anything - an instrumented server logged
-- nothing at all when one was cast. Widening the spell's target mask in the client patch did
-- not change that, and neither did making the body read as dead. Every one of those attempts
-- was reaching for a decision the server never gets to see.
--
-- So the four classes that already have a resurrection get a spell of our own instead. It is
-- in the spellbook, it targets whoever is selected, and what it does is entirely ours - no
-- part of it has to be argued out of the client.
--
--   81004  Resuscitate   5s cast, 30 yards, cancelled by movement or damage
--
-- The effect is a dummy. The revive itself is the module's, which is what lets it apply the
-- same rules smelling salts do: the target must genuinely be down, and health comes back
-- with them so that standing up is not just a formality before dying again.
--
DELETE FROM `spell_dbc` WHERE `ID` = 81004;

INSERT INTO `spell_dbc` (
  `ID`, `Attributes`, `CastingTimeIndex`, `DurationIndex`, `RangeIndex`, `InterruptFlags`,
  `EquippedItemClass`, `Effect_1`, `ImplicitTargetA_1`,
  `SpellVisualID_1`, `SpellIconID`, `SchoolMask`,
  `Name_Lang_enUS`, `Name_Lang_Mask`,
  `Description_Lang_enUS`, `Description_Lang_Mask`,
  `AuraDescription_Lang_enUS`, `AuraDescription_Lang_Mask`
) VALUES (
  81004,
  0,
  6,            -- the 5000ms row in SpellCastTimes: long enough to be interrupted, short
                -- enough to be worth attempting with somebody still swinging nearby
  0,            -- no duration; the effect is instantaneous once the cast lands
  4,            -- the 30 yard row in SpellRange, hostile and friendly alike
  17,           -- INTERRUPT_ON_MOVE | INTERRUPT_ON_DAMAGE. Kneeling over somebody in the
                -- middle of a fight should not be free.
  -1,           -- requires no weapon. 0 would demand one.
  3,            -- SPELL_EFFECT_DUMMY - the module does the work
  25,           -- TARGET_UNIT_TARGET_ANY. The module checks they are actually down; a
                -- friendly-only target would refuse, because a downed player goes NEUTRAL
                -- once their few seconds of grace expire.
  1008,         -- the skinning animation: crouched over a body at arm's length
  4400,
  1,
  'Resuscitate', 16712190,
  'Bring someone back to their senses before the dark closes over them.', 16712190,
  'Bring someone back to their senses before the dark closes over them.', 16712190
);
