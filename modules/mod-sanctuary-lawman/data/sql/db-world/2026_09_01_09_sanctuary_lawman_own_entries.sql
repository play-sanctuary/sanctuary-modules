--
-- mod-sanctuary-lawman - the four items move onto entries we own
--
-- Borrowing retail entries is over. It failed four separate times, each for a different
-- inherited property that was invisible until it bit: ITEM_FLAG_DEPRECATED hid the item,
-- spellcharges of -1 consumed it on use, subclass "Item Enhancement" made the client ask
-- for a weapon to enchant instead of running the script, and the only chain-shaped icons
-- available belonged to weapon-chain consumables carrying all three problems at once.
--
-- The four entries below are ours. They need the client patch that ships beside this module
-- (tools/assets/make-patch.py builds patch-enUS-4.MPQ), because the 3.3.5a client reads an
-- item's class, subclass and display from its OWN Item.dbc and renders a question mark for
-- anything it has never heard of. With the patch installed they have real icons drawn for
-- them; without it they work but show no icon.
--
-- Everything borrowed is handed back below, from the values in
-- data/sql/base/db_world/item_template.sql rather than from memory.
--

-- --- give back what was borrowed ------------------------------------------

UPDATE `item_template` SET
       `name` = 'Deprecated Writ of Lakeshire',
       `description` = 'Signed by the Honorable Magistrate Solomon.',
       `Quality` = 1,
       `Flags` = 16,
       `bonding` = 4,
       `maxcount` = 0,
       `stackable` = 10,
       `BuyPrice` = 0,
       `SellPrice` = 0,
       `RequiredLevel` = 0,
       `ItemLevel` = 1,
       `spellid_1` = 0,
       `spelltrigger_1` = 0,
       `spellcharges_1` = 0,
       `spellcooldown_1` = -1,
       `spellcategory_1` = 0,
       `spellcategorycooldown_1` = -1,
       `spellid_2` = 0,
       `spelltrigger_2` = 0,
       `ScriptName` = ''
 WHERE `entry` = 1078;

UPDATE `item_template` SET
       `name` = 'Deprecated Nightmare Bridle',
       `description` = '',
       `Quality` = 1,
       `Flags` = 16,
       `bonding` = 0,
       `maxcount` = 0,
       `stackable` = 1,
       `BuyPrice` = 750000,
       `SellPrice` = 187500,
       `RequiredLevel` = 40,
       `ItemLevel` = 40,
       `spellid_1` = 0,
       `spelltrigger_1` = 0,
       `spellcharges_1` = 0,
       `spellcooldown_1` = -1,
       `spellcategory_1` = 0,
       `spellcategorycooldown_1` = -1,
       `spellid_2` = 0,
       `spelltrigger_2` = 0,
       `ScriptName` = ''
 WHERE `entry` = 2412;

UPDATE `item_template` SET
       `name` = 'Deprecated Contract for the Magistrate',
       `description` = '',
       `Quality` = 1,
       `Flags` = 16,
       `bonding` = 4,
       `maxcount` = 1,
       `stackable` = 1,
       `BuyPrice` = 0,
       `SellPrice` = 0,
       `RequiredLevel` = 0,
       `ItemLevel` = 1,
       `spellid_1` = 0,
       `spelltrigger_1` = 0,
       `spellcharges_1` = 0,
       `spellcooldown_1` = -1,
       `spellcategory_1` = 0,
       `spellcategorycooldown_1` = -1,
       `spellid_2` = 0,
       `spelltrigger_2` = 0,
       `ScriptName` = ''
 WHERE `entry` = 3513;

UPDATE `item_template` SET
       `name` = 'Deprecated Tower of Althalaxx Key',
       `description` = '',
       `Quality` = 1,
       `Flags` = 2064,
       `bonding` = 1,
       `maxcount` = 1,
       `stackable` = 1,
       `BuyPrice` = 0,
       `SellPrice` = 0,
       `RequiredLevel` = 0,
       `ItemLevel` = 1,
       `spellid_1` = 0,
       `spelltrigger_1` = 0,
       `spellcharges_1` = 0,
       `spellcooldown_1` = -1,
       `spellcategory_1` = 0,
       `spellcategorycooldown_1` = -1,
       `spellid_2` = 0,
       `spelltrigger_2` = 0,
       `ScriptName` = ''
 WHERE `entry` = 5384;

UPDATE `item_template` SET
       `name` = 'Armor Fragment',
       `description` = '',
       `Quality` = 0,
       `Flags` = 0,
       `bonding` = 0,
       `maxcount` = 0,
       `stackable` = 5,
       `BuyPrice` = 220,
       `SellPrice` = 55,
       `RequiredLevel` = 0,
       `ItemLevel` = 1,
       `spellid_1` = 0,
       `spelltrigger_1` = 0,
       `spellcharges_1` = 0,
       `spellcooldown_1` = -1,
       `spellcategory_1` = 0,
       `spellcategorycooldown_1` = -1,
       `spellid_2` = 0,
       `spelltrigger_2` = 0,
       `ScriptName` = ''
 WHERE `entry` = 30509;

UPDATE `item_template` SET
       `name` = 'Adamantite Weapon Chain',
       `description` = '',
       `Quality` = 2,
       `Flags` = 64,
       `bonding` = 0,
       `maxcount` = 0,
       `stackable` = 5,
       `BuyPrice` = 72000,
       `SellPrice` = 18000,
       `RequiredLevel` = 50,
       `ItemLevel` = 63,
       `spellid_1` = 42687,
       `spelltrigger_1` = 0,
       `spellcharges_1` = -1,
       `spellcooldown_1` = -1,
       `spellcategory_1` = 0,
       `spellcategorycooldown_1` = -1,
       `spellid_2` = 0,
       `spelltrigger_2` = 0,
       `ScriptName` = ''
 WHERE `entry` = 33185;

-- --- and take four of our own ---------------------------------------------

DELETE FROM `item_template` WHERE `entry` BETWEEN 990000 AND 990003;

INSERT INTO `item_template`
  (`entry`, `class`, `subclass`, `name`, `displayid`, `Quality`, `Flags`,
   `BuyPrice`, `SellPrice`, `InventoryType`, `ItemLevel`, `RequiredLevel`,
   `maxcount`, `stackable`, `bonding`,
   `spellid_1`, `spelltrigger_1`, `spellcharges_1`, `spellcooldown_1`,
   `spellcategory_1`, `spellcategorycooldown_1`,
   `Material`, `sheath`, `description`, `ScriptName`)
VALUES
  -- spellcharges 0 on every one of them: -1 means "one use, then destroy the item", which
  -- is what silently ate a pair of shackles the first time this was tried.
  (990000, 15, 0, 'Writ of Accusation', 990000, 3, 0,
   0, 0, 0, 1, 0, 0, 1, 0,
   61410, 0, 0, -1, 0, -1,
   8, 0, 'By the authority of the watch, this hand is raised against you.',
   'sanctuary_lawman_writ'),

  (990003, 15, 0, 'Writ of Pardon', 990003, 3, 0,
   0, 0, 0, 1, 0, 0, 1, 0,
   61410, 0, 0, -1, 0, -1,
   8, 0, 'The watch has seen you proven innocent.',
   'sanctuary_lawman_pardon'),

  (990001, 15, 0, 'Iron Shackles', 990001, 3, 0,
   0, 0, 0, 1, 0, 0, 1, 0,
   62646, 0, 0, -1, 0, -1,
   1, 0, 'Heavy irons. Whoever wears them does not stray far.',
   'sanctuary_lawman_shackles'),

  (990002, 15, 0, 'Iron Shackle Key', 990002, 3, 0,
   0, 0, 0, 1, 0, 0, 1, 0,
   61410, 0, 0, -1, 0, -1,
   1, 0, 'A key is a key. It does not ask who turned the lock.',
   'sanctuary_lawman_shackle_key');
