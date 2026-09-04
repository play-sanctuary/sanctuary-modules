--
-- mod-sanctuary-stash - what is actually in the boxes
--
-- This lives in the characters database because the items do: a strongbox holds real rows
-- from `item_instance`, not entry-and-count pairs. That distinction is the whole design.
--
-- Storing entry+count would be far simpler and quietly wrong. An enchanted sword deposited
-- and withdrawn would come back plain; a half-charged wand would come back full; a battered
-- shield would come back pristine. A strongbox would be a laundry for item state. So the
-- module does what the guild bank does: it takes the SAME item, clears its owner, and saves
-- it standing on its own -
--
--     item->SetGuidValue(ITEM_FIELD_CONTAINED, ObjectGuid::Empty);
--     item->SetGuidValue(ITEM_FIELD_OWNER, ObjectGuid::Empty);
--     item->FSetState(ITEM_NEW);
--     item->SaveToDB(trans);
--
-- and this table is the map from a box and a slot to that item's guid.
--
-- An ownerless row in `item_instance` with no row here is an orphan and is lost to the
-- world, which is why every deposit writes both halves inside one transaction.
--

CREATE TABLE IF NOT EXISTS `sanctuary_stash_item` (
  `stash`     INT UNSIGNED NOT NULL COMMENT 'gameobject spawn guid of the strongbox',
  `slot`      TINYINT UNSIGNED NOT NULL COMMENT 'which slot in that box',
  `item_guid` INT UNSIGNED NOT NULL COMMENT 'item_instance.guid - a real item, not a copy',
  PRIMARY KEY (`stash`, `slot`),
  UNIQUE KEY `item_guid` (`item_guid`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_general_ci;
