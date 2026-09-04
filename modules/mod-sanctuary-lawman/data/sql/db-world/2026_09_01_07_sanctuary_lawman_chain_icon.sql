--
-- mod-sanctuary-lawman - the shackles get a chain, not a net
--
-- 2412's icon in the client is INV_Misc_Net_01, and the client's own Item.dbc is what
-- decides that - the server cannot change it. So the shackles move to an entry whose
-- client-side display is an actual chain.
--
-- 30509 "Armor Fragment" carries INV_Misc_WartornScrap_Chain and is the safest chain-iconed
-- entry available: it is class 15, no quest requires or rewards it, and the only thing that
-- referenced it was a single gameobject loot row, removed below so nobody finds a pair of
-- shackles in a chest. 39326 is literally called "Iron Chain" and was the obvious pick, but
-- a live quest uses it.
--
-- 2412 is handed back its old identity rather than left as a stray second copy.
--

DELETE FROM `gameobject_loot_template` WHERE `Item` = 30509;

UPDATE `item_template` SET
  `name` = 'Iron Shackles',
  `description` = 'Heavy irons. Whoever wears them does not stray far.',
  `Quality` = 3, `Flags` = 0, `bonding` = 0, `maxcount` = 0, `stackable` = 1,
  `BuyPrice` = 0, `SellPrice` = 0, `RequiredLevel` = 0,
  `spellid_1` = 62646, `spelltrigger_1` = 0, `spellcooldown_1` = -1, `spellcategorycooldown_1` = -1,
  `ScriptName` = 'sanctuary_lawman_shackles'
WHERE `entry` = 30509;

UPDATE `item_template` SET
  `name` = 'Deprecated Nightmare Bridle',
  `description` = '',
  `spellid_1` = 0, `spelltrigger_1` = 0,
  `ScriptName` = ''
WHERE `entry` = 2412;
