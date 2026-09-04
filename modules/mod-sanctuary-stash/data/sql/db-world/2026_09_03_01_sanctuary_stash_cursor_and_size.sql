--
-- mod-sanctuary-stash - the right cursor, and a bigger box
--
-- 1. IconName was 'Open', which is not a cursor. The client picks the cursor by name and
--    silently falls back when it does not recognise one, so a strongbox got the gossip
--    cursor rather than the hand. 'Interact' is the stock name for the hand - it appears
--    on eleven Blizzard objects, all of them chests and doors, and 'Open' appears on
--    exactly one row in the whole database: ours.
--
-- 2. One more row and one more column: 7x4 = 28 becomes 8x5 = 40. The column count lives
--    in the addon; the slot count lives here, and the two have to agree or the last slots
--    are storage nobody can click.
--

UPDATE `gameobject_template` SET `IconName` = 'Interact' WHERE `entry` = 990100;

ALTER TABLE `sanctuary_stash` MODIFY COLUMN `slots` TINYINT UNSIGNED NOT NULL DEFAULT 40
  COMMENT 'how much it holds, 1-98; 40 is the addon grid of 8 across by 5 down';

UPDATE `sanctuary_stash` SET `slots` = 40 WHERE `slots` = 28;
