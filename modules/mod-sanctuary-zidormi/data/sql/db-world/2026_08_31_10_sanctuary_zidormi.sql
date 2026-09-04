--
-- Zidormi outside the Ruins of Lordaeron.
--
-- She is not a custom creature. Entry 31848 already exists in 3.3.5a as the Caverns of
-- Time traveller in the Bronze Dragonshrine, with the right name, the right model and the
-- right faction, so this borrows her rather than inventing a lookalike.
--
-- The script is therefore attached to the *spawn*, not to creature_template: `creature`
-- carries its own `ScriptName`, and Creature::GetScriptId prefers it over the template's.
-- Putting it on the template instead would give the Dragonblight Zidormi this dialogue
-- too, replacing "Take me to the Caverns of Time" - a travel service players still use.
--

SET @ZIDORMI := 31848;
SET @SCRIPT  := 'sanctuary_zidormi';

--
-- What she says. Two rows: the greeting, and the aside for players who ask what she is.
--
-- text0_0 is the line as told to a male character and text0_1 to a female one; they are
-- the same here because nothing in them is gendered. BroadcastTextID0 stays 0 so the
-- strings below are what the client is sent, rather than a lookup into the DBC.
--

DELETE FROM `npc_text` WHERE `ID` IN (990000, 990001);
INSERT INTO `npc_text` (`ID`, `text0_0`, `text0_1`, `BroadcastTextID0`, `lang0`, `Probability0`) VALUES
(990000,
 'Time sits badly on this ground, traveller. What happened here is still happening, if you know how to look at it.$B$BI can show you Lordaeron as it was, before Putress'' betrayal - or return you to the hour you were born into. Say which, and be certain of it.',
 'Time sits badly on this ground, traveller. What happened here is still happening, if you know how to look at it.$B$BI can show you Lordaeron as it was, before Putress'' betrayal - or return you to the hour you were born into. Say which, and be certain of it.',
 0, 0, 1),
(990001,
 'I am of the bronze flight. We keep the thread of what happened straight, and mend it where others have pulled at it.$B$BWhat I offer you is not travel. You will not go anywhere. You will stand exactly where you stand and see a different hour of it - and those in the other hour will no longer see you, nor you them.',
 'I am of the bronze flight. We keep the thread of what happened straight, and mend it where others have pulled at it.$B$BWhat I offer you is not travel. You will not go anywhere. You will stand exactly where you stand and see a different hour of it - and those in the other hour will no longer see you, nor you them.',
 0, 0, 1);

--
-- Adopt whichever Zidormi is already standing in the Eastern Kingdoms. Deliberately
-- scoped to map 0: the stock spawn in the Bronze Dragonshrine is on map 571 and must keep
-- her own gossip.
--

UPDATE `creature` SET `ScriptName` = @SCRIPT WHERE `id` = @ZIDORMI AND `map` = 0;

--
-- ...and place one if nobody has. On the road outside the Ruins of Lordaeron, about thirty
-- yards short of the Undercity revelers, facing the approach.
--
-- Counted first, into a variable, so the INSERT does not read the table it writes to.
--

SET @ALREADY := (SELECT COUNT(*) FROM `creature` WHERE `id` = @ZIDORMI AND `map` = 0);
SET @GUID    := 5900000;

INSERT INTO `creature`
  (`guid`, `id`, `map`, `zoneId`, `areaId`, `spawnMask`, `phaseMask`, `equipment_id`,
   `position_x`, `position_y`, `position_z`, `orientation`,
   `spawntimesecs`, `wander_distance`, `currentwaypoint`, `curhealth`, `curmana`,
   `MovementType`, `npcflag`, `unit_flags`, `dynamicflags`, `ScriptName`)
SELECT @GUID, @ZIDORMI, 0, 0, 0, 1, 1, 0,
       1982.74, 245.845, 36.804, 3.67589,
       300, 0, 0, 12600, 0,
       0, 0, 0, 0, @SCRIPT
FROM DUAL
WHERE @ALREADY = 0;

--
-- phaseMask 1 above is not an oversight. A player in phase 1 sees her because 1 & 1, and a
-- player in 4294967295 sees her because that mask matches everything - so she is reachable
-- from both sides, which the one NPC who moves people between them has to be.
--
