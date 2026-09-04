--
-- mod-sanctuary-lawman - making the physical items behave like objects
--
-- 1. maxcount 1 is lifted from the shackles, the key and the Writ of Pardon. These three
--    are meant to change hands - that is the whole reason they are not soulbound - and a
--    limit of one made that awkward in a way that reads as a bug: a lawman could not carry
--    a spare, a fence could not hold two, and `.additem` on somebody who already had one
--    failed with an inventory error rather than anything explaining itself.
--
--    The Writ of Accusation keeps its limit. It is soulbound office paper that is handed
--    back out at every login, so a second copy would be meaningless.
--
-- 2. The Writ of Pardon stops being soulbound. It is a physical thing now, issued once with
--    the irons rather than reappearing whenever its owner goes on duty, and something that
--    can be carried, lost, handed to somebody else, or taken off a body.
--

UPDATE `item_template` SET `maxcount` = 0 WHERE `entry` IN (990001, 990002, 990003);

UPDATE `item_template` SET `bonding` = 0 WHERE `entry` = 990003;
