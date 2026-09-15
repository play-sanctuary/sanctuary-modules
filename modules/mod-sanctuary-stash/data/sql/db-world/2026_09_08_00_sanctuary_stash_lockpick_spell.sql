--
-- mod-sanctuary-stash - picking a lock takes eight seconds
--
-- The server half of spell 81012 "Lockpicking". The client half is a row in
-- patch-enUS-4.MPQ, added by tools/mpq/add_lockpicking.py, and reaches players through the
-- account service's client-patch sync rather than a reinstall.
--
--
-- WHY A SPELL OF OUR OWN, after two stock ones were tried and rejected.
--
-- A cast time cannot be varied per cast - Spell::m_casttime is protected with only a getter
-- - so a longer pick has to be a different spell from the two second key. Two stock spells
-- were the obvious way to avoid a client patch, and each failed on something the server
-- cannot reach:
--
--   21651 "Opening"      the only eight second opening spell in the game, and it carries
--                        SPELL_ATTR3_NO_CASTING_BAR_TEXT. The bar runs for the full eight
--                        seconds with no word on it. That flag is read from the client's
--                        own Spell.dbc; nothing sent from here changes it.
--
--   1809  "Lockpicking"  the right name, shown, and unreferenced on this realm - but its
--                        visual has precast kit 0, so the character stands still for eight
--                        seconds doing nothing at all.
--
-- 81012 has the name, the length and the animation, because we chose all three.
--
--
-- Cloned from 81011 rather than written fresh, on both halves. That spell is proven
-- castable on this client - it is what opens a strongbox with a key today - so every field
-- that decides whether a cast is even accepted is known good, and only three differ: the
-- id, the name, and the eight seconds.
--
-- The interrupt flags are the one place this is stricter than 81011. 17 is movement plus
-- damage; 31 adds pushback, the interrupt school and starting an autoattack. Picking a lock
-- should fail if anything at all happens to you, which is the whole reason it is slower
-- than a key.
--

DELETE FROM `spell_dbc` WHERE `ID` = 81012;

INSERT INTO `spell_dbc` (
  `ID`, `Attributes`, `CastingTimeIndex`, `InterruptFlags`, `DurationIndex`, `RangeIndex`,
  `EquippedItemClass`, `Effect_1`, `ImplicitTargetA_1`,
  `SpellVisualID_1`, `SpellIconID`, `SchoolMask`,
  `Name_Lang_enUS`, `Name_Lang_Mask`,
  `Description_Lang_enUS`, `Description_Lang_Mask`,
  `AuraDescription_Lang_enUS`, `AuraDescription_Lang_Mask`
) VALUES (
  81012,
  0,
  170,          -- the 8000ms row in SpellCastTimes
  31,           -- movement | pushback | interrupt | autoattack | complete interrupt on damage
  0,
  1,
  -1,           -- requires no weapon. 0 would mean one is required.
  3,            -- SPELL_EFFECT_DUMMY: the module does the work when the cast lands
  1,            -- TARGET_UNIT_CASTER
  180,          -- the kneel-and-work animation, as 81011
  4400,
  1,
  'Lockpicking', 16712190,
  'Work a strongbox open without its key.', 16712190,
  'Work a strongbox open without its key.', 16712190
);
