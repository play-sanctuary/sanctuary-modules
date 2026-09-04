--
-- Temporary scaffolding: the notice board candidates, side by side in Stormwind.
--
-- Run this by hand. It deliberately does NOT live under data/sql/, because everything
-- there is applied automatically and hash-tracked, which is the wrong lifecycle for props
-- that get deleted again as soon as a choice is made.
--
--   "C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe" -uroot -p acore_world < compare_models.sql
--
-- Then in game:  .reload gameobject_template  and  .reload gameobject
-- Go to the Stormwind Trade District and walk the two rows. Mouse over any of them to see
-- which model and scale it is.
--
-- The problem being solved: the live board uses displayId 202 (WantedPosterWood01), whose
-- bounding box is 0.05 x 0.76 x 1.15 yards - five centimetres wide and knee-high. These
-- are the alternatives that actually exist in 3.3.5a; there is no dedicated bulletin-board
-- model, so the choice is between a poster-board and a signpost.
--
-- Type 2 throughout, matching the real board, so what you are looking at renders exactly
-- as the real one will. None carries a ScriptName, so they are inert scenery.
--

SET @X := -8833.38;   -- the live Stormwind board
SET @Y :=   628.63;
SET @Z :=    94.01;
SET @O :=     3.50;

DELETE FROM `gameobject_template` WHERE `entry` BETWEEN 990101 AND 990110;
DELETE FROM `gameobject`          WHERE `id`    BETWEEN 990101 AND 990110;

--
-- Row one, scale 1.0. Row two, scale 1.5 - the scale the board is configured at today.
-- Scale is half this decision: the current model is proof that a number on paper is not
-- the same as a thing you can pick out of a crowded Trade District.
--
INSERT INTO `gameobject_template`
  (`entry`, `type`, `displayId`, `name`, `IconName`, `castBarCaption`, `size`, `Data0`, `ScriptName`)
VALUES
  (990101, 2, 2491, '[1] NewWantedPoster01 (2491) scale 1.0',   'Speak', '', 1.0, 0, ''),
  (990102, 2, 3053, '[2] NewWantedPoster02 (3053) scale 1.0',   'Speak', '', 1.0, 0, ''),
  (990103, 2,   17, '[3] WantedPosterFramed01 (17) scale 1.0',  'Speak', '', 1.0, 0, ''),
  (990104, 2, 6135, '[4] WoodSignPostNice01 (6135) scale 1.0',  'Speak', '', 1.0, 0, ''),
  (990105, 2, 6033, '[5] HumanSignPost01 (6033) scale 1.0',     'Speak', '', 1.0, 0, ''),
  (990106, 2, 2491, '[6] NewWantedPoster01 (2491) scale 1.5',   'Speak', '', 1.5, 0, ''),
  (990107, 2, 3053, '[7] NewWantedPoster02 (3053) scale 1.5',   'Speak', '', 1.5, 0, ''),
  (990108, 2,   17, '[8] WantedPosterFramed01 (17) scale 1.5',  'Speak', '', 1.5, 0, ''),
  (990109, 2, 6135, '[9] WoodSignPostNice01 (6135) scale 1.5',  'Speak', '', 1.5, 0, ''),
  (990110, 2, 6033, '[10] HumanSignPost01 (6033) scale 1.5',    'Speak', '', 1.5, 0, '');

--
-- Laid out as two rows of five, four yards apart, six yards between the rows. All at the
-- board's own Z and facing, so any difference you see is the model and nothing else.
--
INSERT INTO `gameobject`
  (`guid`, `id`, `map`, `zoneId`, `areaId`, `spawnMask`, `phaseMask`,
   `position_x`, `position_y`, `position_z`, `orientation`,
   `rotation0`, `rotation1`, `rotation2`, `rotation3`, `spawntimesecs`, `animprogress`, `state`)
VALUES
  (5800010, 990101, 0, 0, 0, 1, 1, @X +  0, @Y, @Z, @O, 0, 0, 0, 1, 300, 100, 1),
  (5800011, 990102, 0, 0, 0, 1, 1, @X +  4, @Y, @Z, @O, 0, 0, 0, 1, 300, 100, 1),
  (5800012, 990103, 0, 0, 0, 1, 1, @X +  8, @Y, @Z, @O, 0, 0, 0, 1, 300, 100, 1),
  (5800013, 990104, 0, 0, 0, 1, 1, @X + 12, @Y, @Z, @O, 0, 0, 0, 1, 300, 100, 1),
  (5800014, 990105, 0, 0, 0, 1, 1, @X + 16, @Y, @Z, @O, 0, 0, 0, 1, 300, 100, 1),
  (5800015, 990106, 0, 0, 0, 1, 1, @X +  0, @Y + 6, @Z, @O, 0, 0, 0, 1, 300, 100, 1),
  (5800016, 990107, 0, 0, 0, 1, 1, @X +  4, @Y + 6, @Z, @O, 0, 0, 0, 1, 300, 100, 1),
  (5800017, 990108, 0, 0, 0, 1, 1, @X +  8, @Y + 6, @Z, @O, 0, 0, 0, 1, 300, 100, 1),
  (5800018, 990109, 0, 0, 0, 1, 1, @X + 12, @Y + 6, @Z, @O, 0, 0, 0, 1, 300, 100, 1),
  (5800019, 990110, 0, 0, 0, 1, 1, @X + 16, @Y + 6, @Z, @O, 0, 0, 0, 1, 300, 100, 1);

SELECT CONCAT('Spawned ', COUNT(*), ' comparison props beside the Stormwind board.') AS result
FROM `gameobject` WHERE `id` BETWEEN 990101 AND 990110;
