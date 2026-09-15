--
-- mod-sanctuary-gm - placing an object where you click
--
-- The server half of spell 81013 "Place Object". The client half is a row in
-- patch-enUS-4.MPQ, added by tools/mpq/add_placeobject.py, and reaches players through the
-- account service's client-patch sync.
--
--
-- WHY A SPELL, for something that is not a spell.
--
-- 3.3.5 exposes no way to turn a screen position into a world position: there is no raycast
-- in the Lua API, so an addon cannot know where in the world the cursor is pointing. A game
-- master could therefore only ever spawn an object on top of themselves and nudge it
-- afterwards.
--
-- A ground targeted spell already solves exactly this, and solves it inside the client. The
-- client draws the reticle, decides what the ground under it is, and sends the world
-- position in CMSG_CAST_SPELL as TARGET_FLAG_DEST_LOCATION. The server reads the
-- destination off the cast. Nothing is traced at either end and no new protocol is needed.
--
-- Targets 64 is TARGET_FLAG_DEST_LOCATION - it is what makes the client show a reticle at
-- all - and ImplicitTargetA 28 is TARGET_DEST_DEST, "wherever they clicked". Both are
-- copied from Blizzard (spell 10), the plainest ground targeted spell in the game.
--
-- The effect is a dummy: the module does the placing when the cast lands. Instant, because
-- there is nothing to wait for and a cast bar on a placement tool is noise.
--

DELETE FROM `spell_dbc` WHERE `ID` = 81013;

INSERT INTO `spell_dbc` (
  `ID`, `Attributes`, `CastingTimeIndex`, `InterruptFlags`, `DurationIndex`, `RangeIndex`,
  `Targets`, `EquippedItemClass`, `Effect_1`, `ImplicitTargetA_1`,
  `SpellVisualID_1`, `SpellIconID`, `SchoolMask`,
  `Name_Lang_enUS`, `Name_Lang_Mask`,
  `Description_Lang_enUS`, `Description_Lang_Mask`,
  `AuraDescription_Lang_enUS`, `AuraDescription_Lang_Mask`
) VALUES (
  81013,
  0,
  1,            -- instant
  0,
  0,
  4,            -- the same range row Blizzard uses
  64,           -- TARGET_FLAG_DEST_LOCATION: the client shows a reticle for this
  -1,           -- requires no weapon
  3,            -- SPELL_EFFECT_DUMMY: the module does the work when the cast lands
  28,           -- TARGET_DEST_DEST: wherever they clicked
  0,
  1,
  1,
  'Place Object', 16712190,
  'Put the chosen object where you click.', 16712190,
  'Put the chosen object where you click.', 16712190
);
