--
-- The box is no longer spawned into the world with a script of its own.
--
-- It used to be a real gameobject in the map, with an AI (sanctuary_collision_block) that hid
-- it from the player it stood for. Now each nearby player is sent their own copy by hand and
-- the box never enters the map at all, so there is nothing for a script to do - and a
-- ScriptName the core cannot find is reported as an error at every startup.
--

UPDATE `gameobject_template` SET `ScriptName` = '' WHERE `entry` = 990300;
