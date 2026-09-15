--
-- mod-sanctuary-faction - a faction's top rank may rename its own rungs
--
-- Brings factions made before the rename existed in line with the ones made after it.
--
-- `.faction create` now builds a four rung ladder and gives its Leader PERM_EDIT, which is
-- what puts the Ranks panel in the window. A faction created before that has a Leader
-- carrying 15 - invite, kick, promote, demote - and no way to reach the panel at all, so
-- the feature is deployed and invisible to exactly the people it was built for.
--
-- ONLY THE TOP RUNG, and only that one bit. PERM_EDIT is the mildest thing on the ladder:
-- it changes what a rank is CALLED and nothing about what it may do. What a rank grants
-- stays a game master's `.faction rank`, which is the split the whole design rests on - the
-- game master decides what a rank may do, the faction decides what to call it.
--
-- Idempotent. The bitwise OR leaves a rank that already holds it exactly as it was, so
-- re-running this changes nothing.
--

UPDATE `sanctuary_faction_rank` r
JOIN (
    SELECT `faction`, MAX(`rank_id`) AS top
    FROM `sanctuary_faction_rank`
    GROUP BY `faction`
) highest ON highest.`faction` = r.`faction` AND highest.`top` = r.`rank_id`
SET r.`permissions` = r.`permissions` | 16;
