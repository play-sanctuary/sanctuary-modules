--
-- Takes "Place Object" (spell 81013) out of the spellbooks it was left in.
--
-- The panel learns the spell when it opens, and until now never took it back - so it stayed
-- in the spellbook of every character that had ever opened the panel. That is every
-- character on a game master's account, because security belongs to the account rather than
-- the character: an ordinary-looking character was carrying a game master's tool.
--
-- The module now takes it away when the panel closes and at logout (ForgetPlaceObjectSpell
-- in SanctuaryGm.cpp), so the spell lives exactly as long as the panel is open. This clears
-- out what was learned before that. Nothing is lost: opening the panel grants it again, and
-- only an authorised account can open the panel at all.
--

DELETE FROM `character_spell` WHERE `spell` = 81013;
