--
-- The box a standing player becomes.
--
-- A copy of Blizzard's own gameobject 188215, "Collision PC Size": type 5 (generic), display
-- 7735, World\Generic\Collision\Collision_PCSize.mdx - an invisible model that is nothing but
-- a person-sized collision volume. Blizzard placed it about 1,470 times in the stock world,
-- and every 3.3.5a client already has it, so nothing needs patching on the client side.
--
-- A copy rather than 188215 itself so the ScriptName below can be set without touching
-- Blizzard's placed ones. The script is what hides the box from its own player.
--
-- 990300 sits clear of the other Sanctuary gameobjects: 990000 (the notice board), 990001
-- (the spoils sack) and 990100-990199 (strongboxes).
--

SET @BLOCK := 990300;

DELETE FROM `gameobject_template` WHERE `entry` = @BLOCK;
INSERT INTO `gameobject_template` (`entry`, `type`, `displayId`, `name`, `size`, `ScriptName`) VALUES
(@BLOCK, 5, 7735, 'Sanctuary Collision', 1, 'sanctuary_collision_block');

--
-- Keep the box out of the server's own line of sight. The client collides with it; the
-- server must not, or a person standing in the way would block spells cast past them.
--
-- sourceType 7 is DISABLE_TYPE_GO_LOS.
--

DELETE FROM `disables` WHERE `sourceType` = 7 AND `entry` = @BLOCK;
INSERT INTO `disables` (`sourceType`, `entry`, `flags`, `params_0`, `params_1`, `comment`) VALUES
(7, @BLOCK, 0, '', '', 'mod-sanctuary-collision: a standing player must not block line of sight');
