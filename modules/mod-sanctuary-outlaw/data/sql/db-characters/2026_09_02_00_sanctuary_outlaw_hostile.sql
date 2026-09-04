--
-- mod-sanctuary-outlaw - outlawry gains a second state
--
-- WHY: being open to violence and being hunted by the town watch were the same thing, and
-- they should not be. An outlaw who has robbed nobody yet was wearing faction 14, Monster,
-- which every city guard's hostileMask covers - so declaring yourself available got you
-- chased out of Stormwind before you had done anything.
--
-- So there are two states now:
--
--   wanted    neutral to Alliance and Horde alike. Attackable by anyone, hunted by nobody.
--   hostile   what striking somebody earns. Faction 14, and the whole town turns out.
--
-- Hostility has to be durable or logging out would launder it, which is why it is stored
-- rather than kept in memory with the rest of the hot state.
--
--   `hostile`        1 once they have drawn blood
--   `hostile_until`  when it lapses; 0 means it lasts as long as the outlawry does
--
-- MySQL 8 has no ADD COLUMN IF NOT EXISTS - that is MariaDB - so this is a plain ALTER and
-- relies on the updater running each file exactly once.
--

ALTER TABLE `sanctuary_outlaw`
  ADD COLUMN `hostile` TINYINT UNSIGNED NOT NULL DEFAULT 0
      COMMENT '1 once they have struck someone, so the guards want them',
  ADD COLUMN `hostile_until` INT UNSIGNED NOT NULL DEFAULT 0
      COMMENT 'unix time hostility lapses, 0 = for as long as they remain an outlaw';
