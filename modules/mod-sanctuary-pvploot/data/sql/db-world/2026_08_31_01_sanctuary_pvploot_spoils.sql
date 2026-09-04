--
-- The sack a defeated player's belongings fall into.
--
-- Type 10 (GOOBER), not 3 (CHEST). GameObject::Use runs sScriptMgr->OnGossipHello before
-- its type switch, and GOOBER is dispatched through Use, so the module's script gets the
-- chance to refuse anyone who is not the killer. CHEST is not handled in Use at all and
-- could not be restricted.
--
-- Data0 is the lock id, left at 0 so it opens with no key.
-- Data1 for a goober is the quest id, also 0 - and critically there is no loot id, so
-- Player::SendLoot never overwrites the contents the module wrote onto the object.
--
SET @SPOILS_ENTRY := 990001;

DELETE FROM `gameobject_template` WHERE `entry` = @SPOILS_ENTRY;
INSERT INTO `gameobject_template`
  (`entry`, `type`, `displayId`, `name`, `IconName`, `castBarCaption`, `size`,
   `Data0`, `Data1`, `Data2`, `Data3`, `ScriptName`)
VALUES
  (@SPOILS_ENTRY, 10, 323, 'Spilled Belongings', 'Interact', '', 1.0,
   0, 0, 0, 0, 'sanctuary_pvp_spoils');
