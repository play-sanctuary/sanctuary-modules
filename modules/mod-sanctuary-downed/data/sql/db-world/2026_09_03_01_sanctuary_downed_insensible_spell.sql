--
-- mod-sanctuary-downed - the knocked-out player gets a debuff worth reading
--
-- WHY: the seconds of immunity after going down were invisible. The player had no way to
-- tell whether they were still safe, and an onlooker had no way to tell whether hitting them
-- would achieve anything. An aura says both, in the debuff tray, with a timer running.
--
-- The client half is row 81002 in patch-enUS-4.MPQ, which owns the name and the icon - the
-- client reads those from its OWN Spell.dbc and no server setting can change them. This is
-- the matching server row. Between them the client decides what it is called and the server
-- decides what it does, which here is nothing at all: the immunity is unit flags set by the
-- module and the countdown is the module's own. This aura exists to be looked at.
--
-- The module overwrites the duration from SanctuaryDowned.Downed.ImmunitySeconds when it
-- applies the aura, so the five seconds below is only the default the row carries. Without
-- that the icon could sit there claiming five seconds while the config said eight.
--
-- 81002 continues the range the lawman module opened at 81000, just above the client's
-- highest spell id of 80864. Ids parked far out - 990000, to match the items - would cost
-- megabytes of empty pointers in the index tables on both sides and buy nothing.
--
DELETE FROM `spell_dbc` WHERE `ID` = 81002;

INSERT INTO `spell_dbc` (
  `ID`, `Attributes`, `CastingTimeIndex`, `DurationIndex`, `RangeIndex`,
  `EquippedItemClass`, `Effect_1`, `ImplicitTargetA_1`, `EffectAura_1`,
  `SpellIconID`, `SchoolMask`,
  `Name_Lang_enUS`, `Name_Lang_Mask`,
  `Description_Lang_enUS`, `Description_Lang_Mask`,
  `AuraDescription_Lang_enUS`, `AuraDescription_Lang_Mask`
) VALUES (
  81002,
  2214592512,   -- AURA_IS_DEBUFF | NO_AURA_CANCEL: it reads as something happening TO them,
                -- and they cannot right-click away the only sign of how long they are safe.
  1,            -- instant; the module applies it directly rather than casting it
  28,           -- the 5000ms row in SpellDuration. Overwritten from config on application.
  1,
  -1,           -- no weapon required
  6,            -- SPELL_EFFECT_APPLY_AURA
  1,            -- TARGET_UNIT_CASTER - it is only ever put on the player themselves
  4,            -- SPELL_AURA_DUMMY. Inert: the flags do the work, this is the label on them.
  4400,         -- the icon row the patch already adds
  1,
  'Insensible', 16712190,
  'Out cold. Nothing can reach you, and you cannot reach anything.', 16712190,
  'Out cold. Nothing can reach you, and you cannot reach anything.', 16712190
);
