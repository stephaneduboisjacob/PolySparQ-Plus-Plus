-- Shadow-only observations for exact-window 5m cross-asset comparisons.
-- One row per market/window/sample slot across the host, so multiple PAPER
-- bot processes do not duplicate the same synchronized snapshot.
CREATE TABLE IF NOT EXISTS vp.cross_asset_sync_samples (
    sample_slot_ms                    BIGINT NOT NULL,
    observer_instance                 TEXT NOT NULL DEFAULT 'default',
    condition_id                      TEXT NOT NULL,
    asset                             TEXT NOT NULL CHECK (asset IN ('BTC','ETH','SOL','XRP')),
    horizon_seconds                   INTEGER NOT NULL CHECK (horizon_seconds = 300),
    window_open_ms                    BIGINT NOT NULL,
    window_close_ms                   BIGINT NOT NULL,
    eligible_assets                   INTEGER NOT NULL DEFAULT 0,
    quote_eligible                    BOOLEAN NOT NULL DEFAULT FALSE,
    candidate                         BOOLEAN NOT NULL DEFAULT FALSE,
    up_probability                    DOUBLE PRECISION,
    peer_median_up                    DOUBLE PRECISION,
    divergence_pct                    DOUBLE PRECISION,
    side                              TEXT CHECK (side IS NULL OR side IN ('UP','DOWN')),
    ask_price                         DOUBLE PRECISION,
    ask_depth_usd                     DOUBLE PRECISION,
    fee_rate                          DOUBLE PRECISION NOT NULL DEFAULT 0,
    fee_exponent                      INTEGER NOT NULL DEFAULT 1,
    estimated_fee_per_share           DOUBLE PRECISION NOT NULL DEFAULT 0,
    estimated_net_proxy_gap_per_share DOUBLE PRECISION NOT NULL DEFAULT 0,
    reason                            TEXT NOT NULL,
    payload                           JSONB NOT NULL DEFAULT '{}'::jsonb,
    PRIMARY KEY (condition_id, sample_slot_ms),
    CHECK (window_close_ms > window_open_ms),
    CHECK (up_probability IS NULL OR (up_probability >= 0 AND up_probability <= 1)),
    CHECK (peer_median_up IS NULL OR (peer_median_up >= 0 AND peer_median_up <= 1))
);

CREATE INDEX IF NOT EXISTS cross_asset_sync_slot_idx
    ON vp.cross_asset_sync_samples (sample_slot_ms DESC);
CREATE INDEX IF NOT EXISTS cross_asset_sync_close_candidate_idx
    ON vp.cross_asset_sync_samples (window_close_ms DESC, candidate)
    WHERE candidate;
CREATE INDEX IF NOT EXISTS cross_asset_sync_candidate_window_idx
    ON vp.cross_asset_sync_samples (condition_id, window_close_ms, sample_slot_ms)
    WHERE candidate;
CREATE INDEX IF NOT EXISTS cross_asset_sync_asset_slot_idx
    ON vp.cross_asset_sync_samples (asset, sample_slot_ms DESC);
