-- PolySparQ++ — canonical schema
-- Apply with:  psql vantagepulse -f sql/001_schema.sql
-- Idempotent.

BEGIN;

-- ---------------------------------------------------------------
-- Schemas
-- ---------------------------------------------------------------
CREATE SCHEMA IF NOT EXISTS vp;
SET search_path = vp, public;

-- ---------------------------------------------------------------
-- Reference: trading mode (LIVE / PAPER / REPLAY)
-- ---------------------------------------------------------------
DO $$ BEGIN
    CREATE TYPE trade_mode AS ENUM ('live', 'paper', 'replay');
EXCEPTION WHEN duplicate_object THEN NULL; END $$;

DO $$ BEGIN
    CREATE TYPE order_side AS ENUM ('buy', 'sell');
EXCEPTION WHEN duplicate_object THEN NULL; END $$;

DO $$ BEGIN
    CREATE TYPE share_side AS ENUM ('UP', 'DOWN');
EXCEPTION WHEN duplicate_object THEN NULL; END $$;

DO $$ BEGIN
    CREATE TYPE order_status AS ENUM (
        'pending_sign', 'submitted', 'acked', 'partial',
        'filled', 'cancelled', 'rejected', 'uncertain'
    );
EXCEPTION WHEN duplicate_object THEN NULL; END $$;

-- ---------------------------------------------------------------
-- candles_1m  — 1-minute Binance bars for traded symbols, partitioned by year
-- ---------------------------------------------------------------
CREATE TABLE IF NOT EXISTS candles_1m (
    symbol      TEXT        NOT NULL,
    ts          TIMESTAMPTZ NOT NULL,
    open        DOUBLE PRECISION NOT NULL,
    high        DOUBLE PRECISION NOT NULL,
    low         DOUBLE PRECISION NOT NULL,
    close       DOUBLE PRECISION NOT NULL,
    volume      DOUBLE PRECISION NOT NULL,
    PRIMARY KEY (symbol, ts)
) PARTITION BY RANGE (ts);

-- Yearly partitions 2021..2027 (covers the archive + headroom)
DO $$
DECLARE y INT;
BEGIN
    FOR y IN 2021..2027 LOOP
        EXECUTE format($f$
            CREATE TABLE IF NOT EXISTS candles_1m_%s
            PARTITION OF candles_1m
            FOR VALUES FROM (%L) TO (%L);
        $f$, y, y || '-01-01 00:00:00+00', (y+1) || '-01-01 00:00:00+00');
    END LOOP;
END $$;

CREATE INDEX IF NOT EXISTS candles_1m_symbol_ts_idx
    ON candles_1m (symbol, ts DESC);

-- ---------------------------------------------------------------
-- pm_markets  — Polymarket UP/DOWN markets we have ever seen
-- ---------------------------------------------------------------
CREATE TABLE IF NOT EXISTS pm_markets (
    condition_id      TEXT PRIMARY KEY,
    asset             TEXT NOT NULL,                -- 'BTC' | 'ETH'
    horizon_seconds   INT  NOT NULL,                -- 300 / 900 / 3600
    target_price      DOUBLE PRECISION,             -- the strike
    window_open       TIMESTAMPTZ NOT NULL,
    window_close      TIMESTAMPTZ NOT NULL,
    yes_token_id      TEXT NOT NULL,                -- ERC1155 token id (UP)
    no_token_id       TEXT NOT NULL,                -- (DOWN)
    question          TEXT,
    slug              TEXT,
    raw               JSONB NOT NULL DEFAULT '{}'::jsonb,
    resolved_outcome  share_side,                   -- NULL until resolved
    first_seen        TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_seen         TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS pm_markets_window_close_idx ON pm_markets (window_close DESC);
CREATE INDEX IF NOT EXISTS pm_markets_asset_horizon_idx ON pm_markets (asset, horizon_seconds, window_close DESC);

-- ---------------------------------------------------------------
-- pm_book_snapshots — top-N levels per side, per token
-- Stored periodically (1 Hz) and on big book moves.
-- ---------------------------------------------------------------
CREATE TABLE IF NOT EXISTS pm_book_snapshots (
    id            BIGSERIAL PRIMARY KEY,
    ts            TIMESTAMPTZ NOT NULL,
    condition_id  TEXT NOT NULL,
    token_id      TEXT NOT NULL,                  -- which side (UP or DOWN)
    side_label    share_side NOT NULL,
    bids          JSONB NOT NULL,                 -- [[price, size], ...] top 20
    asks          JSONB NOT NULL,
    best_bid      DOUBLE PRECISION,
    best_ask      DOUBLE PRECISION,
    midprice      DOUBLE PRECISION,
    microprice    DOUBLE PRECISION,
    spread        DOUBLE PRECISION,
    imbalance_top1 DOUBLE PRECISION,
    imbalance_top5 DOUBLE PRECISION
);
CREATE INDEX IF NOT EXISTS pm_book_ts_idx ON pm_book_snapshots (condition_id, ts DESC);

-- ---------------------------------------------------------------
-- pm_trades — every public trade observed on tracked markets
-- ---------------------------------------------------------------
CREATE TABLE IF NOT EXISTS pm_trades (
    id             BIGSERIAL PRIMARY KEY,
    ts             TIMESTAMPTZ NOT NULL,
    condition_id   TEXT NOT NULL,
    token_id       TEXT NOT NULL,
    side_label     share_side NOT NULL,
    price          DOUBLE PRECISION NOT NULL,
    size           DOUBLE PRECISION NOT NULL,
    aggressor      order_side,                    -- inferred buy/sell
    raw            JSONB
);
CREATE INDEX IF NOT EXISTS pm_trades_ts_idx ON pm_trades (condition_id, ts DESC);

-- ---------------------------------------------------------------
-- features — per-tick FeatureSnapshot (versioned schema)
-- ---------------------------------------------------------------
CREATE TABLE IF NOT EXISTS features (
    id            BIGSERIAL PRIMARY KEY,
    ts            TIMESTAMPTZ NOT NULL,
    condition_id  TEXT NOT NULL,
    schema_ver    INT  NOT NULL,
    payload       JSONB NOT NULL
);
CREATE INDEX IF NOT EXISTS features_ts_idx ON features (condition_id, ts DESC);

-- ---------------------------------------------------------------
-- signals — every signal emitted by the rule/model layers
-- ---------------------------------------------------------------
CREATE TABLE IF NOT EXISTS signals (
    id              BIGSERIAL PRIMARY KEY,
    ts              TIMESTAMPTZ NOT NULL,
    condition_id    TEXT NOT NULL,
    side_label      share_side NOT NULL,           -- UP or DOWN we'd buy
    direction       order_side NOT NULL,           -- buy / sell
    confidence      REAL NOT NULL,                 -- 0..1
    recommended     TEXT NOT NULL,                 -- BUY / SELL / HOLD
    rec_size_usd    DOUBLE PRECISION,
    rec_limit_price DOUBLE PRECISION,
    expires_at      TIMESTAMPTZ,
    reason          TEXT,
    model_version   TEXT,
    regime_label    TEXT,
    payload         JSONB
);
CREATE INDEX IF NOT EXISTS signals_ts_idx ON signals (condition_id, ts DESC);

-- ---------------------------------------------------------------
-- orders + fills — full lifecycle
-- ---------------------------------------------------------------
CREATE TABLE IF NOT EXISTS orders (
    client_order_id  UUID PRIMARY KEY,
    instance         TEXT,
    exchange_order_id TEXT,
    execution_mode   TEXT NOT NULL DEFAULT 'taker',
    post_only         BOOLEAN NOT NULL DEFAULT false,
    ts_created       TIMESTAMPTZ NOT NULL DEFAULT now(),
    ts_submitted     TIMESTAMPTZ,
    ts_terminal      TIMESTAMPTZ,
    mode             trade_mode NOT NULL DEFAULT 'paper',
    condition_id     TEXT NOT NULL,
    token_id         TEXT NOT NULL,
    side_label       share_side NOT NULL,
    direction        order_side NOT NULL,
    price            DOUBLE PRECISION NOT NULL,
    size_shares      DOUBLE PRECISION NOT NULL,
    notional_usd     DOUBLE PRECISION NOT NULL,
    status           order_status NOT NULL DEFAULT 'pending_sign',
    source           TEXT NOT NULL,                 -- HUMAN_KEYPRESS / RULE / GHOST
    reject_reason    TEXT,
    raw_request      JSONB,
    raw_response     JSONB
);
CREATE INDEX IF NOT EXISTS orders_status_ts_idx ON orders (status, ts_created DESC);
CREATE INDEX IF NOT EXISTS orders_market_ts_idx ON orders (condition_id, ts_created DESC);
CREATE INDEX IF NOT EXISTS orders_instance_ts_idx ON orders (instance, ts_created DESC);

CREATE TABLE IF NOT EXISTS fills (
    id               BIGSERIAL PRIMARY KEY,
    ts               TIMESTAMPTZ NOT NULL,
    client_order_id  UUID NOT NULL REFERENCES orders(client_order_id),
    price            DOUBLE PRECISION NOT NULL,
    size_shares      DOUBLE PRECISION NOT NULL,
    fee_usd          DOUBLE PRECISION NOT NULL DEFAULT 0,
    raw              JSONB
);
CREATE INDEX IF NOT EXISTS fills_order_idx ON fills (client_order_id);

-- ---------------------------------------------------------------
-- positions_history — periodic snapshot for the equity curve
-- ---------------------------------------------------------------
CREATE TABLE IF NOT EXISTS positions_history (
    id              BIGSERIAL PRIMARY KEY,
    ts              TIMESTAMPTZ NOT NULL,
    mode            trade_mode NOT NULL,
    cash_usd        DOUBLE PRECISION NOT NULL,
    equity_usd      DOUBLE PRECISION NOT NULL,
    realized_pnl    DOUBLE PRECISION NOT NULL,
    unrealized_pnl  DOUBLE PRECISION NOT NULL,
    deployed_usd    DOUBLE PRECISION NOT NULL,
    open_positions  JSONB NOT NULL                  -- per-market detail
);
CREATE INDEX IF NOT EXISTS positions_history_ts_idx ON positions_history (ts DESC);
CREATE INDEX IF NOT EXISTS positions_history_mode_ts_idx ON positions_history (mode, ts DESC);

-- ---------------------------------------------------------------
-- llm_exchanges — every prompt/response pair, with redaction filter version
-- ---------------------------------------------------------------
CREATE TABLE IF NOT EXISTS llm_exchanges (
    id                  BIGSERIAL PRIMARY KEY,
    ts                  TIMESTAMPTZ NOT NULL DEFAULT now(),
    trigger             TEXT NOT NULL,               -- manual | consult | review | strategy_diff | sql
    model               TEXT NOT NULL,
    redaction_ver       INT  NOT NULL,
    context_hash        TEXT NOT NULL,
    prompt              TEXT NOT NULL,
    response            TEXT,
    latency_ms          INT,
    error               TEXT
);
CREATE INDEX IF NOT EXISTS llm_exchanges_ts_idx ON llm_exchanges (ts DESC);

-- ---------------------------------------------------------------
-- strategy_versions — every change to the live strategy
-- ---------------------------------------------------------------
CREATE TABLE IF NOT EXISTS strategy_versions (
    id            BIGSERIAL PRIMARY KEY,
    ts            TIMESTAMPTZ NOT NULL DEFAULT now(),
    actor         TEXT NOT NULL,                    -- HUMAN/TUI | HUMAN/FILE | LLM_PROPOSAL
    diff_summary  TEXT NOT NULL,
    before_doc    JSONB NOT NULL,
    after_doc     JSONB NOT NULL
);
CREATE INDEX IF NOT EXISTS strategy_versions_ts_idx ON strategy_versions (ts DESC);

COMMIT;

-- helpful views
CREATE OR REPLACE VIEW vp.candles_1m_summary AS
SELECT symbol,
       MIN(ts) AS first_ts,
       MAX(ts) AS last_ts,
       COUNT(*) AS rows
FROM   vp.candles_1m
GROUP BY symbol
ORDER BY symbol;
