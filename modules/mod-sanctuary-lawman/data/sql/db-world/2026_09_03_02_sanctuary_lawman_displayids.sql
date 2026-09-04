--
-- mod-sanctuary-lawman - the four items point at the right table again
--
-- item_template.displayid is an ItemDisplayInfo id. It is not an Item.dbc id, and the two
-- being numbered alike here is what made the mistake easy to miss.
--
-- 2026_09_01_02 had this right: it set the Writ of Accusation to 634, a real display row
-- carrying INV_Scroll_04. Then 2026_09_01_09 moved all four onto our own entries and
-- re-inserted them with displayid set to the entry itself - 990000 pointing at 990000 -
-- which is a row in the client's Item.dbc, not a row in ItemDisplayInfo.
--
-- Proven by Hearthdown rather than assumed: it shipped with displayid 990004 and would not
-- draw its feather until 2026_09_03_05 corrected it to 99000, the display row the patch
-- actually adds. Same mistake, same fix, four items later.
--
-- The numbers below are the same ones make-patch.py puts in the Item.dbc rows, which is
-- where the fallback has presumably been coming from. Both now agree, and neither depends
-- on the other:
--
--   634    INV_Scroll_04    - the Writ of Accusation
--   811    INV_Scroll_03    - the Writ of Pardon
--   18172  INV_Belt_18      - the Iron Shackles, borrowed for its chain-link artwork
--   6708   INV_Misc_Key_03  - the Iron Shackle Key, the stock shackle key
--
-- All four are stock rows the client already has, so nothing here needs a new patch.
--

UPDATE `item_template` SET `displayid` = 634   WHERE `entry` = 990000;
UPDATE `item_template` SET `displayid` = 811   WHERE `entry` = 990003;
UPDATE `item_template` SET `displayid` = 18172 WHERE `entry` = 990001;
UPDATE `item_template` SET `displayid` = 6708  WHERE `entry` = 990002;
