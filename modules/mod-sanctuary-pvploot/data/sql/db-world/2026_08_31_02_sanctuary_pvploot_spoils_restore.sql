--
-- Restores the sack a defeated player's belongings fall into.
--
-- Entry 990001 was overwritten by mod-sanctuary-board's model-comparison scaffolding,
-- which claimed 990001-990010 and left a type-2 wanted poster with no ScriptName here.
-- GameObject::Create still succeeded on that row, so the module believed it had dropped a
-- sack while actually leaving an unlootable prop at the corpse.
--
-- This is a new file rather than an edit to 2026_08_31_01_sanctuary_pvploot_spoils.sql,
-- because that one is already recorded in acore_world.updates with its hash: editing an
-- applied update makes the core report a mismatch instead of re-running it.
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

-- The scaffolding also spawned a prop on this entry in Stormwind. The sack is created in
-- memory at the moment of a kill and never saved, so any row here is left over.
DELETE FROM `gameobject` WHERE `id` = @SPOILS_ENTRY;
