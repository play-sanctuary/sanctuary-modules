--
-- mod-sanctuary-lawman - what the irons and the key say in the bag
--
-- An item tooltip is two separate strings from two separate places, and they had drifted
-- into doing the same job:
--
--   * "Use: ..." is the DESCRIPTION OF THE SPELL the item casts, read out of the client's
--     Spell.dbc. It should say what pressing the item does.
--   * the gold line under it is item_template.description, which is flavour and nothing
--     else.
--
-- Both were written as flavour, so the shackles never actually told anybody what they were
-- for. The spell text becomes instructional and the item text becomes scenery.
--
-- The key had no spell of its own to say it with - it was still casting 61410, borrowed,
-- with the borrowed spell's words. So it gets one, 81008 "Unshackling", for exactly the
-- reason Shackling exists: a spell's text lives in the client's own Spell.dbc and no amount
-- of correcting it server-side can change what a borrowed row says.
--
-- 81008 is never cast. sanctuary_lawman_shackle_key::OnUse answers and returns true, which
-- suppresses the cast entirely; the row exists so the tooltip has a line we wrote, and so
-- the core does not see an item pointing at a spell it has never heard of.
--
-- Both halves again: patch-enUS-4.MPQ carries the client's copy of 81001 and 81008. The
-- server's copy below is what keeps the item valid at load.
--

UPDATE `spell_dbc`
   SET `Description_Lang_enUS` = 'Chain someone in irons to prevent them from escaping and fighting.',
       `AuraDescription_Lang_enUS` = 'Chain someone in irons to prevent them from escaping and fighting.'
 WHERE `ID` = 81001;

DELETE FROM `spell_dbc` WHERE `ID` = 81008;

INSERT INTO `spell_dbc` (
  `ID`, `Attributes`, `CastingTimeIndex`, `InterruptFlags`, `DurationIndex`, `RangeIndex`,
  `EquippedItemClass`, `Effect_1`, `ImplicitTargetA_1`,
  `SpellVisualID_1`, `SpellIconID`, `SchoolMask`,
  `Name_Lang_enUS`, `Name_Lang_Mask`,
  `Description_Lang_enUS`, `Description_Lang_Mask`,
  `AuraDescription_Lang_enUS`, `AuraDescription_Lang_Mask`
) VALUES (
  81008,
  0,
  1,            -- instant. Turning a key is not a performance.
  0,
  0,
  1,
  -1,           -- requires no weapon
  3,            -- SPELL_EFFECT_DUMMY, aimed at the caster. The script does the real work.
  1,
  0,            -- no visual: the cast never happens, so there is nothing to play
  4400,
  1,
  'Unshackling', 16712190,
  'Free someone from their chains.', 16712190,
  'Free someone from their chains.', 16712190
);

UPDATE `item_template` SET `spellid_1` = 81008 WHERE `entry` = 990002;

-- And the flavour, which is now only flavour.
UPDATE `item_template`
   SET `description` = 'A pair of shackles and manacles.'
 WHERE `entry` = 990001;

UPDATE `item_template`
   SET `description` = 'A key that suspiciously fits every shackle and manacle.'
 WHERE `entry` = 990002;
