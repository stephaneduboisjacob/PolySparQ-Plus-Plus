-- PolySparQ++ — research-grade data collection schema
-- Apply with:  psql vantagepulse -f sql/002_research_schema.sql
-- Idempotent.
-- Requires 001_schema.sql to have been applied first (depends on vp schema + types).

BEGIN;

SET search_path = vp, public;

-- ---------------------------------------------------------------
-- oracle_ticks — every Chainlink oracle price tick from RTDS feed
-- ---------------------------------------------------------------
CREATE TABLE IF NOT EXISTS oracle_ticks (
    id       BIGSERIAL PRIMARY KEY,
    ts       TIMESTAMPTZ NOT NULL,
    asset    TEXT NOT NULL,                          -- 'BTC' | 'ETH'
    price    DOUBLE PRECISION NOT NULL,
    source   TEXT NOT NULL DEFAULT 'rtds'
);
CREATE INDEX IF NOT EXISTS oracle_ticks_asset_ts_idx ON oracle_ticks (asset, ts DESC);

-- ---------------------------------------------------------------
-- binance_ticks — every Binance trade tick per symbol
-- ---------------------------------------------------------------
CREATE TABLE IF NOT EXISTS binance_ticks (
    id              BIGSERIAL PRIMARY KEY,
    ts              TIMESTAMPTZ NOT NULL,
    symbol          TEXT NOT NULL,                   -- e.g. 'BTCUSDT' | 'ETHUSDT' | 'SOLUSDT' | 'XRPUSDT'
    price           DOUBLE PRECISION NOT NULL,
    qty             DOUBLE PRECISION NOT NULL,
    buyer_is_maker  BOOLEAN NOT NULL,
    aggressor_side  order_side                       -- 'buy' if buyer_is_maker=false else 'sell'
);
CREATE INDEX IF NOT EXISTS binance_ticks_symbol_ts_idx ON binance_ticks (symbol, ts DESC);

-- ---------------------------------------------------------------
-- window_events — lifecycle events for every UP/DOWN window
-- ---------------------------------------------------------------
CREATE TABLE IF NOT EXISTS window_events (
    id            BIGSERIAL PRIMARY KEY,
    ts            TIMESTAMPTZ NOT NULL DEFAULT now(),
    condition_id  TEXT NOT NULL,
    event_type    TEXT NOT NULL,                     -- 'opened' | 'closed' | 'settled' | 'first_oracle_seed' | 'first_book_tick'
    payload       JSONB
);
CREATE INDEX IF NOT EXISTS window_events_cid_ts_idx ON window_events (condition_id, ts DESC);
CREATE INDEX IF NOT EXISTS window_events_type_idx ON window_events (event_type, ts DESC);

-- ---------------------------------------------------------------
-- coinbase_ticks — every Coinbase spot price poll
-- ---------------------------------------------------------------
CREATE TABLE IF NOT EXISTS coinbase_ticks (
    id       BIGSERIAL PRIMARY KEY,
    ts       TIMESTAMPTZ NOT NULL,
    asset    TEXT NOT NULL,                          -- 'BTC' | 'ETH'
    price    DOUBLE PRECISION NOT NULL
);
CREATE INDEX IF NOT EXISTS coinbase_ticks_asset_ts_idx ON coinbase_ticks (asset, ts DESC);

COMMIT;
