--
-- mod-sanctuary-outlaw - a faction template for the wanted state
--
-- WHY: no stock faction template can be neutral to players AND leave ordinary monsters
-- hostile, and the first attempt at one failed in a way worth recording.
--
-- Booty Bay's template (121) has exactly the right masks - ourMask 0x1, friendlyMask 0x0,
-- hostileMask 0x8 - and it did not work at all. Unit::GetReactionTo has a player-vs-player
-- branch that answers from REPUTATION before it ever compares factions: if the target's
-- faction has a reputation entry, the attacker's standing with it decides, and anything
-- short of At War returns REP_FRIENDLY (Unit.cpp:7197). Booty Bay is a reputation faction,
-- so every player read a wanted outlaw as a friend and the server refused the attack with
-- "Invalid target" - while the CLIENT, which weighs it differently, happily showed a
-- hostile cursor and started the swing.
--
-- The requirement is therefore sharper than "neutral masks": the faction must have no
-- reputation entry at all, so that branch falls through to the faction comparison and to
-- IfNormalReaction. Player race factions have none, which is why the module's own reaction
-- hook works, and faction 14 has none, which is why the old hostile faction worked.
--
-- Faction 0 has no row in Faction.dbc whatsoever, so the lookup fails outright and the
-- branch cannot fire. That is what this template is built on.
--
--   FactionGroup 0x1   the player bit. Neither Alliance (friendlyMask 0x2) nor Horde
--                      (0x4) counts it as kin, and neither counts it as an enemy - so a
--                      wanted outlaw is NEUTRAL to every playable race in both directions.
--                      It also keeps ordinary monsters hostile: their EnemyGroup covers
--                      0x1, and a template with FactionGroup 0 would have made an outlaw
--                      invisible to every mob in the world.
--   FriendGroup  0x0   nobody claims them.
--   EnemyGroup   0x8   monsters remain their enemies, so aggro stays mutual.
--
-- Guards need no help staying out of it: a creature's reaction to a player is answered
-- from that player's reputation, so the watch goes on seeing a citizen in good standing
-- until UNIT_FLAG2_IGNORE_REPUTATION is set - which is what striking somebody earns.
--
-- 2300 is above the client's highest template id, 2236. The client's copy of this row ships
-- in patch-enUS-4.MPQ; without it the client falls back to its own file, reads no such
-- template, and renders an outlaw unattackably friendly.
--

DELETE FROM `factiontemplate_dbc` WHERE `ID` = 2300;

INSERT INTO `factiontemplate_dbc`
  (`ID`, `Faction`, `Flags`, `FactionGroup`, `FriendGroup`, `EnemyGroup`,
   `Enemies_1`, `Enemies_2`, `Enemies_3`, `Enemies_4`,
   `Friend_1`, `Friend_2`, `Friend_3`, `Friend_4`)
VALUES
  (2300, 0, 0, 1, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0);
