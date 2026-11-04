-- Persistent, per-window forecast checkpoints for the cockpit Predictions tab.
-- Deliberately has no retention policy: prediction history is append-only.
BEGIN;

CREATE TABLE IF NOT EXISTS vp.prediction_history_windows (
    window_key             TEXT PRIMARY KEY,
    condition_id           TEXT,
    asset                  TEXT NOT NULL CHECK (asset IN ('BTC','ETH','SOL','XRP')),
    horizon_seconds        INTEGER NOT NULL CHECK (horizon_seconds IN (300,900)),
    slug                   TEXT NOT NULL,
    window_open            TIMESTAMPTZ NOT NULL,
    window_close           TIMESTAMPTZ NOT NULL,
    settlement_source      TEXT,
    settlement_window_s    INTEGER NOT NULL DEFAULT 0,
    model_version          TEXT,
    model_basis            TEXT,
    open_price             DOUBLE PRECISION,
    open_price_ts          TIMESTAMPTZ,
    close_price            DOUBLE PRECISION,
    close_price_ts         TIMESTAMPTZ,
    outcome                TEXT CHECK (outcome IN ('UP','DOWN')),
    outcome_source         TEXT,
    resolved_at            TIMESTAMPTZ,
    resolution_checked_at  TIMESTAMPTZ,
    created_at             TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at             TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE UNIQUE INDEX IF NOT EXISTS prediction_history_window_series_idx
    ON vp.prediction_history_windows(asset,horizon_seconds,window_open);
CREATE INDEX IF NOT EXISTS prediction_history_window_page_idx
    ON vp.prediction_history_windows(asset,horizon_seconds,window_open DESC);
CREATE INDEX IF NOT EXISTS prediction_history_resolution_idx
    ON vp.prediction_history_windows(window_close)
    WHERE outcome IS NULL OR close_price IS NULL;

CREATE TABLE IF NOT EXISTS vp.prediction_history_checkpoints (
    window_key          TEXT NOT NULL REFERENCES vp.prediction_history_windows(window_key) ON DELETE CASCADE,
    checkpoint_key      TEXT NOT NULL CHECK (checkpoint_key IN (
                            'pre_start_60s','open_0pct','elapsed_20pct',
                            'elapsed_40pct','elapsed_60pct','elapsed_80pct')),
    target_at           TIMESTAMPTZ NOT NULL,
    captured_at         TIMESTAMPTZ,
    observed_elapsed_pct DOUBLE PRECISION,
    capture_offset_s    DOUBLE PRECISION,
    capture_status      TEXT NOT NULL DEFAULT 'pending' CHECK (capture_status IN (
                            'pending','captured','unavailable','not_recorded')),
    up_probability      DOUBLE PRECISION CHECK (up_probability IS NULL OR up_probability BETWEEN 0 AND 1),
    side                TEXT CHECK (side IN ('UP','DOWN','EVEN')),
    side_probability    DOUBLE PRECISION CHECK (side_probability IS NULL OR side_probability BETWEEN 0 AND 1),
    model_version       TEXT,
    model_basis         TEXT,
    model_status        TEXT,
    spot_price          DOUBLE PRECISION,
    strike_price        DOUBLE PRECISION,
    market_up_probability DOUBLE PRECISION CHECK (market_up_probability IS NULL OR market_up_probability BETWEEN 0 AND 1),
    fee_adjusted_edge   DOUBLE PRECISION,
    details             JSONB NOT NULL DEFAULT '{}'::jsonb,
    PRIMARY KEY (window_key,checkpoint_key)
);

CREATE INDEX IF NOT EXISTS prediction_history_checkpoint_target_idx
    ON vp.prediction_history_checkpoints(target_at DESC);

CREATE TABLE IF NOT EXISTS vp.prediction_history_imports (
    migration_key  TEXT PRIMARY KEY,
    applied_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    windows_imported INTEGER NOT NULL DEFAULT 0,
    checkpoints_imported INTEGER NOT NULL DEFAULT 0
);

COMMIT;
