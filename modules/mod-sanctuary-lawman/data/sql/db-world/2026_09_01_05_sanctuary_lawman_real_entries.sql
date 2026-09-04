--
-- mod-sanctuary-lawman - moving the four items onto entries the client already knows
--
-- WHY: they rendered as question marks. The 3.3.5a client reads an item's DisplayInfoID
-- from its OWN Item.dbc, not from the query response the server sends - and that file has
-- 46,096 rows, nothing above entry 56,806, and no row for 990000-990003. A custom entry
-- therefore has no display at all client-side and no server setting can give it one. The
-- name and tooltip still arrive from the server, which is why the items worked and read
-- correctly while showing no icon.
--
-- There are no unused entries to claim: item_template already defines every id in
-- Item.dbc. So these four ride on deprecated rows - dead content that nothing spawns,
-- sells, drops or asks for - keeping the client's display while the server supplies
-- everything else.
--
--   3513  Deprecated Contract for the Magistrate  INV_Scroll_04  -> Writ of Accusation
--   1078  Deprecated Writ of Lakeshire            INV_Scroll_03  -> Writ of Pardon
--   2412  Deprecated Nightmare Bridle             INV_Misc_Net_01 -> Iron Shackles
--   5384  Deprecated Tower of Althalaxx Key       INV_Misc_Key_02 -> Iron Shackle Key
--
-- `class` and `subclass` are deliberately NOT changed: the core rewrites them to match
-- Item.dbc anyway, and a mismatch is only an argument the client wins.
--
-- The on-use spells change too. 38067 "Guard's Mark" was putting the Hunter's Mark
-- description into every tooltip as flavour text - a borrowed spell brings its own words
-- with it. 61410 "Bind" is instant, self-targeted, does nothing, and has an EMPTY
-- description, so the tooltip shows only what we wrote. The shackles use 62646 "Shackle",
-- chosen for its name: the cast bar shows the spell's name, the client reads that text
-- from its own files, and picking a spell already called the right thing is the only way
-- to change it. Its own targeting and effect are corrected at load by the module.
--

DELETE FROM `item_template` WHERE `entry` IN (990000, 990001, 990002, 990003);

UPDATE `item_template` SET
  `name` = 'Writ of Accusation',
  `description` = 'By the authority of the watch, this hand is raised against you.',
  `Quality` = 3, `bonding` = 0, `maxcount` = 0, `stackable` = 1,
  `BuyPrice` = 0, `SellPrice` = 0, `RequiredLevel` = 0,
  `spellid_1` = 61410, `spelltrigger_1` = 0, `spellcooldown_1` = -1, `spellcategorycooldown_1` = -1,
  `ScriptName` = 'sanctuary_lawman_writ'
WHERE `entry` = 3513;

UPDATE `item_template` SET
  `name` = 'Writ of Pardon',
  `description` = 'The charge is dropped. Go, and be seen to have gone.',
  `Quality` = 3, `bonding` = 0, `maxcount` = 0, `stackable` = 1,
  `BuyPrice` = 0, `SellPrice` = 0, `RequiredLevel` = 0,
  `spellid_1` = 61410, `spelltrigger_1` = 0, `spellcooldown_1` = -1, `spellcategorycooldown_1` = -1,
  `ScriptName` = 'sanctuary_lawman_pardon'
WHERE `entry` = 1078;

UPDATE `item_template` SET
  `name` = 'Iron Shackles',
  `description` = 'Heavy irons. Whoever wears them does not stray far.',
  `Quality` = 3, `bonding` = 0, `maxcount` = 0, `stackable` = 1,
  `BuyPrice` = 0, `SellPrice` = 0, `RequiredLevel` = 0,
  `spellid_1` = 62646, `spelltrigger_1` = 0, `spellcooldown_1` = -1, `spellcategorycooldown_1` = -1,
  `ScriptName` = 'sanctuary_lawman_shackles'
WHERE `entry` = 2412;

UPDATE `item_template` SET
  `name` = 'Iron Shackle Key',
  `description` = 'A key is a key. It does not ask who turned the lock.',
  `Quality` = 3, `bonding` = 0, `maxcount` = 0, `stackable` = 1,
  `BuyPrice` = 0, `SellPrice` = 0, `RequiredLevel` = 0,
  `spellid_1` = 61410, `spelltrigger_1` = 0, `spellcooldown_1` = -1, `spellcategorycooldown_1` = -1,
  `ScriptName` = 'sanctuary_lawman_shackle_key'
WHERE `entry` = 5384;
