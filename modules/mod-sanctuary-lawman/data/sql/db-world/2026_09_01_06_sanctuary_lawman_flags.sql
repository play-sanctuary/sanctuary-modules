--
-- mod-sanctuary-lawman - clearing the flag that hid the items
--
-- ITEM_FLAG_DEPRECATED (0x10) was inherited from the retail rows these four ride on, and
-- never cleared: repurposing an entry means inheriting everything about it that was not
-- explicitly overwritten. The client does not render an item carrying it, which is why the
-- Iron Shackle Key never appeared in a bag however correct the rest of the row was.
--
-- 5384 also carried 0x800 (multi-drop), which is meaningless here. All four are set to 0.
--
-- The Writ of Pardon's wording changes at the same time, to something that reads like a
-- verdict rather than a shrug.
--

UPDATE `item_template` SET `Flags` = 0 WHERE `entry` IN (3513, 1078, 2412, 5384);

UPDATE `item_template`
   SET `description` = 'The watch has seen you proven innocent.'
 WHERE `entry` = 1078;
