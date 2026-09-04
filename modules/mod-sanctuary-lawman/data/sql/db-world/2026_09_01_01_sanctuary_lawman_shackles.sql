--
-- mod-sanctuary-lawman - Iron Shackles and the Iron Shackle Key
--
-- Entries 990001 and 990002. 990000 is the Writ of Accusation; see the custom entry id
-- table in Launcher/README.md before allocating another.
--
-- Both are deliberately NOT soulbound. The writ is a badge of office and binds; these are
-- meant to change hands, because a criminal obtaining a pair from a corrupt guard is the
-- reason they exist. Nothing in the module asks whether the person using them holds any
-- office - possession is the whole authority - so every use is logged instead.
--
-- The shackles' on-use spell, 10617 "Release Rageclaw", is not decoration and its name is
-- never seen. It was chosen from the client's own Spell.dbc because every value already
-- suits: RangeIndex 12 is 0-5 yards *client-side* (the client gates range from its own
-- copy before it will even send the packet, so a base spell with the wrong range would
-- desync), ImplicitTargetA is 25 (TARGET_UNIT_TARGET_ANY, the one target type that skips
-- the friend-or-foe check), its effect is a dummy so casting it does nothing of its own,
-- and its interrupt flags already include movement. Only the cast time is wrong - 10
-- seconds - and the module rewrites that to 5 at load through OnLoadSpellCustomAttr.
--
-- Unlike the writ, this cast is NOT suppressed: the ItemScript returns false so the core
-- runs a real cast, which is what gives a visible bar, movement interruption, and range
-- and line of sight checks both before and after the five seconds.
--
-- Icons are borrowed from real items so they exist client-side: 51750 is the Iron Chain's
-- display and 6708 is the stock Shackle Key's.
--
-- The key's spell is 38067, the same instant the writ uses, purely for the targeting
-- cursor; its cast is suppressed and the range checked in code.
--

DELETE FROM `item_template` WHERE `entry` IN (990001, 990002);

INSERT INTO `item_template`
  (`entry`, `class`, `subclass`, `name`, `displayid`, `Quality`, `Flags`,
   `BuyPrice`, `SellPrice`, `InventoryType`, `ItemLevel`, `RequiredLevel`,
   `maxcount`, `stackable`, `bonding`,
   `spellid_1`, `spelltrigger_1`, `spellcharges_1`, `spellcooldown_1`,
   `spellcategory_1`, `spellcategorycooldown_1`,
   `Material`, `sheath`, `description`, `ScriptName`)
VALUES
  (990001, 15, 0, 'Iron Shackles', 51750, 3, 0,
   0, 0, 0, 1, 1,
   1, 1, 0,
   10617, 0, 0, -1,
   0, -1,
   1, 0,
   'Heavy irons. Whoever wears them does not stray far.', 'sanctuary_lawman_shackles'),

  (990002, 15, 0, 'Iron Shackle Key', 6708, 3, 0,
   0, 0, 0, 1, 1,
   1, 1, 0,
   38067, 0, 0, -1,
   0, -1,
   1, 0,
   'A key is a key. It does not ask who turned the lock.', 'sanctuary_lawman_shackle_key');
