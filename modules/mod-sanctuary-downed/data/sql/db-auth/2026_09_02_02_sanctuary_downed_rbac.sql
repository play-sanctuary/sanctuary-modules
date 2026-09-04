--
-- `.carry` is a player command, so it needs its own permission linked under the player
-- role. 100001 is proximity voice, 100002 outlaw, 100003 lawman.
--
DELETE FROM `rbac_permissions` WHERE `id` = 100004;
INSERT INTO `rbac_permissions` (`id`, `name`) VALUES (100004, 'Command: carry');

-- 199 is "Role: Player Commands", which is where 100001-100003 already sit. 195 is
-- "Role: Sec Level Player" and would also grant it, but mixing the two would leave the
-- realm's player permissions split across two roles for no reason.
DELETE FROM `rbac_linked_permissions` WHERE `linkedId` = 100004;
INSERT INTO `rbac_linked_permissions` (`id`, `linkedId`) VALUES (199, 100004);
