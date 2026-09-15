--
-- mod-sanctuary-downed - 81006 is called "Carry"
--
-- It was "Lifting", which described the moment rather than the thing. What the spell does is
-- carry somebody; the lift is only how it starts.
--
-- Both halves again. The name on the cast bar and in the spellbook comes from the CLIENT's
-- Spell.dbc, so patch-enUS-4.MPQ carries the rename too and neither copy is sufficient
-- alone: without the patch the bar still reads "Lifting" however this row is worded, and
-- without this row the server's SpellInfo disagrees with what the player is looking at.
--
-- The description is untouched - "Pick up someone over your shoulder" was already right.
--

UPDATE `spell_dbc`
   SET `Name_Lang_enUS` = 'Carry'
 WHERE `ID` = 81006;
