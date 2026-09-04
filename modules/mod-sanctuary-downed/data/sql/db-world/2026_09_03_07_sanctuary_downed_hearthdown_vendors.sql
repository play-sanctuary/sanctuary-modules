--
-- mod-sanctuary-downed - innkeepers sell Hearthdown
--
-- WHY: the item existed and nothing sold it. A revive route that only a GM can hand out is
-- not a route, and the two ways back up were meant to be a trained one and one anybody could
-- carry - which only works if anybody can actually buy it.
--
-- Innkeepers because it is called Hearthdown: ale, bread and a bed are the same idea, and an
-- inn is somewhere a player already walks past in every town rather than a errand.
--
-- Written as set operations over the innkeeper flag rather than a list of 125 entries. A
-- hand-written list would be wrong the first time somebody adds an inn, and would have to be
-- audited to know whether it was complete.
--
-- Note this edits stock creature_template rows for the eleven innkeepers who were not already
-- vendors. That is a custom-server change and a core update would overwrite it; re-running
-- this file puts it back.
--

-- 20 silver. SellPrice left at 5, so buying and reselling is a loss rather than a trade.
UPDATE `item_template` SET `BuyPrice` = 2000, `SellPrice` = 500 WHERE `entry` = 990004;

--
-- Eleven innkeepers keep a room but no stock. They need the vendor flag before a vendor
-- entry means anything - without it the "Browse goods" option never appears, and the
-- npc_vendor rows below would sit there doing nothing.
--
UPDATE `creature_template`
   SET `npcflag` = `npcflag` | 128                    -- UNIT_NPC_FLAG_VENDOR
 WHERE `npcflag` & 65536                              -- UNIT_NPC_FLAG_INNKEEPER
   AND NOT `npcflag` & 128;

--
-- One row per innkeeper. maxcount 0 is unlimited stock: a vial that has to be waited on is a
-- vial nobody relies on, and the point of it is being able to help somebody now.
--
DELETE FROM `npc_vendor` WHERE `item` = 990004;

INSERT INTO `npc_vendor` (`entry`, `slot`, `item`, `maxcount`, `incrtime`, `ExtendedCost`)
SELECT `entry`, 0, 990004, 0, 0, 0
  FROM `creature_template`
 WHERE `npcflag` & 65536;
