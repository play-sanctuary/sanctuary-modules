--
-- The staging half of `.carry` stops being a player command.
--
-- `.carry testgrid` spawns four hundred morphed dummies, and it summons them with
-- TEMPSUMMON_MANUAL_DESPAWN - they do not time out, they wait to be cleared. Under
-- 100004 that was reachable by anybody with a character: one command, four hundred
-- creatures that stay, and nothing stopping a second command after it.
--
-- `.carry seat` and `.carry mountseat` go with it. They are tuning tools that take an
-- arbitrary vehicle id and put whoever you are carrying into that seat, which is fine
-- for measuring offsets and not something a player needs.
--
-- What stays on 100004: bare `.carry`, `.carry drop`, `.bleedout` and `.getdown`. Those
-- are the mechanic itself, and every one of them acts only on the caster or on somebody
-- who is already in their arms.
--
-- 100001 voice, 100002 outlaw, 100003 lawman, 100004 carry, 100005 disguise.
--
DELETE FROM `rbac_permissions` WHERE `id` = 100006;
INSERT INTO `rbac_permissions` (`id`, `name`) VALUES (100006, 'Command: carry staging');

-- 197 is "Role: Gamemaster Commands", reached from secId 2 via 193. The player role, 199,
-- deliberately does not link it - that is the entire point of the file.
DELETE FROM `rbac_linked_permissions` WHERE `linkedId` = 100006;
INSERT INTO `rbac_linked_permissions` (`id`, `linkedId`) VALUES (197, 100006);
