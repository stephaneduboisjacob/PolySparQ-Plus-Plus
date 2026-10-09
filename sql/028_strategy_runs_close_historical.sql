-- 028: Close historical vp.strategy_runs rows.
--
-- Rows written before session tracking (June 2026 era, session_id IS NULL)
-- never got an ended_at: their instances are retired and nothing will close
-- them. Their exact end times are unknown; stamp ended_at = started_at so
-- they are visibly synthetic and "open runs" queries stay meaningful. The
-- runtime closes its own instance's orphans at startup (Pg::
-- close_stale_strategy_runs); this is the one-time backlog cleanup.
BEGIN;
SET LOCAL lock_timeout='3s';
UPDATE vp.strategy_runs
   SET ended_at = started_at
 WHERE ended_at IS NULL
   AND session_id IS NULL;
COMMIT;
