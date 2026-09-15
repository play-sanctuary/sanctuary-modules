--
-- The Timekeeper's per-level rates: regeneration, and crit from stats.
--
-- Class slot 10 was never a class, so every one of the game's per-class rate tables holds a
-- placeholder in its row. They are not harmless placeholders:
--
--   gtRegenMPPerSpt   row 900 = 0        mana regen from spirit is exactly zero, so a
--                                        Timekeeper's mana never comes back
--   gtRegenHPPerSpt   row 900 = 0        same for health
--   gtOCTRegenHP      row 900 = 1.0      out-of-combat health regen, wildly out of scale
--   gtChanceToMeleeCrit                  0.01 per point of agility - 1% each, ~100x real
--   gtChanceToMeleeCritBase  row 9 = 0.2 20% melee crit before any gear
--   gtChanceToSpellCritBase  row 9 = 0.2 20% spell crit before any gear
--   gtOCTClassCombatRatingScalar   = 0   every combat rating scaled to nothing
--
-- The core reads these from these tables rather than from its DBC files (they are shipped
-- fully populated, and the DBC loader lets the database override every row), so this is a
-- plain data fix - no client patch involved. The client keeps the placeholders in its own
-- copies, which only affects a couple of its local tooltip estimates; every value that
-- decides what actually happens comes from the server.
--
-- Each row is copied from the Timekeeper's donor class, Paladin (class 2) - the same class
-- its ChrClasses row was cloned from, a mana user in plate. Row indices are zero-based and
-- laid out by class:
--
--   per level   (class - 1) * 100 + (level - 1)   Paladin 100-199, Timekeeper 900-999   +800
--   per class   (class - 1)                       Paladin 1,       Timekeeper 9          +8
--   per rating  (class - 1) * 32 + rating + 1     Paladin 32-63,   Timekeeper 288-319   +256
--
-- REPLACE, and copied from the donor rather than written out as literals, so re-running this
-- is a no-op and the numbers cannot drift from the class they are meant to match. The donor
-- rows are read through a derived table because MySQL will not read the table an INSERT
-- targets directly.
--

-- Regeneration, per level.
REPLACE INTO `gtregenmpperspt_dbc` (`ID`, `Data`)
SELECT `ID` + 800, `Data` FROM (SELECT `ID`, `Data` FROM `gtregenmpperspt_dbc` WHERE `ID` BETWEEN 100 AND 199) donor;

REPLACE INTO `gtregenhpperspt_dbc` (`ID`, `Data`)
SELECT `ID` + 800, `Data` FROM (SELECT `ID`, `Data` FROM `gtregenhpperspt_dbc` WHERE `ID` BETWEEN 100 AND 199) donor;

REPLACE INTO `gtoctregenhp_dbc` (`ID`, `Data`)
SELECT `ID` + 800, `Data` FROM (SELECT `ID`, `Data` FROM `gtoctregenhp_dbc` WHERE `ID` BETWEEN 100 AND 199) donor;

-- Crit from agility and from intellect, per level.
REPLACE INTO `gtchancetomeleecrit_dbc` (`ID`, `Data`)
SELECT `ID` + 800, `Data` FROM (SELECT `ID`, `Data` FROM `gtchancetomeleecrit_dbc` WHERE `ID` BETWEEN 100 AND 199) donor;

REPLACE INTO `gtchancetospellcrit_dbc` (`ID`, `Data`)
SELECT `ID` + 800, `Data` FROM (SELECT `ID`, `Data` FROM `gtchancetospellcrit_dbc` WHERE `ID` BETWEEN 100 AND 199) donor;

-- Base crit, one row per class.
REPLACE INTO `gtchancetomeleecritbase_dbc` (`ID`, `Data`)
SELECT `ID` + 8, `Data` FROM (SELECT `ID`, `Data` FROM `gtchancetomeleecritbase_dbc` WHERE `ID` = 1) donor;

REPLACE INTO `gtchancetospellcritbase_dbc` (`ID`, `Data`)
SELECT `ID` + 8, `Data` FROM (SELECT `ID`, `Data` FROM `gtchancetospellcritbase_dbc` WHERE `ID` = 1) donor;

-- How each combat rating scales for the class.
REPLACE INTO `gtoctclasscombatratingscalar_dbc` (`ID`, `Data`)
SELECT `ID` + 256, `Data` FROM (SELECT `ID`, `Data` FROM `gtoctclasscombatratingscalar_dbc` WHERE `ID` BETWEEN 32 AND 63) donor;
