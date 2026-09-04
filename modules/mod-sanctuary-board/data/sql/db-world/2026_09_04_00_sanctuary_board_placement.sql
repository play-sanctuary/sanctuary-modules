--
-- mod-sanctuary-board - six boards moved to where they were finally put, and four turned
--
-- These positions were arrived at in game and existed only in the development database.
-- They match neither placement this module has shipped: not _01_towns (the first guesses)
-- and not _02_model (the mailbox anchors). Left uncaptured, the next database rebuild would
-- have thrown them away - `reinstall.sh db` drops all three databases and replays the
-- migrations, and the migrations did not know.
--
-- ROTATION IS THE QUATERNION, NOT `orientation`. GameObject::Create takes `data->rotation`
-- and passes it to SetWorldRotation with no fallback, so rotation0-3 decide which way a
-- board faces and `orientation` alone changes nothing you can see. The first version of
-- this file set orientation only, and the four turned boards stood facing their default.
--
-- Position and rotation are all that is copied. The rows these came from also carried
-- zoneId 0 and spawntimesecs 0 - artefacts of `.gobject` writing the row rather than
-- anything anyone chose - and taking those would discard the zone ids _02 set deliberately
-- and turn a 300 second respawn into 0.
--
-- Exodar, Silvermoon, Shattrath and Dalaran are absent: they were already right.
--
-- Note for whoever reads this next: 5800103 and 5800105-5800109 still carry the identity
-- quaternion with a non-zero `orientation`, which by the rule above means their orientation
-- is inert and they face the default direction. That is inherited from _02 and is left
-- alone here rather than quietly re-aiming boards nobody asked about.
--

SET @BOARD := 990000;

-- Stormwind, moved ~26 yd and turned
UPDATE `gameobject` SET
    `position_x` = -8858.840, `position_y` =   637.710, `position_z` =   96.2102, `orientation` = 1.98527,
    `rotation0`  = 0, `rotation1` = 0, `rotation2` = -0.837469, `rotation3` = -0.546485
  WHERE `guid` = 5800100 AND `id` = @BOARD;

-- Ironforge, ~7 yd and turned
UPDATE `gameobject` SET
    `position_x` = -4914.380, `position_y` =  -976.791, `position_z` =  501.4530, `orientation` = 2.25915,
    `rotation0`  = 0, `rotation1` = 0, `rotation2` = -0.904231, `rotation3` = -0.427044
  WHERE `guid` = 5800101 AND `id` = @BOARD;

-- Darnassus, ~22 yd and turned
UPDATE `gameobject` SET
    `position_x` =  9934.100, `position_y` =  2340.080, `position_z` = 1330.7800, `orientation` = 1.55895,
    `rotation0`  = 0, `rotation1` = 0, `rotation2` = -0.702905, `rotation3` = -0.711284
  WHERE `guid` = 5800102 AND `id` = @BOARD;

-- Orgrimmar, the largest move of the six, and turned twice
UPDATE `gameobject` SET
    `position_x` =  1619.120, `position_y` = -4392.910, `position_z` =   10.6388, `orientation` = 4.40447,
    `rotation0`  = 0, `rotation1` = 0, `rotation2` = -0.807179, `rotation3` =  0.590307
  WHERE `guid` = 5800104 AND `id` = @BOARD;

-- Thunder Bluff, ~6 yd. Not turned, so its rotation is left as _02 set it.
UPDATE `gameobject` SET
    `position_x` = -1263.520, `position_y` =    49.0116, `position_z` =  127.3660, `orientation` = 3.14200
  WHERE `guid` = 5800105 AND `id` = @BOARD;

-- Undercity, ~6 yd. Likewise not turned.
UPDATE `gameobject` SET
    `position_x` =  1556.270, `position_y` =   240.704, `position_z` =  -43.1026, `orientation` = 3.14200
  WHERE `guid` = 5800106 AND `id` = @BOARD;
