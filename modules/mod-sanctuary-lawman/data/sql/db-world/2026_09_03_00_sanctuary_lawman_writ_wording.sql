--
-- mod-sanctuary-lawman - what the writs say
--
-- Both were written from the watch's point of view, describing the office rather than the
-- act. The wording that matters is the wording the person holding it reads out, so both
-- become first person and address the accused directly.
--

UPDATE `item_template`
   SET `description` = 'By this decree, my hand is raised against you.'
 WHERE `entry` = 990000;

UPDATE `item_template`
   SET `description` = 'By this decree, you are pardoned.'
 WHERE `entry` = 990003;
