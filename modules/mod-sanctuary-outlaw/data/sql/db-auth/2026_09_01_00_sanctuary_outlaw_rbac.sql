--
-- mod-sanctuary-outlaw - the permission ordinary players need for `.outlaw`.
--
-- Role 199 is "Role: Player Commands", which every account holds. 100001 is already taken
-- by mod-proximity-voice's `Command: voice`.
--
-- The game master forms, `.outlaw set` and `.outlaw clear`, use the core's existing
-- RBAC_PERM_COMMAND_MODIFY_FACTION (549) instead - which is honest, since changing
-- somebody's faction is precisely what they do.
--

DELETE FROM `rbac_linked_permissions` WHERE `id` = 199 AND `linkedId` = 100002;
DELETE FROM `rbac_permissions` WHERE `id` = 100002;

INSERT INTO `rbac_permissions` (`id`, `name`) VALUES (100002, 'Command: outlaw');
INSERT INTO `rbac_linked_permissions` (`id`, `linkedId`) VALUES (199, 100002);
