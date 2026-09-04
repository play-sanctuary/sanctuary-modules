--
-- mod-sanctuary-stash - rogues cut keys
--
-- Bring a key, pay, walk away with a second one. That is what makes a key worth owning
-- rather than an administrative fact: anybody holding one can copy it, so lending a key is
-- a decision, and so is leaving one in a strongbox somebody else can open.
--
-- A copy is the same item entry as the original, and could not be anything else - both are
-- rows in item_instance carrying that entry, and a box only ever asks whether you have one.
-- Nothing distinguishes the first key from the tenth.
--
-- WHY THE ROGUE TRAINERS, AND WHY A SCRIPT RATHER THAN A MENU ROW
--
-- The obvious approach is a `gossip_menu_option` row on each trainer's menu, and it cannot
-- work. A database option reaches scripts with sender 0 and action = its OptionType, so
-- ours would be indistinguishable from every other plain gossip line on the same menu - and
-- an OptionType invented to make it unique is hidden outright, because PrepareGossipMenu
-- ends its validation switch with `default: canTalk = false`.
--
-- So the trainers carry the script instead. It does NOT replace their menu: it builds their
-- stock one itself, quests and training included, and appends one line. Anything it does not
-- recognise is handed straight back to the core.
--
-- Only trainers with no script of their own are taken, so nothing already scripted is
-- clobbered.
--

UPDATE `creature_template`
   SET `ScriptName` = 'sanctuary_stash_locksmith'
 WHERE `subname` LIKE '%Rogue Trainer%'
   AND (`ScriptName` = '' OR `ScriptName` IS NULL);

--
-- The header on the list of keys he will copy. SendGossipMenuFor needs a real npc_text row;
-- without one the window opens empty and looks broken rather than silent.
--
DELETE FROM `npc_text` WHERE `ID` = 990200;
INSERT INTO `npc_text` (`ID`, `text0_0`, `BroadcastTextID0`)
VALUES
  (990200,
   'Show me what you carry and I will cut you another. I do not ask what it opens, and I do not remember faces.',
   0);
