--
-- mod-sanctuary-lawman - the shackle debuff gets a spell of its own
--
-- WHY: the chain a prisoner wears has to be an aura. The client draws a lasting visual only
-- while an aura sits on the unit, so a one-shot packet cannot do it - and an aura is named,
-- in letters, above the head of everyone who looks at the prisoner.
--
-- Borrowing a retail chain spell borrows its name along with its artwork, and the name was
-- "Arcane Chains: Chain Channel". The client reads an aura's name from its OWN Spell.dbc,
-- so there is no server setting that changes it and no amount of correcting the SpellInfo
-- at load will help: the text never comes from the server at all.
--
-- So the shackles own a spell. patch-enUS-4.MPQ adds row 81000 to the client's Spell.dbc,
-- named "Iron Shackles" and pointing at the same chain artwork as before; this adds the
-- matching row on the server. AzerothCore supports exactly this - DBCStores loads
-- Spell.dbc and then overlays this table on top of it, growing its index table to fit ids
-- the DBC never had.
--
-- Both copies have to agree, and they divide the work: the client decides what the aura is
-- called and what it looks like, the server decides what it does.
--
--   81000  Iron Shackles   visual 10175 (state kit 1224, channel kit 8634), icon 4400
--
-- 81000 sits just above the client's highest spell id, 80864. An id parked far out - 990000,
-- to match the items - would cost megabytes of empty pointers in the index tables on both
-- sides and buy nothing.
--
-- What it does is nothing. The effect is one dummy aura; the disarm, the tether, the timer
-- and every rule about who may shackle whom live in the module. This row exists so the
-- chain has something to hang on and a name worth reading.
--

DELETE FROM `spell_dbc` WHERE `ID` = 81000;

INSERT INTO `spell_dbc` (
  `ID`, `Attributes`, `CastingTimeIndex`, `DurationIndex`, `RangeIndex`,
  `EquippedItemClass`, `Effect_1`, `ImplicitTargetA_1`, `EffectAura_1`,
  `SpellVisualID_1`, `SpellIconID`, `SchoolMask`,
  `Name_Lang_enUS`, `Name_Lang_Mask`,
  `Description_Lang_enUS`, `Description_Lang_Mask`,
  `AuraDescription_Lang_enUS`, `AuraDescription_Lang_Mask`
) VALUES (
  81000,
  2214592512,   -- AURA_IS_DEBUFF | NO_AURA_CANCEL. Forced negative and the client's cancel
                -- request refused, so a prisoner cannot right-click the chain off: the
                -- cancel handler checks both conditions and there is no reason to rely on
                -- only one of them.
  1,            -- instant. The module applies it directly; it is never actually cast.
  21,           -- the -1 row in SpellDuration - no duration at all. The module times it.
  1,
  -1,           -- requires no weapon. 0 would mean one is required.
  6,            -- SPELL_EFFECT_APPLY_AURA
  25,           -- TARGET_UNIT_TARGET_ANY
  4,            -- SPELL_AURA_DUMMY - inert. The artwork is the entire purpose.
  10175,        -- state kit 1224 worn by the prisoner, channel kit 8634 strung to the captor
  4400,         -- a SpellIcon row added by the patch: the item's own iron chain
  1,            -- physical, so no school immunity shrugs the irons off
  'Iron Shackles', 16712190,
  'Bound in iron, and going nowhere the one holding the chain does not.', 16712190,
  'Bound in iron, and going nowhere the one holding the chain does not.', 16712190
);
