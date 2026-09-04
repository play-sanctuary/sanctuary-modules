--
-- Smelling salts: the item that brings a downed player round.
--
-- Entry 990004 continues the range the lawman module opened at 990000. The client learns the
-- item's class, subclass and icon from its own Item.dbc row in patch-enUS-4.MPQ; everything
-- here is the half the server owns.
--
-- spellid_1 is 61410, the same inert "use" spell the lawman items carry. It exists only so
-- the client offers a Use option - the ItemScript does the actual work and consumes the vial.
--
DELETE FROM `item_template` WHERE `entry` = 990004;
INSERT INTO `item_template`
    (`entry`, `class`, `subclass`, `name`, `displayid`, `Quality`, `Flags`,
     `BuyCount`, `BuyPrice`, `SellPrice`, `InventoryType`, `stackable`, `MaxCount`,
     `spellid_1`, `spelltrigger_1`, `bonding`, `description`, `ScriptName`, `VerifiedBuild`)
VALUES
    (990004, 15, 0, 'Smelling Salts', 990004, 3, 0,
     1, 2500, 500, 0, 5, 0,
     61410, 0, 0,
     'Sharp enough to reach someone who has stopped listening.',
     'sanctuary_downed_smelling_salts', 0);
