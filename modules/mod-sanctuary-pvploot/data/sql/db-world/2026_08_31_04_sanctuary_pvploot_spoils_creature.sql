--
-- Moves the spoils sack from a gameobject to a lootable creature corpse.
--
-- The gameobject route was abandoned after it was proven, by trace, to be unusable. The
-- server side worked perfectly: the script hook fired, ownership matched, distance and
-- phase were correct, and Player::SendLoot returned a populated loot guid - it really was
-- sending a loot window, with an item in it. The client simply discarded it every time.
--
-- The reason is structural rather than a bug. WorldSession::HandleLootOpcode begins
--
--     if (!player->IsAlive() || !guid.IsCreatureOrVehicle())
--         return;
--
-- so CMSG_LOOT is only ever accepted for creatures. A gameobject can never be the subject
-- of a client-initiated loot, and GameObject::Use has no CHEST case at all - real chests
-- are opened by the client casting Opening, which routes through Spell::EffectOpenLock.
-- Pushing SMSG_LOOT_RESPONSE at a client that never asked for loot gets it ignored.
--
-- A creature corpse is the path every mob in the game uses, and it hands us two things the
-- gameobject could not:
--
--   * the client initiates the loot itself, so it is expecting the response;
--   * killer-only access for free. Player::SendLoot resolves a creature's loot recipient to
--     OWNER_PERMISSION for that player and NONE_PERMISSION for everyone else, so the
--     restriction no longer needs a script at all.
--
-- Display 26252 is the Goblin Sapper Backpack: a satchel sitting on the ground, which is
-- what a dropped pack should look like.
--
-- faction 35 is friendly to everyone and unit_flags 0x300 is immune to players and NPCs, so
-- nothing can attack the sack in the instant before the module kills it. It is deliberately
-- NOT flagged unselectable - a corpse has to be clickable to be looted.
--
-- lootid stays 0: with a loot template the core would fill the corpse from it and discard
-- the victim's actual belongings.
--
SET @SPOILS_ENTRY := 990001;

DELETE FROM `creature_template` WHERE `entry` = @SPOILS_ENTRY;
INSERT INTO `creature_template`
  (`entry`, `name`, `subname`, `minlevel`, `maxlevel`, `faction`, `npcflag`, `unit_class`,
   `unit_flags`, `flags_extra`, `type`, `lootid`, `RegenHealth`, `rank`, `MovementType`,
   `speed_walk`, `speed_run`, `AIName`, `ScriptName`)
VALUES
  (@SPOILS_ENTRY, 'Spilled Belongings', '', 1, 1, 35, 0, 1,
   768, 0x00200000, 10, 0, 0, 0, 0,
   1.0, 1.14286, '', '');

-- flags_extra 0x00200000 is CREATURE_FLAG_EXTRA_NO_PLAYER_DAMAGE_REQ, and without it the
-- sack cannot be looted at all. Creature::IsDamageEnoughForLootingAndReward is
--
--     HasFlagsExtra(NO_PLAYER_DAMAGE_REQ) || (_playerDamageReq == 0 && _damagedByPlayer)
--
-- and nobody ever damages this corpse - the module conjures it already dead - so the second
-- clause can never be true. Player::isAllowedToLoot refuses on that alone, which means the
-- client is never shown the corpse as lootable and never asks to loot it.

-- Scale belongs to creature_template_model.DisplayScale below, not here: creature_template
-- has no scale column, and referencing one aborts the whole startup rather than the query.

DELETE FROM `creature_template_model` WHERE `CreatureID` = @SPOILS_ENTRY;
INSERT INTO `creature_template_model`
  (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`)
VALUES
  (@SPOILS_ENTRY, 0, 26252, 1.0, 1.0);

-- The gameobject the sack used to be. Removed so nothing is left claiming this entry, and
-- so the module's startup check cannot be satisfied by the wrong kind of object.
DELETE FROM `gameobject` WHERE `id` = @SPOILS_ENTRY;
DELETE FROM `gameobject_template` WHERE `entry` = @SPOILS_ENTRY;
