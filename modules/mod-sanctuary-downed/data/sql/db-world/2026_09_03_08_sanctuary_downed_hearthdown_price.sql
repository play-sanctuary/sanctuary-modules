--
-- mod-sanctuary-downed - Hearthdown costs 50 silver, and sits at the top of the innkeeper's list
--
-- WHY the price went up: 20 silver is pocket change past the first few levels, and a revive
-- anybody can carry should cost enough that carrying a stack of them is a decision. 50 silver
-- is still inside a low-level character's means for one vial.
--
-- SellPrice is deliberately left at 5 silver rather than scaled with it. The gap is the point:
-- buying to resell is a 90% loss, so the innkeepers cannot be farmed.
--

UPDATE `item_template` SET `BuyPrice` = 5000 WHERE `entry` = 990004;

--
-- WHY the slot is negative: the vendor list is sent in load order, and the load order is
-- `ORDER BY entry, slot ASC, item, ExtendedCost` in ObjectMgr::LoadVendors. Every stock row
-- on every innkeeper is slot 0, so within that tie the list falls back to item id ascending -
-- and 990004 is a custom entry, higher than anything Blizzard shipped, which buried Hearthdown
-- at the bottom of the list behind the tea and the cheese.
--
-- slot is a signed smallint and the core never reads the value: it is selected only as an
-- ORDER BY key and is not part of VendorItem. So -1 costs nothing and puts this one row ahead
-- of the stock list without rewriting the slot of every other item on 125 innkeepers, which is
-- the alternative and which a core update would then undo.
--

UPDATE `npc_vendor` SET `slot` = -1 WHERE `item` = 990004;
