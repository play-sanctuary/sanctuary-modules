--
-- The chosen board model, and placement with room for it.
--
-- A new file again: 2026_08_31_01 is already recorded in acore_world.updates, so editing it
-- would trip the hash check rather than re-run.
--
-- MODEL
--
-- NewWantedPoster02 (3053) at scale 1.5, chosen by standing next to all five candidates.
-- It is the most substantial of them and reads as a town fixture rather than scenery.
--
-- PLACEMENT, WHICH THE MODEL CHANGED
--
-- 3053 is 0.55 x 2.37 x 2.29 yards, so at scale 1.5 it is 0.83 x 3.56 x 3.44 - a
-- half-footprint of 1.78 yards. The previous positions were placed to 2.0 yards of
-- clearance, which was fine for a flat poster and too tight for this: it would have
-- intersected whatever was next to it. Every board is therefore re-placed with 3.0 yards
-- clear, which is the half-footprint plus room for the neighbour's own bulk and for
-- walking past. Stormwind and Dalaran moved about 4 yards; the rest barely shifted.
--
-- Clearance is measured against the gameobject and creature tables, which do not contain
-- walls. Confirm by eye and nudge with .gobject add 990000 - see Launcher/README.md.
--

SET @BOARD := 990000;
SET @G     := 5800100;

UPDATE `gameobject_template` SET `displayId` = 3053, `size` = 1.5 WHERE `entry` = @BOARD;

-- The five model candidates spawned in Stormwind to compare. 990100-990199 is the reserved
-- scratch range, so this cannot reach anything permanent.
DELETE FROM `gameobject`          WHERE `id`    BETWEEN 990100 AND 990199;
DELETE FROM `gameobject_template` WHERE `entry` BETWEEN 990100 AND 990199;

DELETE FROM `gameobject` WHERE `id` = @BOARD;

INSERT INTO `gameobject`
  (`guid`, `id`, `map`, `zoneId`, `areaId`, `spawnMask`, `phaseMask`,
   `position_x`, `position_y`, `position_z`, `orientation`,
   `rotation0`, `rotation1`, `rotation2`, `rotation3`, `spawntimesecs`, `animprogress`, `state`)
VALUES
  -- Stormwind: 3.0 yd clear, by the mailbox at -8877 652
  (@G + 0 , @BOARD,   0, 1519, 0, 1, 1,  -8879.954,     652.399,    95.993, 6.152, 0, 0, 0, 1, 300, 100, 1),
  -- Ironforge: 3.0 yd clear, by the mailbox at -4910 -976
  (@G + 1 , @BOARD,   0, 1537, 0, 1, 1,  -4908.259,    -974.091,   501.408, 3.927, 0, 0, 0, 1, 300, 100, 1),
  -- Darnassus: 3.0 yd clear, by the mailbox at 9916 2348
  (@G + 2 , @BOARD,   1, 1657, 0, 1, 1,   9915.514,    2351.098,  1330.700, 4.974, 0, 0, 0, 1, 300, 100, 1),
  -- Exodar: 3.0 yd clear, by the mailbox at -3975 -11700
  (@G + 3 , @BOARD, 530, 3557, 0, 1, 1,  -3975.040,  -11697.000,  -139.258, 4.712, 0, 0, 0, 1, 300, 100, 1),
  -- Orgrimmar: 3.2 yd clear, by the mailbox at 1658 -4433
  (@G + 4 , @BOARD,   1, 1637, 0, 1, 1,   1661.334,   -4435.030,    17.482, 2.618, 0, 0, 0, 1, 300, 100, 1),
  -- Thunder Bluff: 3.0 yd clear, by the mailbox at -1263 45
  (@G + 5 , @BOARD,   1, 1638, 0, 1, 1,  -1260.310,      44.545,   127.545, 3.142, 0, 0, 0, 1, 300, 100, 1),
  -- Undercity: 3.0 yd clear, by the mailbox at 1555 235
  (@G + 6 , @BOARD,   0, 1497, 0, 1, 1,   1557.970,     235.108,   -43.201, 3.142, 0, 0, 0, 1, 300, 100, 1),
  -- Silvermoon City: 3.0 yd clear, by the mailbox at 9652 -7404
  (@G + 7 , @BOARD, 530, 3487, 0, 1, 1,   9652.132,   -7406.664,    13.628, 1.702, 0, 0, 0, 1, 300, 100, 1),
  -- Shattrath: 3.0 yd clear, by the mailbox at -1688 5509
  (@G + 8 , @BOARD, 530, 3703, 0, 1, 1,  -1690.688,    5507.744,    -9.808, 0.262, 0, 0, 0, 1, 300, 100, 1),
  -- Dalaran: 3.0 yd clear, by the mailbox at 5863 639
  (@G + 9 , @BOARD, 571, 4395, 0, 1, 1,   5862.670,     635.684,   647.160, 1.571, 0, 0, 0, 1, 300, 100, 1);
