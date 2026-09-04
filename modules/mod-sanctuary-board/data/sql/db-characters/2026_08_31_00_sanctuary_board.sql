--
-- Notice boards. The author's guid is stored so they can take their own bill down and so
-- a game master can trace abuse; it is never shown to another player. A bill is signed by
-- whatever the author wrote in it, which is what keeps the board compatible with the
-- realm's anonymity.
--
CREATE TABLE IF NOT EXISTS `character_board_posts` (
  `id`          INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `author_guid` INT UNSIGNED NOT NULL,
  `category`    TINYINT UNSIGNED NOT NULL DEFAULT 1,
  `body`        VARCHAR(255) NOT NULL,
  `posted_at`   INT UNSIGNED NOT NULL DEFAULT 0,
  `expires_at`  INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`),
  KEY `idx_author` (`author_guid`),
  KEY `idx_expiry` (`expires_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
