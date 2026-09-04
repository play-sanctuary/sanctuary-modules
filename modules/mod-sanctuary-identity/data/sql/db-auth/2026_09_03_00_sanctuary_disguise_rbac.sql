--
-- mod-sanctuary-identity - the permission ordinary players need for `.disguise`.
--
-- Role 199 is "Role: Player Commands", which every account holds. 100001-100004 are already
-- taken by voice, outlaw, lawman and carry.
--
DELETE FROM `rbac_linked_permissions` WHERE `id` = 199 AND `linkedId` = 100005;
DELETE FROM `rbac_permissions` WHERE `id` = 100005;

INSERT INTO `rbac_permissions` (`id`, `name`) VALUES (100005, 'Command: disguise');
INSERT INTO `rbac_linked_permissions` (`id`, `linkedId`) VALUES (199, 100005);
