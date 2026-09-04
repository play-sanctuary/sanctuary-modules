--
-- mod-sanctuary-lawman - the permission a lawman needs for `.lawman`.
--
-- Role 199 is "Role: Player Commands", which every account holds. The command refuses
-- anyone who does not actually hold the office, so granting it to everybody costs nothing:
-- 100001 is voice, 100002 is outlaw, this is 100003.
--
-- `.lawman set` and `.lawman remove` use the core's RBAC_PERM_COMMAND_MODIFY_FACTION (549)
-- instead, the same permission `.outlaw set` uses.
--

DELETE FROM `rbac_linked_permissions` WHERE `id` = 199 AND `linkedId` = 100003;
DELETE FROM `rbac_permissions` WHERE `id` = 100003;

INSERT INTO `rbac_permissions` (`id`, `name`) VALUES (100003, 'Command: lawman');
INSERT INTO `rbac_linked_permissions` (`id`, `linkedId`) VALUES (199, 100003);
