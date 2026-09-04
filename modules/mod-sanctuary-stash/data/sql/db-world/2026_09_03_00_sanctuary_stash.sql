--
-- mod-sanctuary-stash - strongboxes, and what defines one
--
-- A strongbox is a gameobject plus a row here. The row says what it is called, how many
-- slots it has, and which item opens it; the gameobject says where it stands. Placing a new
-- one is a spawn and an INSERT, with no code change and no restart beyond a reload.
--
-- The key is an ITEM ENTRY, not a special kind of object, and that is deliberate. Any item
-- can be a key - including a stock retail one, which costs no client patch at all. A rusty
-- skeleton key from a vendor, a signet ring, a quest token somebody was given years ago:
-- name its entry here and it opens this box and no other. Possession is the whole of the
-- authority, as it is for the writs and the shackles, so a key can be copied, lost, stolen
-- off a body, or sold to a fence.
--
-- Type 2 is QUESTGIVER, which is what makes a gameobject offer gossip at all - the same
-- reason the notice board uses it. The strongbox never actually shows a gossip menu: the
-- module answers OnGossipHello, opens the window and returns true.
--

SET @STASH_GO   := 990100;
SET @STASH_KEY  := 990006;

--
-- The object itself. 259 is the stock reinforced chest; any chest display works, and the
-- one to change is displayId here rather than anything in code.
--
DELETE FROM `gameobject_template` WHERE `entry` = @STASH_GO;
INSERT INTO `gameobject_template`
  (`entry`, `type`, `displayId`, `name`, `IconName`, `castBarCaption`, `size`, `Data0`, `ScriptName`)
VALUES
  (@STASH_GO, 2, 259, 'Strongbox', 'Open', '', 1.0, 0, 'sanctuary_stash');

--
-- What each strongbox is. Keyed by the gameobject's SPAWN guid, not its entry, so that a
-- dozen boxes can share one model and still be a dozen separate strongboxes with a dozen
-- different keys. `guid` here is `gameobject`.`guid`.
--
CREATE TABLE IF NOT EXISTS `sanctuary_stash` (
  `guid`    INT UNSIGNED NOT NULL COMMENT 'gameobject spawn guid this strongbox lives in',
  `name`    VARCHAR(48) NOT NULL DEFAULT 'Strongbox' COMMENT 'shown on the window',
  `keyitem` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'item entry that opens it, 0 = unlocked',
  `slots`   TINYINT UNSIGNED NOT NULL DEFAULT 28 COMMENT 'how much it holds, 1-98',
  PRIMARY KEY (`guid`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_general_ci;

--
-- A key to start with. Nothing about it is special beyond being an item somebody can carry,
-- and it is only a key because a strongbox row names its entry.
--
DELETE FROM `item_template` WHERE `entry` = @STASH_KEY;
INSERT INTO `item_template`
  (`entry`, `class`, `subclass`, `name`, `description`, `displayid`, `Quality`, `Flags`,
   `bonding`, `maxcount`, `stackable`, `BuyPrice`, `SellPrice`, `RequiredLevel`,
   `InventoryType`, `Material`, `sheath`, `spellid_1`, `spelltrigger_1`, `spellcharges_1`)
VALUES
  (@STASH_KEY, 15, 0, 'Strongbox Key',
   'Cut for one lock, and no other.',
   13885, 3, 0,
   0, 0, 1, 0, 0, 0,
   0, 1, 0, 0, 0, 0);

--
-- PLACING ONE
--
--   .gobject add 990100           -- stand where it should be, note the guid it reports
--   INSERT INTO `sanctuary_stash` (`guid`, `name`, `keyitem`, `slots`)
--        VALUES (<that guid>, 'The Cellar Cache', 990006, 28);
--   .reload sanctuary_stash       -- or restart
--
-- Give the key out with `.additem 990006`. Whoever holds it can open that box; whoever does
-- not gets told it is locked.
--
