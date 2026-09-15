--
-- mod-sanctuary-faction - the permission an ordinary player needs for `.faction`.
--
-- Role 199 is "Role: Player Commands", which every account holds. Granting it to everybody
-- costs nothing because the command refuses anybody who is not in a faction, or not of a
-- rank that may do what they asked - and that refusal is server-side, which is the only
-- place it means anything.
--
-- 100001 is voice, 100002 outlaw, 100003 lawman, 100004 carry, 100005 disguise,
-- 100006 carry staging. This is 100007.
--
-- The game master half of the command tree - create, disband, rank, spell - uses the core's
-- RBAC_PERM_COMMAND_MODIFY_FACTION (549) instead, the same permission `.outlaw set`,
-- `.lawman set` and `.stash place` already use.
--

DELETE FROM `rbac_linked_permissions` WHERE `id` = 199 AND `linkedId` = 100007;
DELETE FROM `rbac_permissions` WHERE `id` = 100007;

INSERT INTO `rbac_permissions` (`id`, `name`) VALUES (100007, 'Command: faction');
INSERT INTO `rbac_linked_permissions` (`id`, `linkedId`) VALUES (199, 100007);
