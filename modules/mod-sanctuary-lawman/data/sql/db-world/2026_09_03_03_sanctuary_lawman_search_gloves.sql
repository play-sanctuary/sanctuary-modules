--
-- mod-sanctuary-lawman - Search Gloves, for going through a prisoner's pack
--
-- A prisoner in irons can be searched. The gloves are the authority to do it, not the
-- office: they are handed out like the writs and the shackles, and like those they can be
-- lost, taken off a body, or end up in the wrong hands. Anyone holding them can search
-- anyone who is shackled.
--
-- They only LOOK. Nothing can be taken with them, which is deliberate: a window that shows
-- another player's live inventory and lets you take from it is an item duplicator unless
-- every take is mirrored back as a destroy, and the prisoner can be trading or drinking
-- that potion at the same moment. Reading the pack out is the whole feature; anything that
-- changes hands afterwards is roleplayed through an ordinary trade.
--
-- 81009 exists for one line of tooltip. A "Use:" line is the on-use spell's DESCRIPTION,
-- read by the client from its own Spell.dbc, so an item riding on a borrowed spell either
-- shows that spell's words or - with 61410 "Bind", which the writs use - no Use line at
-- all. The spell is never cast: the item script answers OnUse and returns true.
--
--   990005  Search Gloves   display 972, INV_Gauntlets_05
--   81009   Search          "Search the belongings of someone shackled."
--
-- The client's copies of both are in patch-enUS-4.MPQ. Without it the gloves are a question
-- mark in the bag, exactly as the other four were before they had rows of their own.
--

DELETE FROM `spell_dbc` WHERE `ID` = 81009;

INSERT INTO `spell_dbc` (
  `ID`, `Attributes`, `CastingTimeIndex`, `InterruptFlags`, `DurationIndex`, `RangeIndex`,
  `EquippedItemClass`, `Effect_1`, `ImplicitTargetA_1`,
  `SpellVisualID_1`, `SpellIconID`, `SchoolMask`,
  `Name_Lang_enUS`, `Name_Lang_Mask`,
  `Description_Lang_enUS`, `Description_Lang_Mask`,
  `AuraDescription_Lang_enUS`, `AuraDescription_Lang_Mask`
) VALUES (
  81009,
  0,
  1,            -- instant; it is suppressed before it can cast anyway
  0,
  0,
  1,
  -1,           -- requires no weapon. 0 would mean one is required.
  3,            -- SPELL_EFFECT_DUMMY
  1,            -- TARGET_UNIT_CASTER
  0,            -- no artwork: searching somebody is not a spell effect
  4400,
  1,
  'Search', 16712190,
  'Search the belongings of someone shackled.', 16712190,
  'Search the belongings of someone shackled.', 16712190
);

DELETE FROM `item_template` WHERE `entry` = 990005;

INSERT INTO `item_template`
  (`entry`, `class`, `subclass`, `name`, `description`, `displayid`, `Quality`, `Flags`,
   `bonding`, `maxcount`, `stackable`, `BuyPrice`, `SellPrice`, `RequiredLevel`,
   `InventoryType`, `Material`, `sheath`,
   `spellid_1`, `spelltrigger_1`, `spellcharges_1`, `spellcooldown_1`,
   `spellcategorycooldown_1`, `ScriptName`)
VALUES
  (990005, 15, 0, 'Search Gloves',
   'These gloves will protect you during searches.',
   972, 3, 0,
   0, 0, 1, 0, 0, 0,
   0, 8, 0,
   81009, 0, 0, -1,
   -1, 'sanctuary_lawman_search');
