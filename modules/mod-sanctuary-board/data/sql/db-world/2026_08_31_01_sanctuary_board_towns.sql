--
-- Notice boards in every major town, and a model you can actually see.
--
-- A new file rather than an edit to 2026_08_31_00_sanctuary_board_object.sql: that one is
-- already recorded in acore_world.updates, and changing an applied update makes the core
-- complain about the hash instead of re-running it.
--
-- THE MODEL
--
-- The board shipped with displayId 202 (WantedPosterWood01), whose bounding box is
-- 0.05 x 0.76 x 1.15 yards - five centimetres wide and knee-high, a flat plane that
-- disappears edge-on. That is why nobody could find it. It is also an Orcish wanted
-- poster, which reads as litter in Stormwind.
--
-- 2491 (NewWantedPoster01) is 0.61 x 1.36 x 2.79 - taller than a player, with real depth,
-- and it lives under World\Generic\ rather than a racial folder, so it does not look
-- foreign in either faction's cities. 3.3.5a has no dedicated bulletin-board model; this
-- is the closest thing to one.
--
-- To try a different one, this is the only line that needs changing:
--     UPDATE `gameobject_template` SET `displayId` = <id>, `size` = <n> WHERE `entry` = 990000;
-- Candidates: 2491 board 2.79 tall | 17 framed 4.15 | 6135 signpost 5.01 | 6033 signpost 5.57
--
-- PLACEMENT
--
-- Each board stands 2.5 yards from the mailbox nearest that city's bank-and-auction hub.
-- Mailboxes make good anchors: they sit on ground players demonstrably stand on, in the
-- busiest part of town, and a notice board beside one is where you would expect to find it.
-- Every position was checked to be at least 2 yards clear of any existing gameobject or
-- creature spawn.
--
-- That check cannot see walls. Building geometry is not in the gameobject table, so
-- clearance from props is no proof a spot is not inside one. Confirm each board by eye and
-- nudge anything that looks wrong - see Launcher/README.md, "Moving a notice board".
--

SET @BOARD := 990000;
SET @G     := 5800100;

UPDATE `gameobject_template` SET `displayId` = 2491, `size` = 1.0 WHERE `entry` = @BOARD;

-- Replaces the original two spawns (guids 5800000-5800001) with a contiguous block.
DELETE FROM `gameobject` WHERE `id` = @BOARD;

INSERT INTO `gameobject`
  (`guid`, `id`, `map`, `zoneId`, `areaId`, `spawnMask`, `phaseMask`,
   `position_x`, `position_y`, `position_z`, `orientation`,
   `rotation0`, `rotation1`, `rotation2`, `rotation3`, `spawntimesecs`, `animprogress`, `state`)
VALUES
  -- Stormwind: 2.3 yd clear, beside the mailbox at -8877 652
  (@G + 0 , @BOARD,   0, 1519, 0, 1, 1,  -8876.546,     649.545,    95.993, 1.745, 0, 0, 0, 1, 300, 100, 1),
  -- Ironforge: 2.0 yd clear, beside the mailbox at -4910 -976
  (@G + 1 , @BOARD,   0, 1537, 0, 1, 1,  -4909.525,    -973.863,   501.408, 4.363, 0, 0, 0, 1, 300, 100, 1),
  -- Darnassus: 2.5 yd clear, beside the mailbox at 9916 2348
  (@G + 2 , @BOARD,   1, 1657, 0, 1, 1,   9915.856,    2350.662,  1330.700, 4.887, 0, 0, 0, 1, 300, 100, 1),
  -- Exodar: 2.5 yd clear, beside the mailbox at -3975 -11700
  (@G + 3 , @BOARD, 530, 3557, 0, 1, 1,  -3975.040,  -11697.500,  -139.258, 4.712, 0, 0, 0, 1, 300, 100, 1),
  -- Orgrimmar: 2.0 yd clear, beside the mailbox at 1658 -4433
  (@G + 4 , @BOARD,   1, 1637, 0, 1, 1,   1660.332,   -4433.464,    17.482, 2.967, 0, 0, 0, 1, 300, 100, 1),
  -- Thunder Bluff: 2.5 yd clear, beside the mailbox at -1263 45
  (@G + 5 , @BOARD,   1, 1638, 0, 1, 1,  -1260.961,      45.400,   127.545, 3.491, 0, 0, 0, 1, 300, 100, 1),
  -- Undercity: 2.5 yd clear, beside the mailbox at 1555 235
  (@G + 6 , @BOARD,   0, 1497, 0, 1, 1,   1557.432,     235.542,   -43.201, 3.316, 0, 0, 0, 1, 300, 100, 1),
  -- Silvermoon City: 2.2 yd clear, beside the mailbox at 9652 -7404
  (@G + 7 , @BOARD, 530, 3487, 0, 1, 1,   9652.990,   -7405.855,    13.628, 2.094, 0, 0, 0, 1, 300, 100, 1),
  -- Shattrath: 2.5 yd clear, beside the mailbox at -1688 5509
  (@G + 8 , @BOARD, 530, 3703, 0, 1, 1,  -1690.139,    5507.665,    -9.808, 0.349, 0, 0, 0, 1, 300, 100, 1),
  -- Dalaran: 2.1 yd clear, beside the mailbox at 5863 639
  (@G + 9 , @BOARD, 571, 4395, 0, 1, 1,   5860.170,     638.684,   647.160, 0.000, 0, 0, 0, 1, 300, 100, 1);
