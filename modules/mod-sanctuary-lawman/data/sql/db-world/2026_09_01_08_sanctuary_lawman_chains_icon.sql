--
-- mod-sanctuary-lawman - an icon that actually looks like a chain
--
-- INV_Misc_WartornScrap_Chain, despite the name, is a scrap of chain MAIL. Of every icon
-- reachable here only two contain a chain at all and are not armour, because the icon has
-- to come from an ItemDisplayInfo row that some Item.dbc entry already points at - a new
-- item cannot bring its own, since the client reads display data from its own files and has
-- no row for anything we invent.
--
-- The other one is Spell_Frost_ChainsOfIce: unmistakably a chain, though an icy blue rather
-- than iron grey. 33185 "Adamantite Weapon Chain" carries it and is referenced by no quest,
-- no loot table and no vendor.
--
-- 30509 goes back to being Armor Fragment. Its loot row was deleted when it was taken and
-- is restored here.
--

UPDATE `item_template` SET
  `name` = 'Iron Shackles',
  `description` = 'Heavy irons. Whoever wears them does not stray far.',
  `Quality` = 3, `Flags` = 0, `bonding` = 0, `maxcount` = 0, `stackable` = 1,
  `BuyPrice` = 0, `SellPrice` = 0, `RequiredLevel` = 0, `ItemLevel` = 1,
  `spellid_1` = 62646, `spelltrigger_1` = 0, `spellcooldown_1` = -1, `spellcategorycooldown_1` = -1,
  `spellid_2` = 0, `spelltrigger_2` = 0,
  `ScriptName` = 'sanctuary_lawman_shackles'
WHERE `entry` = 33185;

UPDATE `item_template` SET
  `name` = 'Armor Fragment',
  `description` = '',
  `spellid_1` = 0, `spelltrigger_1` = 0,
  `ScriptName` = ''
WHERE `entry` = 30509;

DELETE FROM `gameobject_loot_template` WHERE `Entry` = 21029 AND `Item` = 30509;
INSERT INTO `gameobject_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`)
VALUES (21029, 30509, 0, 7, 0, 1, 0, 1, 1);
