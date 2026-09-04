-- mod-proximity-voice: per-character voice settings
--
-- Kept out of `characters` so the module can be removed without touching core
-- tables. `speak_range` avoids the reserved word RANGE.

CREATE TABLE IF NOT EXISTS `character_voice_settings` (
  `guid`           INT UNSIGNED     NOT NULL COMMENT 'character guid (low part)',
  `speak_range`    FLOAT            NOT NULL DEFAULT 25    COMMENT 'speaking distance in yards',
  `voice_language` INT UNSIGNED     NOT NULL DEFAULT 0     COMMENT 'Language enum the voice is carried in',
  `muted`          TINYINT UNSIGNED NOT NULL DEFAULT 0     COMMENT 'microphone muted',
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;
