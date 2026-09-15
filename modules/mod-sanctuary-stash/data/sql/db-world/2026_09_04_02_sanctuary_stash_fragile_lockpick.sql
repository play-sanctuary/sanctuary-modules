--
-- mod-sanctuary-stash - 990006 comes back as a lockpick
--
-- The previous file retired this entry. It was the module's first key, it never worked, and
-- the palette of fifteen had replaced it. It returns as something the palette has no answer
-- for: a pick that opens a box it was not cut for, once, and does not survive it.
--
-- The reason it never worked is fixed at the same time. Nothing was wrong with its
-- item_template row - it had no row in the client's Item.dbc, so the client could not draw
-- an entry it had never heard of. make-patch.py now ships one, at display 57379
-- (INV_Misc_EngGizmos_SwissArmy): a tool's icon rather than a key's, because it is one.
--
-- stackable 5, because it is spent rather than kept, and somebody carrying picks is carrying
-- more than one. spellid_1 81010 is the same inert "Use:" the keys carry - the item script
-- answers and suppresses it, and for the pick it says what the pick will do rather than
-- which box it opens, since the answer is "whichever you like".
--
-- The module spends it only at the moment a lid actually lifts, and only when nothing else
-- in the bags fits that lock. Carrying the right key and a pick spends neither.
--

DELETE FROM `item_template` WHERE `entry` = 990006;

INSERT INTO `item_template`
  (`entry`, `class`, `subclass`, `name`, `description`, `displayid`, `Quality`, `Flags`,
   `bonding`, `maxcount`, `stackable`, `BuyPrice`, `SellPrice`, `RequiredLevel`,
   `InventoryType`, `Material`, `sheath`, `spellid_1`, `spelltrigger_1`, `spellcharges_1`,
   `spellcooldown_1`, `spellcategorycooldown_1`, `ScriptName`)
VALUES
  (990006, 15, 0, 'Fragile Lockpick',
   'Good for one lock. Not for the one after it.',
   57379, 3, 0, 0, 0, 5, 0, 0, 0, 0, 1, 0, 81010, 0, 0, -1, -1, 'sanctuary_stash_key');
