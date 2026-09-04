--
-- Bind the spirit healers to the module.
--
-- 6491 and 29259 are the only two creature templates in the world database carrying
-- UNIT_NPC_FLAG_SPIRITHEALER (16384), and neither had a ScriptName, so this takes nothing
-- over from anybody. Selected by the flag rather than by id so a future healer that gains
-- it is picked up too.
--
-- The script answers OnGossipHello and returns true, which suppresses the stock gossip
-- menu. That is what stops the "resurrect me" option existing, which stops spell 17251
-- casting, which stops the client ever being sent SMSG_SPIRIT_HEALER_CONFIRM - the packet
-- it answers with the resurrection sickness warning.
--
-- No npc_text row is needed: the dialogue is built from config as gossip rows, the same way
-- mod-sanctuary-board renders a notice.
--
UPDATE `creature_template`
SET `ScriptName` = 'sanctuary_spirit_healer'
WHERE `npcflag` & 16384
  AND (`ScriptName` = '' OR `ScriptName` = 'sanctuary_spirit_healer');
