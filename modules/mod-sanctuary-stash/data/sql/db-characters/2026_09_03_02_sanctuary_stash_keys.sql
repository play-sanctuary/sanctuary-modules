--
-- mod-sanctuary-stash - a key is a physical object, not a kind of object
--
-- WHY: boxes were locked to an item ENTRY, so "do you have a key" meant "do you carry an
-- item of type X". Two boxes locked with the same entry therefore shared a key, and since
-- item entries cannot be created while the server runs, the number of distinguishable keys
-- was the number shipped in the client patch - fifteen. The sixteenth strongbox had to
-- collide with one of the first fifteen.
--
-- Binding by item_instance.guid removes the ceiling entirely. A key is now one specific row
-- in item_instance, cut for one lock, and a single entry can serve any number of boxes: two
-- Skeleton Keys can open two different strongboxes and neither opens the other's.
--
-- It is also what a key actually is. "The sort of key that opens this sort of box" was never
-- the idea.
--
-- The fifteen key items stop being a namespace and become a wardrobe: the entry decides what
-- a key LOOKS like, and this table decides what it opens.
--
-- Bindings whose item no longer exists are swept at startup. Item guids are not normally
-- reused, but a stale row inheriting a recycled guid would hand somebody a key they never
-- cut, and the sweep costs one statement.
--

CREATE TABLE IF NOT EXISTS `sanctuary_stash_key` (
  `item_guid` INT UNSIGNED NOT NULL COMMENT 'item_instance.guid - one physical key',
  `stash`     INT UNSIGNED NOT NULL COMMENT 'gameobject spawn guid it was cut for',
  PRIMARY KEY (`item_guid`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_general_ci;
