--
-- mod-sanctuary-downed - Smelling Salts becomes Hearthdown, and neither revive works in a fight
--
-- The rename carries an icon change with it. An item's bag icon comes from ItemDisplayInfo
-- and a spell's from SpellIcon, and nothing already in the client pairs them: no display row
-- carries Spell_Magic_FeatherFall, and the nearest alternatives are the INV_Feather drawings,
-- which are a different picture. So the patch adds display 99000 of its own, and the vial in
-- the bag and the entry in the spellbook are now the same art.
--
-- Attribute 0x10000000 is the other half of this: neither revive can be started while the
-- caster is in combat. There is a separate attribute for refusing a target who is in combat
-- and it is deliberately not set - a downed player has had CombatStop called on them anyway,
-- and it is the rescuer who should have to disengage first.
--

UPDATE `spell_dbc`
   SET `Attributes` = 268435456,   -- SPELL_ATTR0_NOT_IN_COMBAT_ONLY_PEACEFUL
       -- 1237 is already INV_Misc_Bandage_08 in the stock client, so nothing is added for
       -- it. Hearthdown keeps the feather: the trained hand and the improvised vial should
       -- not look like the same thing in a spellbook.
       `SpellIconID` = 1237,
       `Description_Lang_enUS` = 'Use your expertise in healing to awaken the unconscious.',
       `AuraDescription_Lang_enUS` = 'Use your expertise in healing to awaken the unconscious.'
 WHERE `ID` = 81004;

UPDATE `spell_dbc`
   SET `Attributes` = 268435456,
       `SpellIconID` = 4401,
       `Name_Lang_enUS` = 'Hearthdown',
       `Description_Lang_enUS` = 'The smell of ale and bread should revitalize anybody unconscious.',
       `AuraDescription_Lang_enUS` = 'The smell of ale and bread should revitalize anybody unconscious.'
 WHERE `ID` = 81005;

--
-- The item follows the spell it casts. displayid points at the patch's own row so the client
-- draws the feather; the description is what shows in the tooltip under the name.
--
UPDATE `item_template`
   SET `name` = 'Hearthdown',
       `displayid` = 99000,
       `description` = 'The smell of ale and bread should revitalize anybody unconscious.'
 WHERE `entry` = 990004;
