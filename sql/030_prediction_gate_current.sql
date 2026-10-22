-- Latest current-window forecast exposed by the root Predictions page.
-- This is a live snapshot, separate from the sparse historical checkpoints.
CREATE TABLE IF NOT EXISTS vp.prediction_gate_current (
    condition_id       TEXT PRIMARY KEY,
    asset              TEXT NOT NULL CHECK (asset IN ('BTC','ETH','SOL','XRP')),
    horizon_seconds    INTEGER NOT NULL CHECK (horizon_seconds IN (300,900)),
    slug               TEXT NOT NULL DEFAULT '',
    window_open        TIMESTAMPTZ NOT NULL,
    window_close       TIMESTAMPTZ NOT NULL,
    model_status       TEXT NOT NULL,
    up_probability     DOUBLE PRECISION CHECK (
        up_probability IS NULL OR (up_probability >= 0 AND up_probability <= 1)),
    model_version      TEXT NOT NULL DEFAULT '',
    model_basis        TEXT NOT NULL DEFAULT '',
    source_updated_at  TIMESTAMPTZ NOT NULL,
    published_at       TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    CHECK (window_close > window_open)
);

CREATE INDEX IF NOT EXISTS prediction_gate_current_fresh_idx
    ON vp.prediction_gate_current(source_updated_at DESC, asset, horizon_seconds);
