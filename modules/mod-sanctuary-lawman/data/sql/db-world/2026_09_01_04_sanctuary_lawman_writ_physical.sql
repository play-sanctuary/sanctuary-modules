--
-- mod-sanctuary-lawman - the Writ of Accusation becomes a physical thing too
--
-- It was the last of the four still soulbound and still handed back at every login. Now it
-- is issued once with the rest of the kit and can be carried, lost, given away or taken off
-- a body, the same as the irons.
--
-- maxcount goes with it: a limit of one is wrong for something meant to change hands, and
-- it makes adding a second copy fail with an inventory error rather than anything that
-- explains itself.
--

UPDATE `item_template` SET `bonding` = 0, `maxcount` = 0 WHERE `entry` = 990000;
