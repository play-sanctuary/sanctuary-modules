--
-- mod-sanctuary-downed - the server learns spell 81007 "Carried"
--
-- The aura a passenger wears while somebody is carrying them. It does nothing on its own -
-- the carry is a vehicle seat - but it is the ONLY signal the addon has for offering the
-- "Get down" button, and the only thing that tells a conscious captive what has happened to
-- them, since the screen otherwise simply starts moving.
--
-- It has been in patch-enUS-4.MPQ since the button was written, and nowhere else. That is
-- the whole bug: the client knew the spell and the server did not, so `AddAura(81007)` in
-- PickUp found no such spell and did nothing, silently. No aura, no button, and nothing in
-- any log to say so.
--
-- Both halves, every time. The client's copy is what names it on the debuff tray; this one
-- is what lets the server apply it at all. Spells 81000-81006 and 81008-81011 all had both;
-- 81007 was the one that slipped.
--
-- The values below mirror the client row exactly, because a spell that disagrees with itself
-- across the two is worse than one that is missing from both:
--
--   attributes 0x04000000 | 0x80000000  debuff, and not right-clickable off
--   duration index 21                   the -1 row in SpellDuration.dbc, so it lasts
--   cast index 1                        instant; nothing casts it, the module applies it
--   effect 6 aura 4                     APPLY_AURA / DUMMY - inert by design
--   target 1                            TARGET_UNIT_CASTER
--   visual 0                            no artwork; the icon and the tray are the point
--   icon 79                             Spell_Holy_LayOnHands, a pair of hands
--

DELETE FROM `spell_dbc` WHERE `ID` = 81007;

INSERT INTO `spell_dbc` (
  `ID`, `Attributes`, `CastingTimeIndex`, `DurationIndex`, `RangeIndex`, `InterruptFlags`,
  -- EffectAura_1, not EffectApplyAuraName_1: this table names the column after the DBC
  -- field, and the core's own enum name is not what it is called here.
  `EquippedItemClass`, `Effect_1`, `EffectAura_1`, `ImplicitTargetA_1`,
  `SpellVisualID_1`, `SpellIconID`, `SchoolMask`,
  `Name_Lang_enUS`, `Name_Lang_Mask`,
  `Description_Lang_enUS`, `Description_Lang_Mask`,
  `AuraDescription_Lang_enUS`, `AuraDescription_Lang_Mask`
) VALUES (
  81007,
  0x84000000,   -- AURA_IS_DEBUFF | NO_AURA_CANCEL: it sits with the debuffs, and a captive
                -- cannot right-click their way out of being carried.
  1,            -- instant
  21,           -- the -1 duration row: it lasts until the module takes it off
  1,            -- self
  0,
  -1,           -- requires no weapon
  6,            -- SPELL_EFFECT_APPLY_AURA
  4,            -- SPELL_AURA_DUMMY - it carries no behaviour, only meaning
  1,            -- TARGET_UNIT_CASTER
  0,
  79,
  1,
  'Carried', 16712190,
  'Over somebody''s shoulder, and going where they go.', 16712190,
  'Over somebody''s shoulder, and going where they go.', 16712190
);
