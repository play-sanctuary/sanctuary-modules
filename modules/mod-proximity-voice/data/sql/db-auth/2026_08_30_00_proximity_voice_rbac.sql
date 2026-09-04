-- mod-proximity-voice: RBAC permission for the player-facing .voice commands
--
-- 100001 is linked under 199 ("Role: Player Commands"), which is what a normal
-- account already holds, so every player can run .voice without a GM level.

DELETE FROM `rbac_linked_permissions` WHERE `id` = 199 AND `linkedId` = 100001;
DELETE FROM `rbac_permissions` WHERE `id` = 100001;

INSERT INTO `rbac_permissions` (`id`, `name`) VALUES
(100001, 'Command: voice');

INSERT INTO `rbac_linked_permissions` (`id`, `linkedId`) VALUES
(199, 100001);
