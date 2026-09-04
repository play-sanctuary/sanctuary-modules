--
-- mod-sanctuary-stash - the locksmith stops claiming ScriptName
--
-- 2026_09_03_03 attached the locksmith to the rogue trainers by writing
-- creature_template.ScriptName, and skipped any trainer that already had one so nothing
-- scripted would be clobbered. Exactly one trainer in the game hit that guard: Shenthul in
-- Orgrimmar, who carries npc_shenthul - the AI behind The Shattered Salute. He was the only
-- rogue trainer in the world with no option to copy a key, and there was no way to give him
-- one, because a creature has exactly one ScriptName and taking it would have removed his
-- GetAI along with his quest.
--
-- The script is now an AllCreatureScript. Those hooks run for every creature and run BEFORE
-- the ScriptName lookup in ScriptMgr::OnGossipHello, so the line is appended without owning
-- the slot. Shenthul keeps npc_shenthul and gains the option; nothing of his is displaced,
-- because npc_shenthul has no gossip handler of its own.
--
-- So the column can be handed back. This scopes the revert to the exact value 03 wrote,
-- which is the only way to be sure nothing else is being cleared: any trainer that had a
-- script of their own was never touched in the first place.
--

UPDATE `creature_template`
   SET `ScriptName` = ''
 WHERE `ScriptName` = 'sanctuary_stash_locksmith';
