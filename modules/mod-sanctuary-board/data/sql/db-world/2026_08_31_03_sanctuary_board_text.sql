--
-- Flavour text for the notice board screens.
--
-- A board should read as furniture. Until now all four screens passed
-- DEFAULT_GOSSIP_MESSAGE, which is 0xffffff - a real npc_text row (16777215) whose text is
-- literally "Greetings, $n." So every board greeted you by name like an innkeeper.
--
-- A new file rather than an edit to _01 or _02: both are already recorded in
-- acore_world.updates, and changing an applied update trips the hash check instead of
-- re-running.
--
-- IDS
--
-- npc_text has its own id space. 990000-990001 there belong to mod-sanctuary-zidormi; the
-- fact that the board owns gameobject_template 990000 is a coincidence of numbering, not a
-- shared allocation. See the id table in Launcher/README.md.
--
-- Both text columns are filled, matching the shape zidormi's rows use. Only the first is
-- strictly required - QueryHandler.cpp:335 mirrors whichever is empty into the other, so a
-- female character never gets a blank header - but the duplication keeps the rows uniform.
--
-- $B is a line break. Apostrophes are doubled.
--

DELETE FROM `npc_text` WHERE `ID` IN (990010, 990011, 990012, 990013);
INSERT INTO `npc_text` (`ID`, `text0_0`, `text0_1`, `BroadcastTextID0`, `lang0`, `Probability0`) VALUES

-- The board itself.
(990010,
 'Old and new bills and posters cover the wooden board. Some are freshly pinned; others have weathered past reading.',
 'Old and new bills and posters cover the wooden board. Some are freshly pinned; others have weathered past reading.',
 0, 0, 1),

-- Reading one notice.
(990011,
 'One bill among the many, pinned at eye height.',
 'One bill among the many, pinned at eye height.',
 0, 0, 1),

-- Choosing what to write. Deliberately not board flavour: this is the moment you take out
-- charcoal, not the moment you walk up.
(990012,
 'There is a clear space near the bottom of the board, and a stub of charcoal hanging on a string.',
 'There is a clear space near the bottom of the board, and a stub of charcoal hanging on a string.',
 0, 0, 1),

-- Your own notices. This screen deletes: choosing a bill takes it down, with no
-- confirmation step. The text has to say so, because the rows are rendered by the same
-- Summarise() as the read-only list on the board and look identical to them.
(990013,
 'Your own bills, pinned among the rest.$B$BTake one down and it is gone from the board for good.',
 'Your own bills, pinned among the rest.$B$BTake one down and it is gone from the board for good.',
 0, 0, 1);
