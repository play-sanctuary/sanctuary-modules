--
-- mod-sanctuary-stash - the keys named after what they look like
--
-- The fifteen keys were named in the order of their icons, Iron through Frostbound against
-- INV_Misc_Key_01 through _15, without anyone opening the textures. The icons are not in
-- that order, so almost none of the names matched: the "Brass Key" was a purple ornate one,
-- the "Iron Key" bright red, the "Glass Key" solid gold.
--
-- The icons were read out of the client's ItemDisplayInfo and looked at, and the names now
-- follow the picture. Nothing here changes an entry's displayid, so the client needs no new
-- Item.dbc row and no patch - entry order already ran with icon order, and only the words
-- were wrong.
--
--   entry   icon  is actually            named
--   990020  _01   red, ornate            Ruby
--   990021  _02   purple, ornate         Amethyst
--   990022  _03   pale and translucent   Quartz
--   990023  _04   gold, ornate           Topaz
--   990024  _05   warm brass             Brass
--   990025  _06   plain grey             Iron
--   990026  _07   long and thin          Fine
--   990027  _08   icy blue               Frostbound
--   990028  _09   curled                 Curved
--   990029  _10   pale and spiralled     Shell
--   990030  _11   dark steel             Skeleton   (the one name already right)
--   990031  _12   heavy, round bow       Strongbox
--   990032  _13   decorated gold         Grim
--   990033  _14   plain silver           Silver
--   990034  _15   green, etched          Rune-cut
--
-- Seven names survived the remap and take their old descriptions with them to whichever
-- entry now carries them - Brass keeps "Yellow with handling", Iron keeps "Plain, heavy".
-- The eight new names needed new lines.
--
-- Clients that have already seen these items hold the old names in Cache/WDB/itemcache.wdb
-- and will keep showing them until that is cleared.
--

UPDATE `item_template` SET `name` = 'Ruby Key',       `description` = 'The stone in it is worth more than the door.'      WHERE `entry` = 990020;
UPDATE `item_template` SET `name` = 'Amethyst Key',   `description` = 'Purple, and rather pleased about it.'              WHERE `entry` = 990021;
UPDATE `item_template` SET `name` = 'Quartz Key',     `description` = 'Clouded all the way through, and always cold.'     WHERE `entry` = 990022;
UPDATE `item_template` SET `name` = 'Topaz Key',      `description` = 'Yellow stone in yellow metal. Somebody''s taste.'  WHERE `entry` = 990023;
UPDATE `item_template` SET `name` = 'Brass Key',      `description` = 'Yellow with handling, and older than it looks.'    WHERE `entry` = 990024;
UPDATE `item_template` SET `name` = 'Iron Key',       `description` = 'Plain, heavy, and cut for one lock.'               WHERE `entry` = 990025;
UPDATE `item_template` SET `name` = 'Fine Key',       `description` = 'Thin as a hairpin, and about as strong.'           WHERE `entry` = 990026;
UPDATE `item_template` SET `name` = 'Frostbound Key', `description` = 'It never quite warms in the hand.'                 WHERE `entry` = 990027;
UPDATE `item_template` SET `name` = 'Curved Key',     `description` = 'It curls where a key should not, and turns anyway.' WHERE `entry` = 990028;
UPDATE `item_template` SET `name` = 'Shell Key',      `description` = 'Pale and spiralled, like something washed up.'     WHERE `entry` = 990029;
UPDATE `item_template` SET `name` = 'Skeleton Key',   `description` = 'Fits more locks than it should.'                   WHERE `entry` = 990030;
UPDATE `item_template` SET `name` = 'Strongbox Key',  `description` = 'Cut for one lock, and no other.'                   WHERE `entry` = 990031;
UPDATE `item_template` SET `name` = 'Grim Key',       `description` = 'Heavy at the head, and not friendly about it.'     WHERE `entry` = 990032;
UPDATE `item_template` SET `name` = 'Silver Key',     `description` = 'Too fine for a cellar door, and used on one anyway.' WHERE `entry` = 990033;
UPDATE `item_template` SET `name` = 'Rune-cut Key',   `description` = 'The marks along it mean nothing to you.'           WHERE `entry` = 990034;
