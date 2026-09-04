--
-- mod-sanctuary-lawman - the Writ of Accusation
--
-- Entry 990000 in the 990xxx range Sanctuary uses for its own content. It is the first
-- custom item_template row; see the custom entry id table in Launcher/README.md before
-- allocating another.
--
-- The on-use spell, 38067 "Guard's Mark", is not a decoration and is not cast. It exists
-- so the CLIENT offers a targeting cursor: it is instant, costs nothing, and its
-- ImplicitTargetA is 25 (TARGET_UNIT_TARGET_ANY), which is the one target type
-- SpellInfo::CheckExplicitTarget short-circuits with no friend-or-foe check - so the writ
-- can be pointed at a peaceful player of your own side. The module's ItemScript returns
-- true from OnUse, which suppresses the cast entirely, so the spell's own name and visual
-- are never seen.
--
-- Soulbound with no sell price: a badge of office, not loot. It is handed out when the
-- office is granted and re-granted at login if it has gone missing.
--

DELETE FROM `item_template` WHERE `entry` = 990000;

INSERT INTO `item_template`
  (`entry`, `class`, `subclass`, `name`, `displayid`, `Quality`, `Flags`,
   `BuyPrice`, `SellPrice`, `InventoryType`, `ItemLevel`, `RequiredLevel`,
   `maxcount`, `stackable`, `bonding`,
   `spellid_1`, `spelltrigger_1`, `spellcharges_1`, `spellcooldown_1`,
   `spellcategory_1`, `spellcategorycooldown_1`,
   `Material`, `sheath`, `description`, `ScriptName`)
VALUES
  (990000, 15, 0, 'Writ of Accusation', 3331, 3, 0,
   0, 0, 0, 1, 1,
   1, 1, 1,
   38067, 0, 0, -1,
   0, -1,
   8, 0,
   'By the authority of the watch, this hand is raised against you.', 'sanctuary_lawman_writ');
