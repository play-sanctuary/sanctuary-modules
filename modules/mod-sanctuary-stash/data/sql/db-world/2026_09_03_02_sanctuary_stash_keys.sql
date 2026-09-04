--
-- mod-sanctuary-stash - a palette of keys
--
-- A key is only a key because a strongbox row names its entry, so "more keys" means "more
-- item entries" - and item entries cannot be created while the server runs. AzerothCore has
-- 305 reload subcommands and `item_template` is not among them; templates are referenced too
-- widely to swap underneath a running world. So the keys are shipped in advance and chosen
-- from, rather than invented on demand.
--
-- Fifteen of them, one for each INV_Misc_Key icon the 3.3.5a client carries. Every display
-- id below is a stock ItemDisplayInfo row, so nothing here needs artwork - but each ENTRY
-- still needs a row in the client's Item.dbc, which patch-enUS-4.MPQ supplies. Without the
-- patch they are question marks in the bag, the same as every other custom item.
--
-- Two boxes may share a key, and that is a feature: it is how a crew gets one cache between
-- them, or the watch a master key. Give a box its own entry when it should stand alone.
--
--   .stash key 990030    lock the box you are standing at with the Skeleton Key
--   .stash key 0         take the lock off again
--
-- Every key is usable, and using one lists the strongboxes it opens. That is the only way a
-- key can say where it works: name and description are per-ENTRY, so all fifteen copies of
-- the Skeleton Key read alike however many different boxes they are locking. Spell 81010
-- exists purely so the client offers a "Use:" line - the item script answers and suppresses
-- it before anything is cast.
--

DELETE FROM `item_template` WHERE `entry` BETWEEN 990020 AND 990034;

INSERT INTO `item_template`
  (`entry`, `class`, `subclass`, `name`, `description`, `displayid`, `Quality`, `Flags`,
   `bonding`, `maxcount`, `stackable`, `BuyPrice`, `SellPrice`, `RequiredLevel`,
   `InventoryType`, `Material`, `sheath`, `spellid_1`, `spelltrigger_1`, `spellcharges_1`,
   `spellcooldown_1`, `spellcategorycooldown_1`, `ScriptName`)
VALUES
  (990020, 15, 0, 'Iron Key',
   'Plain, heavy, and cut for one lock.',
   3885, 3, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 81010, 0, 0, -1, -1, 'sanctuary_stash_key'),
  (990021, 15, 0, 'Brass Key',
   'Yellow with handling, and older than it looks.',
   4757, 3, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 81010, 0, 0, -1, -1, 'sanctuary_stash_key'),
  (990022, 15, 0, 'Rusted Key',
   'It turns. That is all anyone asks of it.',
   3745, 3, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 81010, 0, 0, -1, -1, 'sanctuary_stash_key'),
  (990023, 15, 0, 'Bone Key',
   'Cut from something that was not always a key.',
   2530, 3, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 81010, 0, 0, -1, -1, 'sanctuary_stash_key'),
  (990024, 15, 0, 'Silver Key',
   'Too fine for a cellar door, and used on one anyway.',
   8951, 3, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 81010, 0, 0, -1, -1, 'sanctuary_stash_key'),
  (990025, 15, 0, 'Ornate Key',
   'Someone was paid well to make this.',
   8902, 3, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 81010, 0, 0, -1, -1, 'sanctuary_stash_key'),
  (990026, 15, 0, 'Crooked Key',
   'Bent once, straightened badly, still works.',
   9153, 3, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 81010, 0, 0, -1, -1, 'sanctuary_stash_key'),
  (990027, 15, 0, 'Strongbox Key',
   'Cut for one lock, and no other.',
   13885, 3, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 81010, 0, 0, -1, -1, 'sanctuary_stash_key'),
  (990028, 15, 0, 'Gilded Key',
   'Gold leaf over cheap iron. Like its owner.',
   22185, 3, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 81010, 0, 0, -1, -1, 'sanctuary_stash_key'),
  (990029, 15, 0, 'Blackened Key',
   'Held in a fire, and not by accident.',
   20802, 3, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 81010, 0, 0, -1, -1, 'sanctuary_stash_key'),
  (990030, 15, 0, 'Skeleton Key',
   'Fits more locks than it should.',
   16100, 3, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 81010, 0, 0, -1, -1, 'sanctuary_stash_key'),
  (990031, 15, 0, 'Glass Key',
   'Cold, clear, and alarmingly fragile.',
   16453, 3, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 81010, 0, 0, -1, -1, 'sanctuary_stash_key'),
  (990032, 15, 0, 'Rune-cut Key',
   'The marks along it mean nothing to you.',
   22477, 3, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 81010, 0, 0, -1, -1, 'sanctuary_stash_key'),
  (990033, 15, 0, 'Tarnished Key',
   'Green at the teeth. Left somewhere damp.',
   9660, 3, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 81010, 0, 0, -1, -1, 'sanctuary_stash_key'),
  (990034, 15, 0, 'Frostbound Key',
   'It never quite warms in the hand.',
   58526, 3, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 81010, 0, 0, -1, -1, 'sanctuary_stash_key');

--
-- The Use line on every key. Never cast: the item script answers OnUse and returns true.
--
DELETE FROM `spell_dbc` WHERE `ID` = 81010;

INSERT INTO `spell_dbc` (
  `ID`, `Attributes`, `CastingTimeIndex`, `InterruptFlags`, `DurationIndex`, `RangeIndex`,
  `EquippedItemClass`, `Effect_1`, `ImplicitTargetA_1`,
  `SpellVisualID_1`, `SpellIconID`, `SchoolMask`,
  `Name_Lang_enUS`, `Name_Lang_Mask`,
  `Description_Lang_enUS`, `Description_Lang_Mask`,
  `AuraDescription_Lang_enUS`, `AuraDescription_Lang_Mask`
) VALUES (
  81010, 0, 1, 0, 0, 1, -1, 3, 1, 0, 4400, 1,
  'Read the Key', 16712190,
  'Find out which strongbox this key opens.', 16712190,
  'Find out which strongbox this key opens.', 16712190
);
