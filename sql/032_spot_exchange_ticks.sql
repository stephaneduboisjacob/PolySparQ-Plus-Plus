-- Read-only spot venue BBO observations for the Predictions reference chart.
-- Source timestamps and local receipt timestamps remain distinct so later
-- lead/lag analysis can account for publication and transport delay.
BEGIN;

CREATE TABLE IF NOT EXISTS vp.spot_exchange_ticks (
    id              BIGSERIAL PRIMARY KEY,
    venue           TEXT NOT NULL CHECK (venue IN ('kraken','gemini','okx')),
    asset           TEXT NOT NULL CHECK (asset IN ('BTC','ETH','SOL','XRP')),
    pair            TEXT NOT NULL,
    ts              TIMESTAMPTZ NOT NULL,
    received_at     TIMESTAMPTZ NOT NULL,
    exchange_ts_ms  BIGINT NOT NULL,
    price           DOUBLE PRECISION NOT NULL CHECK (price > 0),
    bid_price       DOUBLE PRECISION NOT NULL CHECK (bid_price > 0),
    bid_size        DOUBLE PRECISION NOT NULL DEFAULT 0 CHECK (bid_size >= 0),
    ask_price       DOUBLE PRECISION NOT NULL CHECK (ask_price >= bid_price),
    ask_size        DOUBLE PRECISION NOT NULL DEFAULT 0 CHECK (ask_size >= 0),
    UNIQUE (venue, asset, exchange_ts_ms, bid_price, ask_price)
);

CREATE INDEX IF NOT EXISTS spot_exchange_ticks_venue_asset_ts_idx
    ON vp.spot_exchange_ticks (venue, asset, ts DESC);

COMMIT;
