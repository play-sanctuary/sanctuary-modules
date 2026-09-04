--
-- Removes the notice board comparison props created by compare_models.sql.
--
-- Run once a model has been chosen and baked into
-- data/sql/db-world/2026_08_31_01_sanctuary_board_model.sql:
--
--   "C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe" -uroot -p acore_world < compare_models_cleanup.sql
--
-- Then .reload gameobject_template and .reload gameobject, or restart worldserver.
--
-- Only touches 990101-990110, a range reserved for scaffolding precisely so a script like
-- this cannot reach anything permanent. It previously used 990001-990010, which silently
-- destroyed mod-sanctuary-pvploot's spoils sack at 990001 and left PvP kills dropping an
-- unlootable prop. Check the entry-id table in Launcher/README.md before picking new ids.
--
-- Left alone: 990000 (the real board) and 990001 (the spoils sack).
--

DELETE FROM `gameobject`          WHERE `id`    BETWEEN 990101 AND 990110;
DELETE FROM `gameobject_template` WHERE `entry` BETWEEN 990101 AND 990110;

SELECT CONCAT(
    'Remaining comparison props: ',
    (SELECT COUNT(*) FROM `gameobject_template` WHERE `entry` BETWEEN 990101 AND 990110),
    ' templates, ',
    (SELECT COUNT(*) FROM `gameobject` WHERE `id` BETWEEN 990101 AND 990110),
    ' spawns. The real board (990000) has ',
    (SELECT COUNT(*) FROM `gameobject` WHERE `id` = 990000),
    ' spawns.'
) AS result;
