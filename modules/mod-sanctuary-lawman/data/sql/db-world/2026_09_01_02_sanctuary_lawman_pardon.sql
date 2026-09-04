--
-- mod-sanctuary-lawman - the Writ of Pardon, and two corrections
--
-- 1. The Writ of Accusation now uses INV_Scroll_04 (displayid 634) rather than the
--    generic parchment it was given first.
--
-- 2. A Writ of Pardon, entry 990003, undoing an accusation without anyone having to type
--    a command. Soulbound like the accusation writ - both are the office's own paper, not
--    something to hand around, which is the opposite of the shackles and their key.
--
-- 3. The Iron Shackles change to a SELF-cast spell, and that is a bug fix rather than a
--    preference. Aimed spells did not work: the candidates with a usable range all carry
--    an empty `Targets` mask in the client's own DBC, and the ones the server would accept
--    are *positive* spells - so target selection runs IsValidAssistTarget, which refuses a
--    hostile unit. An outlaw carries faction 14 and is hostile to everyone, so the shackles
--    failed with "Invalid Target" on exactly the person they exist for while working on an
--    innocent. Casting on the user instead leaves nothing to validate, and the prisoner is
--    read from the caster's selection.
--
--    32990 was chosen because it is self-cast, has a dummy effect, a visual, interrupts on
--    movement, and already has a cast time of exactly five seconds.
--

UPDATE `item_template` SET `displayid` = 634 WHERE `entry` = 990000;

UPDATE `item_template` SET `spellid_1` = 32990 WHERE `entry` = 990001;

DELETE FROM `item_template` WHERE `entry` = 990003;

INSERT INTO `item_template`
  (`entry`, `class`, `subclass`, `name`, `displayid`, `Quality`, `Flags`,
   `BuyPrice`, `SellPrice`, `InventoryType`, `ItemLevel`, `RequiredLevel`,
   `maxcount`, `stackable`, `bonding`,
   `spellid_1`, `spelltrigger_1`, `spellcharges_1`, `spellcooldown_1`,
   `spellcategory_1`, `spellcategorycooldown_1`,
   `Material`, `sheath`, `description`, `ScriptName`)
VALUES
  (990003, 15, 0, 'Writ of Pardon', 811, 3, 0,
   0, 0, 0, 1, 1,
   1, 1, 1,
   38067, 0, 0, -1,
   0, -1,
   8, 0,
   'The charge is dropped. Go, and be seen to have gone.', 'sanctuary_lawman_pardon');
