BEGIN;
SET LOCAL lock_timeout='3s';
ALTER TABLE vp.positions_history ADD COLUMN IF NOT EXISTS instance text;
ALTER TABLE vp.positions_history ADD COLUMN IF NOT EXISTS session_id uuid;
CREATE INDEX IF NOT EXISTS positions_history_instance_ts_idx
    ON vp.positions_history(instance,ts DESC);
COMMIT;
