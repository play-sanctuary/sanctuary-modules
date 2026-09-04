--
-- The notice board object.
--
-- Type 2 (questgiver) rather than a decoration: GameObject::Use calls
-- sScriptMgr->OnGossipHello before its type switch, and type 2 is what makes the board
-- right-clickable in the first place. Data0 is the gossip menu id, left at 0 because the
-- script answers before the default questgiver handling is reached.
--
SET @BOARD_ENTRY := 990000;

DELETE FROM `gameobject_template` WHERE `entry` = @BOARD_ENTRY;
INSERT INTO `gameobject_template`
  (`entry`, `type`, `displayId`, `name`, `IconName`, `castBarCaption`, `size`, `Data0`, `ScriptName`)
VALUES
  (@BOARD_ENTRY, 2, 202, 'Notice Board', 'Speak', '', 1.5, 0, 'sanctuary_board');

--
-- Two boards to start with, at the spots players already gravitate to. Place the rest
-- wherever you like: stand where you want one and use
--     .gobject add 990000
-- or the Objects tab of the Sanctuary GM panel.
--
SET @BOARD_GUID := 5800000;

DELETE FROM `gameobject` WHERE `id` = @BOARD_ENTRY;

INSERT INTO `gameobject`
  (`guid`, `id`, `map`, `zoneId`, `areaId`, `spawnMask`, `phaseMask`,
   `position_x`, `position_y`, `position_z`, `orientation`,
   `rotation0`, `rotation1`, `rotation2`, `rotation3`, `spawntimesecs`, `animprogress`, `state`)
VALUES
  -- Stormwind, Trade District, by the fountain
  (@BOARD_GUID + 0, @BOARD_ENTRY, 0, 0, 0, 1, 1,
   -8833.38, 628.628, 94.0066, 3.5, 0, 0, 0, 1, 300, 100, 1),
  -- Orgrimmar, Valley of Strength
  (@BOARD_GUID + 1, @BOARD_ENTRY, 1, 0, 0, 1, 1,
   1633.75, -4439.60, 15.4396, 0.1, 0, 0, 0, 1, 300, 100, 1);
