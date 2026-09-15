--
-- mod-sanctuary-stash - two key descriptions that did not fit their keys
--
-- Both are left over from the rename, where seven names moved to new entries and took their
-- old lines with them. The lines travelled with the NAME, which was right for most of them
-- and wrong for these two.
--
--   Silver  - "Too fine for a cellar door, and used on one anyway" was written for a key
--             that looked precious. It now sits on INV_Misc_Key_14, which is the plainest
--             key in the set, so the line argued with the picture.
--
--   Ruby    - "worth more than the door" was the wrong comparison. A door is not what the
--             key is measured against; the lock is.
--

UPDATE `item_template` SET `description` = 'Plain silver, worn smooth at the grip.'        WHERE `entry` = 990033;
UPDATE `item_template` SET `description` = 'The stone in it is worth more than the lock.'  WHERE `entry` = 990020;
