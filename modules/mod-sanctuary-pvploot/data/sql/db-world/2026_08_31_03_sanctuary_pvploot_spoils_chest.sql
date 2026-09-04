--
-- Turns the spoils sack into an ordinary lootable chest.
--
-- It was a GOOBER (type 10), chosen because GameObject::Use runs OnGossipHello before its
-- type switch and GOOBER is dispatched through Use. That reasoning was right but
-- incomplete: it got the sack opened, and never got it *clickable*.
--
-- GameObject::BuildValuesUpdate only sets GO_DYNFLAG_LO_ACTIVATE - the flag that makes the
-- client offer an object to be clicked - on a CHEST or GOOBER when ActivateToQuest() is
-- true, or when the viewer is a game master. ActivateToQuest is entirely quest-driven, so a
-- sack with no quest attached was interactable for a GM and inert for everybody else. That
-- is exactly how it presented: visible on the ground, and impossible to open.
--
-- A chest is what the client already understands as "a container you loot", so it needs no
-- dynamic flag to be clickable. The killer-only restriction survives the change: Use()
-- calls OnGossipHello before the switch, so sanctuary_pvp_spoils still gets first refusal
-- even though the switch has no CHEST case of its own.
--
-- Data1 (lootId) stays 0. Player::SendLoot only refills a chest from
-- LootTemplates_Gameobject when GetLootId() is non-zero, so leaving it at 0 is what keeps
-- the contents the module wrote onto the object.
--
-- Data3 (consumable) is 1: a spilled sack is a one-shot, and should go once emptied.
--
SET @SPOILS_ENTRY := 990001;

DELETE FROM `gameobject_template` WHERE `entry` = @SPOILS_ENTRY;
INSERT INTO `gameobject_template`
  (`entry`, `type`, `displayId`, `name`, `IconName`, `castBarCaption`, `size`,
   `Data0`,   -- lockId: 0, opens with no key
   `Data1`,   -- lootId: 0, so SendLoot never overwrites our contents
   `Data2`,   -- chestRestockTime
   `Data3`,   -- consumable: gone once looted
   `Data15`,  -- groupLootRules: off, the killer's spoils are not rolled for
   `Data16`,  -- floatingTooltip: name it on hover
   `ScriptName`)
VALUES
  (@SPOILS_ENTRY, 3, 323, 'Spilled Belongings', 'Interact', '', 1.0,
   0, 0, 0, 1, 0, 1,
   'sanctuary_pvp_spoils');

-- The sack is built in memory at the moment of a kill and never saved, so any spawn row on
-- this entry is debris from earlier scaffolding.
DELETE FROM `gameobject` WHERE `id` = @SPOILS_ENTRY;
