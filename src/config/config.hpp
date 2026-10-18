#pragma once
#include <string>
#include <vector>
#include "core/types.hpp"

namespace vp {

struct PgConfig {
    std::string conninfo;
    std::string read_replica;
};
struct PolymarketConfig {
    std::string clob_rest_url;
    std::string gamma_rest_url;
    std::string clob_ws_url;
    std::string clob_user_ws_url;
    std::string rtds_ws_url;       // RTDS oracle feed (no auth needed)
    std::string coinbase_ws_url;   // Coinbase Exchange ticker WS (lead signal)
    int         poll_seconds = 60;
    std::vector<int> universe_horizons_seconds = {300, 900};
    int         universe_max_windows_per_series = 10;
    int         universe_max_markets = 80;
    int         universe_prewarm_ahead_minutes = 20;
    int         market_prune_interval_ms = 30000;
    int         market_prune_grace_ms = 180000;
    int         closed_market_display_grace_ms = 900000;
    int         book_health_log_interval_ms = 30000;
    int         book_health_stale_ms = 120000;
    int         rest_book_seed_interval_ms = 10000;
    int         rest_book_seed_log_interval_ms = 60000;
    int         rest_book_seed_min_age_ms = 1000;
    std::string proxy_address;
    std::string signer_address;
    bool        rtds_enabled = true;
    // ── Per-horizon block knobs (Phase 4.4, Polymarket-new port) ──
    // Instant hot-reloadable horizon disable: e.g. block 5m during a vol
    // event without a restart. Applied at universe selection (next
    // discovery refresh, ≤ poll_seconds).
    std::vector<int> blocked_horizons_seconds;
    // Convenience flags for common horizons (populate blocked_horizons_seconds on load).
    bool block_5m  = false;
    bool block_15m = false;
};
struct BinanceConfig {
    std::string              ws_url;
    std::vector<std::string> streams;
    // Binance USDT-M futures WS (forceOrder liquidation stream, Phase 4.1).
    // Example: wss://fstream.binance.com/stream?streams=btcusdt@forceOrder
    // Empty = disabled.
    std::string              futures_ws_url;
};
struct PolygonConfig {
    std::string rpc_url;
};
struct LlmConfig {
    std::string url;
    std::string model;
    int         timeout_seconds = 50;
    int         parallel_max    = 10;
    std::string api_key_file;
    std::string api_key;          // populated at runtime from api_key_file
    bool        enabled = true;
};
struct StrategyHot {
    double max_entry_share_price   = 0.35;
    double ideal_entry_share_price = 0.20;
    double hard_share_price_ceil   = 0.95;
    double max_gap_btc_usd         = 50.0;
    double max_gap_eth_usd         = 4.0;
    int    min_secs_remaining_5m   = 60;
    int    min_secs_remaining_15m  = 300;  // research: 5min minimum for 15m windows
    int    min_secs_remaining_60m  = 600;
    // Max secs remaining for mid-window gate (entries too early are noise)
    int    max_secs_remaining_5m   = 9999;
    int    max_secs_remaining_15m  = 9999;
    int    max_secs_remaining_60m  = 9999;
    double per_click_size_usd      = 5.00;   // $5 per trade — meets CLOB 5-share minimum
    double min_order_usd           = 1.00;   // Polymarket minimum per bet
    double max_per_window_usd      = 5.00;   // $5 max per window
    // Kelly fraction for bankroll-aware position sizing (0.25 = quarter-Kelly for safety)
    double kelly_fraction          = 0.25;  // fraction of edge to risk
    // Size floor/cap: minimum and maximum absolute dollar size when bankroll scaling
    double min_size_usd            = 5.00;   // never smaller than this
    double max_size_usd            = 5.00;   // never larger than this
    bool   equity_sizing_enabled   = false;  // when true: order size = equity * equity_order_pct
    double equity_order_pct        = 0.08;   // 8% of effective equity per entry
    double equity_max_deployed_pct = 0.80;   // runtime deployed cap as % of effective equity
    double autopilot_conf_floor    = 0.50;  // higher bar = filter weak signals
    int    autopilot_throttle_ms   = 800;
    int    autopilot_max_per_sweep = 6;     // max qualifying entries submitted per sweep
    int    max_entries_per_window  = 3;     // non-scalp max entries in one market window
    int    max_same_side_positions = 2;     // correlation cap: open UP or DOWN positions at once
    double trend_block_pct         = 0.10;  // block entries against this 3m oracle move (%)
    double trend_lookback_secs     = 180.0;
    double entry_cross_ticks       = 2.0;   // marketable buy crosses best ask by N ticks
    // PAPER-only repeated maker cycles; independent of directional rule entries.
    std::string recycler_mode = "off"; // off | sniper | range
    // Opt-in for the higher-turnover, multi-market Maker Sniper experiment.
    // False preserves existing profile behavior across rebuilds/restarts.
    bool recycler_maniac_mode = false;
    double recycler_entry_price = 0.40;
    double recycler_target_price = 0.55;
    double recycler_entry_min_price = 0.25;
    double recycler_entry_max_price = 0.65;
    double recycler_target_profit_pct = 20.0;
    int recycler_momentum_lookback_secs = 10;
    int recycler_min_momentum_ticks = 5;
    int recycler_stop_ticks = 0; // legacy key; price stops are not used
    int recycler_max_hold_secs = 0; // legacy key; 0 means no timed liquidation
    int recycler_entry_cutoff_secs = 300;
    int recycler_max_cycles = 15;
    int recycler_lookback_secs = 120;
    double recycler_min_range = 0.08;
    double recycler_max_window_loss = 0.0;   // disabled by default for paper research
    double recycler_max_session_loss = 0.0;  // disabled by default for paper research
    bool recycler_allow_taker_exit = false; // legacy key; late-window exit uses recycler_late_taker_exit_enabled
    // Settlement-aware Recycler controls. The BTC reference and strike are
    // required; missing/stale inputs fail closed rather than guessing.
    double recycler_min_settlement_edge_pct = 10.0;
    int recycler_reference_max_age_ms = 3000;
    // Late-window controls are PAPER-only through recycler_mode validation.
    bool recycler_late_risk_exit_enabled = true;
    double recycler_late_risk_min_prob = 0.35;
    double recycler_late_risk_bid_loss_pct = 30.0;
    double recycler_late_target_6m_pct = 15.0;
    double recycler_late_target_4m_pct = 10.0;
    double recycler_late_target_2m_pct = 5.0;
    int recycler_late_maker_secs = 120;
    int recycler_late_taker_secs = 60;
    bool recycler_late_taker_exit_enabled = true;
    // Experimental PAPER-only high-price BTC 15m settlement entry. This posts
    // one fixed passive bid in the final seconds and holds fills to settlement.
    bool recycler_settlement_entry_enabled = false;
    int recycler_settlement_entry_secs = 15;
    double recycler_settlement_entry_min_price = 0.90;
    double recycler_settlement_entry_max_price = 0.98;
    // Dedicated settlement-only profile: skip ordinary Recycler cycle entries
    // and require the conservative independent forecast edge before a late bid.
    bool recycler_settlement_only = false;
    // Entry-shaping and late-exit refinements (2026-09-28 study). Defaults
    // preserve the previous behavior exactly.
    int recycler_entry_open_grace_secs = 0;       // skip entries this long after window open
    double recycler_first_entry_scale_pct = 100.0; // click scale for a window's first cycle
    double recycler_late_taker_min_bid = 0.0;     // legacy threshold: below it, ride to settlement instead of posting a late maker exit
    std::string entry_order_mode   = "maker"; // fixed post-only LIMIT-MAKER routing
    int    maker_improve_ticks     = 0;
    int    maker_reprice_ms        = 750;
    int    maker_poll_ms           = 500;
    int    maker_max_age_ms        = 5000;
    int    maker_cancel_before_close_ms = 3000;
    double manual_buy_slippage_pct = 0.10;
    double manual_buy_slippage_extra = 0.01;
    int    order_cooldown_ms       = 2000;
    int    token_quarantine_payload_ms = 90000;
    int    token_quarantine_no_match_ms = 20000;
    double min_position_shares     = 1.0;
    double dust_position_shares    = 0.01;
    int    price_history_max_points = 60;
    int    price_velocity_lookback_ms = 10000;
    int    sparkline_points = 30;
    int64_t tick_buffer_max_age_us = 30000000;
    int    tick_buffer_max_entries = 5000;
    int    consecutive_loss_pause  = 10;
    int    consecutive_pause_secs  = 600;
    // Oracle-lag strategy (primary alpha source)
    bool   oracle_lag_mode          = true;   // use oracle delta as primary signal
    double min_oracle_delta_pct     = 0.15;   // 0.15% — real signal, not noise
    // ── Coinbase lead (2026-06-10) ──────────────────────────────────────
    // The Polymarket oracle follows Coinbase by ~1.5-3s (Chainlink aggregates
    // USD venues, Coinbase dominant). coinbase_lead_mode lets oracle-lag fire
    // on the COINBASE move before the oracle prints it (oracle must not
    // contradict); coinbase_veto_mode blocks entries when Coinbase shows the
    // oracle's move has already reversed (oracle blip filter).
    bool   coinbase_lead_mode       = true;
    bool   coinbase_veto_mode       = true;
    double coinbase_lead_min_delta_pct = 0.0; // 0 => reuse min_oracle_delta_pct
    // ── Binance bookTicker confirmation (Phase 6.3) ──────────────────────
    // Book-level confirmation for oracle-lag entries: the sub-100ms Binance
    // bookTicker mid must agree with the oracle/CB direction over the last
    // 2s, OR top-of-book size imbalance must lean toward our side by at
    // least binance_book_imb_min (|imb| in [0,1]; 0 = delta-agreement only).
    // Guards against oracle blips the real market already rejected.
    bool   binance_book_confirm     = true;
    double binance_book_imb_min     = 0.0;
    // ── Edge-persistence anti-flicker gate (Phase 5.6) ───────────────────
    // Require the SAME signal to persist for entry_persist_ms before firing
    // (kills 1-tick blip entries), and scale the entry edge floor by window
    // phase: early (>50% left) ×early mult, mid (>25%) ×mid, late ×late.
    // The multiplier applies to the signal's own edge metric (oracle delta
    // for ORACLE signals, predictor edge for PREDICTOR signals).
    bool   entry_persist_gate       = true;
    int    entry_persist_ms         = 250;
    double entry_early_edge_mult    = 1.6;
    double entry_mid_edge_mult      = 1.0;
    double entry_late_edge_mult     = 0.6;
    // ── Digital fair value N(d₂) (Phase 5.1, paper-test first) ──────────
    // Principled replacement for the refuted predictor ensemble: per-second
    // realized oracle vol → zero-drift GBM digital fair value → buy EITHER
    // side when fair − cost > min_edge + fee buffer. Entry window 15–85% of
    // the cycle; DOWN via complement parity with a slippage buffer; jump
    // regimes (|z| > threshold) veto entries; vol clamped to sanity rails.
    bool   digital_fv_mode          = false;
    // Price the digital from Coinbase's move since the window opened, mapped
    // onto the Chainlink opening strike. This removes the fixed cross-venue
    // basis while retaining Coinbase's lead over the settlement oracle.
    bool   digital_fv_use_coinbase  = false;
    double digital_fv_min_edge_pct  = 4.5;
    double digital_fv_fee_buffer    = 1.5;
    double digital_fv_down_slip_pct = 1.0;
    double digital_fv_vol_floor     = 0.00002;  // per-second σ sanity floor
    double digital_fv_vol_ceil      = 0.003;    // per-second σ sanity ceil
    double digital_fv_jump_z        = 3.0;
    // ── Multi-source directional vote (Phase 4.1) ────────────────────────
    // Independent confirmers for oracle-lag direction: Coinbase delta,
    // Binance bookTicker mid delta, and futures liquidation pressure.
    // Require >= quorum of the available voters to agree; fewer vetoes.
    bool   multi_source_vote        = false;   // paper-test first
    int    multi_source_quorum      = 2;

    // ── Midcycle Continuation (Homerun crypto_5m_midcycle) ──────────────
    // At exactly 150s into a 300s cycle: Chainlink distance ≥15bps from
    // strike + winning-side share ask ≤ threshold → continuation bet.
    // Only on 5m (300s) windows. Direction matches the oracle move.
    bool   midcycle_mode            = false;
    int    midcycle_trigger_secs    = 150;    // seconds left to trigger (for 300s window)
    double midcycle_min_distance_bps= 15.0;   // min Chainlink distance from strike in bps
    double midcycle_max_share_price = 0.70;   // max winning-side ask to enter
    int    midcycle_window_s        = 300;    // only on this window size (300s = 5m)

    // ── Polymarket book imbalance filter (Phase 4.2) ───────────────────
    // Require |imbalance| ≥ threshold on the entry side. High imbalance
    // means the book is stacked with passive orders supporting our direction.
    // imbalance_top1() = (bid_qty - ask_qty) / (bid_qty + ask_qty) at top level.
    // + = bid-heavy (UP pressure), - = ask-heavy (DOWN pressure).
    bool   book_imb_filter          = false;
    double book_imb_min             = 0.18;   // |imb| ≥ 0.18 per Homerun
    // ── Smart conviction exits (2026-06-10) ────────────────────────────
    // Forensics: 67% of stop-lossed positions would have WON at settlement —
    // blind price stops sell book noise. Exits now consult a conviction score:
    // votes in favor of our side from (1) window-anchored oracle delta,
    // (2) window-anchored Coinbase delta, (3) the cached predictor ensemble.
    bool   smart_exit_mode          = true;
    double conviction_min_delta_pct = 0.04;  // delta (%) needed for an oracle/CB vote
    int    smart_stop_max_votes     = 0;     // stop-loss only fires when votes <= this
    double hard_stop_pct            = 60.0;  // catastrophe floor: stop regardless of votes
    int    ride_to_settle_secs      = 150;   // skip profit-take this close to settle when votes >= 2
    double oracle_max_token_price   = 0.80;   // max token price to enter in oracle-lag mode
    // Assets to skip entirely for new entries (e.g. ["SOL"]). Settlement backtest
    // showed SOL is a consistent net loser (too volatile for the oracle-lag edge).
    std::vector<std::string> blocked_assets;
    bool   autopilot_on_start       = true;   // enable autopilot immediately at launch
    int    autopilot_startup_delay_secs = 60; // short wait for initial oracle ticks
    // Multi-signal predictor (ensemble: BS fair value + RSI/MACD/BB/slope/microstructure)
    // When enabled, autopilot ALSO fires on predictor signals (logical OR with oracle-lag).
    bool   predictor_mode           = false;  // enable predictor as autopilot trigger
    double min_predictor_edge       = 0.02;   // 2% edge = catch more
    double min_predictor_conf       = 0.35;   // tightened — require high-quality signal
    double predictor_max_token_price= 0.80;
    // When oracle is FLAT (< threshold), predictor needs even higher conf to fire on technicals alone
    double predictor_no_oracle_conf = 0.55;   // moderate when oracle is flat
    // Profit-take: auto-flatten a position once it's up >= profit_take_min_pct
    // OR bid >= profit_take_price_thresh (near-resolved).
    bool   profit_take_mode         = false;
    double profit_take_min_pct      = 20.0;   // % gain on avg cost
    double profit_take_momentum_max = 0.10;   // tech_score for held side <= this => stalled
    double profit_take_price_thresh = 0.90;   // also take profit when bid >= this price
    // Stop-loss: auto-flatten when bid reaches stop_loss_price (when > 0),
    // otherwise when it drops stop_loss_pct below avg cost.
    bool   stop_loss_mode          = false;
    double stop_loss_pct           = 55.0;
    double stop_loss_price         = 0.0;    // absolute executable bid; 0 = percentage mode
    // Timed-exit: DISABLED (0). Let all positions settle naturally via
    // the oracle. Selling at $0.12 to save $0.01 at settlement isn't
    // worth the taker fees. Stop-loss at 55% catches true collapses.
    int    timed_exit_secs          = 0;
    // Endgame snipe: in the last `endgame_secs_left` seconds of a window,
    // if a side's price implies high probability AND there's still
    // `endgame_min_profit_pct` % gain to settlement, buy it. Time decay
    // makes a reversal extremely unlikely => low risk, free profit.
    bool   endgame_mode             = false;
    int    endgame_secs_left        = 60;     // only fire when secs_left <= this
    double endgame_min_profit_pct   = 12.0;   // require >= this % to settle ($1.00)
    double endgame_max_price        = 0.92;   // never buy above (=> >= 8.7% min gross)
    double endgame_min_price        = 0.55;   // and never below (sub-coin-flip; not "endgame")
    double endgame_min_oracle_pct   = 0.05;   // require oracle to agree by at least this %
    // Fee model — Polymarket taker fee formula: fee = rate * p * (1-p) * shares
    // BTC/ETH updown-15m: rate=0.072, exponent=1 (rebate program expired 2026-04-30)
    double taker_fee_rate           = 0.07;
    int    fee_exponent             = 1;

    // ── Momentum Scalping (15m windows) ────────────────────────────────────────
    // Follow price velocity. UP rising → buy UP, DOWN rising → buy DOWN.
    // Catches every meaningful move. Exit via profit_take / stop_loss.
    bool   momentum_scalp_enabled   = false;
    double momentum_scalp_min_price = 0.15;   // Min entry price (avoid sub-15c noise)
    double momentum_scalp_max_price = 0.65;   // Max entry price (leave room for moves)
    double momentum_scalp_velocity_thresh = 0.0020;  // $/s — ~2c over 10s = real move, not noise
    int    momentum_scalp_min_secs = 30;      // Minimum seconds left in window
    int    momentum_scalp_max_secs = 900;     // Maximum (15m windows = 900s)

// ── Momentum Scalp V2 (sub-second TickBuffer + trailing stop) ──
    bool   scalp_v2_enabled              = false;
    double scalp_v2_velocity_thresh      = 0.0015;  // $/s (aligned with momentum_scalp 0.0020)
    double scalp_v2_min_price            = 0.08;    // was 0.15
    double scalp_v2_max_price            = 0.75;    // was 0.65
    int    scalp_v2_min_secs             = 15;      // was 30
    int    scalp_v2_max_secs             = 3600;    // was 900 (all timeframes)
    int    scalp_v2_max_entries_per_window = 5;      // was 3
    double scalp_v2_per_entry_usd        = 3.00;    // $3 per scalp entry
    // Exit: Momentum Reversal
    bool   scalp_v2_reversal_exit        = false;
    double scalp_v2_reversal_thresh      = -0.0010;  // $/s (tightened from -0.0050 to align with entry thresh 0.0015)
    int    scalp_v2_reversal_window_us   = 1000000; // 1s lookback
    // Exit: Trailing Stop
    bool   scalp_v2_trailing_stop        = false;
    double scalp_v2_trail_start_loss     = 0.02;    // initial stop below entry
    double scalp_v2_trail_ratchet_pct    = 0.50;     // ratchet aggressiveness
    double scalp_v2_trail_min_distance   = 0.01;     // min distance from peak
    // Exit: Tight Profit Target
    bool   scalp_v2_profit_take          = false;
    double scalp_v2_profit_target        = 0.03;    // 3¢ after fees
    int    scalp_v2_min_hold_ms          = 5000;

    // ── Momentum Scalp V3 (fast in-window rotation + runner promotion) ──
    bool   scalp_v3_enabled              = false;
    // Require the scalp side to agree with the oracle move (>= min_oracle_delta_pct
    // in the side's direction). Velocity alone is book noise: 154/156 oracle-flat
    // scalp entries on 2026-06-09 were a 53% coin flip; the oracle-confirmed ones won.
    bool   scalp_v3_require_oracle_agree = true;
    std::vector<std::string> scalp_v3_blocked_assets;
    double scalp_v3_velocity_thresh      = 0.0016;  // $/s; side must be moving now
    double scalp_v3_accel_min            = -0.0004; // allow mild decel, block fading moves
    double scalp_v3_dominance_thresh     = 0.0010;  // chosen side velocity must beat opposite
    double scalp_v3_min_confidence       = 0.58;
    double scalp_v3_min_price            = 0.24;
    double scalp_v3_max_price            = 0.76;
    int    scalp_v3_min_secs             = 18;
    int    scalp_v3_max_secs             = 270;
    int    scalp_v3_max_entries_per_window = 3;
    double scalp_v3_per_entry_usd        = 5.00;
    int    scalp_v3_cooldown_ms          = 8000;
    double scalp_v3_max_spread_pct       = 7.5;
    int    scalp_v3_max_book_age_ms      = 1500;
    // Exit: direction change / fading momentum.
    bool   scalp_v3_reversal_exit        = true;
    double scalp_v3_reversal_thresh      = -0.0011;
    int    scalp_v3_reversal_window_us   = 1000000;
    double scalp_v3_reversal_min_profit  = 0.03;   // reversal exit only fires in profit (≥ fee cover)
    int    scalp_v3_min_hold_ms          = 2500;
    // Exit: long-token trailing stop for both UP and DOWN shares.
    bool   scalp_v3_trailing_stop        = true;
    double scalp_v3_trail_start_loss     = 0.026;
    double scalp_v3_trail_ratchet_pct    = 0.55;
    double scalp_v3_trail_min_distance   = 0.012;
    double scalp_v3_trail_stop_pct       = 0.0;   // % loss from entry for initial stop (0 = use absolute trail_start_loss)
    // Optional fixed profit clip. Disabled by default so runners can breathe.
    bool   scalp_v3_profit_take          = false;
    double scalp_v3_profit_target        = 0.070;
    double scalp_v3_profit_target_pct    = 0.0;   // % gain on entry; overrides absolute when > 0
    // Runner: when a scalp is already strong late in the window, stop scalping it
    // and hold for settlement unless momentum clearly dies.
    bool   scalp_v3_runner_enabled       = true;
    double scalp_v3_runner_min_bid       = 0.82;
    double scalp_v3_runner_min_profit    = 0.12;
    int    scalp_v3_runner_max_secs_left = 75;
    double scalp_v3_runner_velocity_floor= -0.0003;

    // ── BTC Coinbase → Chainlink lead/lag window scalper ──
    // Independent strategy path for the new 5m and 15m bot instances. It
    // requires a fresh Coinbase impulse that is not yet reflected in the
    // official Chainlink reference, then checks a conservative, Coinbase-
    // adjusted settlement forecast before quoting a passive Polymarket bid.
    bool   lead_lag_scalp_enabled = false;
    // PAPER-only experimental entry path using the event-time Binance OFI
    // detector instead of the Coinbase lead/lag trigger.
    bool   lead_lag_scalp_impulse_only = false;
    // Disable settlement-value entries when no Coinbase lead is present.
    // Coinbase entries still require the same conservative probability and
    // net-edge guards before they can place a maker quote.
    bool   lead_lag_scalp_model_value_enabled = true;
    int    lead_lag_scalp_horizon_s = 0;       // 0 = both supported windows
    double lead_lag_scalp_min_signal_confidence = 0.55;
    double lead_lag_scalp_min_model_confidence = 0.20;
    double lead_lag_scalp_min_cb_move_bps = 0.5;
    double lead_lag_scalp_min_cb_lead_bps = 0.5;
    double lead_lag_scalp_max_oracle_counter_bps = 1.5;
    double lead_lag_scalp_min_net_edge_pct = 1.5;
    double lead_lag_scalp_min_price = 0.03;
    double lead_lag_scalp_max_price = 0.86;
    double lead_lag_scalp_max_spread_pct = 8.0;
    int    lead_lag_scalp_max_book_age_ms = 1000;
    int    lead_lag_scalp_stop_entry_secs = 45;
    int    lead_lag_scalp_stop_entry_5m_secs = 90;
    int    lead_lag_scalp_stop_entry_15m_secs = 270;
    int    lead_lag_scalp_max_secs_remaining_5m = 0;  // 0 disables the opening delay
    int    lead_lag_scalp_max_secs_remaining_15m = 0; // 0 disables the opening delay
    int    lead_lag_scalp_flatten_secs = 30;
    int    lead_lag_scalp_flatten_5m_secs = 90;
    int    lead_lag_scalp_flatten_15m_secs = 270;
    int    lead_lag_scalp_max_entries_per_window = 24;
    double lead_lag_scalp_order_usd = 5.0;
    double lead_lag_scalp_profit_target_pct = 15.0;
    double lead_lag_scalp_hard_stop_pct = 12.0;
    int    lead_lag_scalp_hard_stop_ticks = 0; // 0 disables the entry-relative tick stop
    bool   lead_lag_scalp_reversal_exit_enabled = true;
    double lead_lag_scalp_reversal_oracle_confirm_bps = 0.0; // 0 disables oracle confirmation
    double lead_lag_scalp_rearm_bps = 0.25;

    // ── Late-window settlement sniper ──
    // Quote-band paper limit buys; qualifying same-side clips may accumulate
    // while filled shares are held to settlement.
    bool   late_window_sniper_enabled = false;
    int    late_window_sniper_min_secs = 1;
    int    late_window_sniper_max_secs = 45;
    int    late_window_sniper_min_secs_5m = 1;
    int    late_window_sniper_max_secs_5m = 45;
    int    late_window_sniper_min_secs_15m = 1;
    int    late_window_sniper_max_secs_15m = 45;
    double late_window_sniper_min_price = 0.82;
    double late_window_sniper_max_price = 0.97;
    double late_window_sniper_min_net_edge_pct = 3.0;
    double late_window_sniper_order_usd = 5.0;
    int    late_window_sniper_max_entries_per_window = 5;
    int    late_window_sniper_hard_stop_ticks = 0; // 0 disables the entry-relative emergency stop

    // Isolated, paper-only implementation of the reference engine's simple
    // 5m simulation strategy: fixed 5-share UP maker entry, maker target,
    // then a paper-only emergency exit 30s before close.
    bool   simulation_strategy_enabled = false;

    // ── Trend Filter (easy to revert: set trend_filter_enabled = false) ──
    bool   trend_filter_enabled       = false;   // toggle: set true to enable, false to disable
    int    trend_filter_windows       = 5;        // how many past windows to consider
    double trend_filter_strength      = 0.04;     // edge penalty per losing streak window
    // NOTE: True arbitrage requires buying BOTH UP and DOWN shares.
    // Current implementation only buys ONE side, so it's disabled until
    // we can implement proper two-sided arbitrage (requires two separate orders).
    // Guaranteed profit: if up_price + down_price < 1.0, buy both sides
    // At settlement, one pays $1, the other $0 → profit = 1 - (up + down)
    bool   arbitrage_enabled       = false;  // DISABLED - not true arbitrage yet
    double arbitrage_max_total     = 1.00;   // Only enter when up + down < this
    double arbitrage_min_profit    = 0.05;   // Minimum expected profit (5c)

    // ── Oracle-direction reversal exit ─────────────────────────────────────────
    // Exit a winning position when the oracle flips direction. Only fires when
    // bid > avg_price (never exits at a loss — stop_loss_mode covers that).
    bool   oracle_reversal_exit          = false;
    int    oracle_reversal_min_hold_secs = 30;   // minimum hold before checking reversal

    // ── Oracle panic exit (loss-side complement to oracle_reversal_exit) ─────────
    // When the oracle moves DECISIVELY against an open position, bail at market
    // NOW — before the losing side's book gaps through the stop_loss threshold
    // (the -64%/-84% gap-through losses). The oracle reprices ahead of the
    // orderbook, so reacting to it catches a bid before it evaporates. Fires on
    // the LOSS side only (bid <= avg); the profit side is left to oracle_reversal.
    bool   oracle_panic_exit          = false;
    double oracle_panic_delta_pct     = 0.25;  // adverse oracle move (% of underlying) that triggers immediate exit
    int    oracle_panic_min_hold_secs = 10;    // min hold before panic-exit can fire (avoid entry wobble)

    // ── Stop-loss reverse ──────────────────────────────────────────────────────
    // After a stop-loss fires, buy the opposite side if >= 90s remain in window.
    bool   stop_loss_reverse = false;

    // ── Entry floor (P3) ─────────────────────────────────────────────────────────
    // Lowest share price we will buy. Together with oracle_max_token_price /
    // predictor_max_token_price (the ceiling) this defines the mid-window band
    // (e.g. 0.40–0.60): cheap enough for real upside to $1, not a lottery ticket.
    double min_entry_share_price = 0.40;

    // ── Predictor agreement gate on oracle-lag entries (P1) ──────────────────────
    // Only take an oracle-lag trade when the ensemble predictor agrees with the
    // direction and is confident. Filters oracle blips that mean-revert.
    //   UP  entry needs pred_up >= 0.5 + margin ; DOWN needs pred_up <= 0.5 - margin.
    bool   oracle_require_pred_agree = true;
    double pred_agree_margin         = 0.02;
    double pred_agree_min_conf       = 0.40;

    // ── Momentum-flip exit + reverse (P2) ────────────────────────────────────────
    // Watch each open position; the instant the predictor flips to favour the
    // OTHER side with confidence, fire-sale immediately (profit OR loss) and buy
    // the opposite side if enough time remains. This is the "momentum changed →
    // bail now and flip" behaviour. stop_loss_pct is a separate configured backstop.
    bool   momentum_flip_exit     = false;
    double momentum_flip_margin   = 0.06;   // pred must cross against us by this past 0.5
    double momentum_flip_min_conf = 0.45;   // require this predictor confidence to act
    bool   momentum_flip_reverse  = false;  // buy the opposite side after flipping out
    int    momentum_flip_min_secs = 60;     // only flip/reverse with >= this secs left
    int    momentum_flip_min_hold_ms = 15000;

    // ── Stale-book entry guard (root-cause fix for bad fills) ──
    // Never open/reverse into a book we haven't seen update within this many ms.
    // Pricing off a stale book is the cause of "instant -25%" entries (we buy at
    // a phantom ask while the live market is far away) that then trip stop-loss.
    int    max_book_age_ms        = 2000;
    // ── Predictor compute throttle (lock-starvation fix, 2026-06-06) ──
    // The autopilot sweep runs the full ensemble predict() per market while
    // holding the global state.mu. predict() copies up to a YEAR of candles and
    // runs multi-horizon indicators, so on ~256 markets the lock was held ~5s,
    // starving the on_book callback (which needs the same mutex) → books went
    // stale even though WS frames kept arriving. Fix: cache each market's
    // PredictorOutput and only recompute when older than predictor_refresh_ms,
    // and never recompute more than predict_budget_per_sweep markets per sweep
    // (the rest reuse cache). Price-sensitive edges are recomputed cheaply each
    // sweep against the live mid, so entry pricing stays responsive. The slow
    // ensemble signals (vol/trend/RSI/MACD) don't move meaningfully in 2s.
    int    predictor_refresh_ms     = 2000;  // recompute a market's ensemble at most this often
    int    predict_budget_per_sweep = 10;    // max predict() calls per sweep (bounds lock hold)
    int    predictor_recent_1m_bars = 10080;
    int    predictor_recent_1m_ohlcv_bars = 10080;
    int    predictor_long_60m_bars = 43800;
    int    predictor_5m_closes = 288;
    int    predictor_15m_closes = 96;
    int    predictor_5m_bars = 30;
    int    predictor_15m_bars = 24;
    int    predictor_60m_bars = 24;
    // ── Reversal anti-churn guard ──
    // After a stop-loss/momentum-flip reverses into the opposite side on a market,
    // suppress further reverses on that market for this long (breaks the
    // flip→stop→flip-back death spiral that bleeds the bank into the daily kill).
    int    reversal_cooldown_secs = 45;     // 0 = disabled (old behaviour)
    int    max_reversals_per_window = 0;    // max reverses per market window
    int    stop_loss_cooldown_secs = 600;
    int    reverse_min_secs_left = 90;
    double reverse_slippage_pct = 0.05;
    double reverse_slippage_extra = 0.02;
    // ── Wide-spread entry gate (the real churn root cause) ──
    // Skip entries/reverses when the book's (ask-bid)/ask spread exceeds this %.
    // On a wide-spread market you buy the ask and the position is instantly marked
    // to the bid — an instant "loss" == the spread — which trips the stop-loss the
    // moment you open, regardless of direction. Don't trade markets you can't get
    // a fair fill in. 0 = disabled.
    double max_entry_spread_pct   = 12.0;
    // ── Stop-loss min-hold ──
    // Don't stop-loss a position younger than this many seconds: a fresh entry's
    // bid-marked P&L is dominated by the spread, not by being wrong. Mirrors the
    // momentum-flip 15s hold so a just-opened position can't be instantly stopped
    // (and then reversed) into a churn spiral.
    int    stop_loss_min_hold_secs = 20;
    double timed_exit_min_bid = 0.03;
    int    timed_exit_throttle_ms = 2000;
    int    settlement_grace_ms = 30000;

    // ── Window-fraction checkpoint predictor (PREDICTOR instance) ──
    // At deterministic fractions of each window's elapsed time, evaluate an
    // UP/DOWN prediction and optionally buy the selected side at the current
    // ask. Orders bypass the rule-engine submit path entirely.
    bool   window_predictor_mode = false;
    std::vector<double> window_predictor_fractions;    // e.g. {0.25, 0.5, 0.75}
    std::vector<double> window_predictor_amounts_usd;  // parallel to fractions
    std::string window_predictor_buy_mode = "usd";     // "usd" | "shares"
    double window_predictor_price_min = 0.05;          // sanity band on the ask
    double window_predictor_price_max = 0.95;
    bool   window_predictor_skip_late = true;          // never fire a passed checkpoint
    int    window_predictor_late_grace_ms = 15000;     // > sweep interval; beyond this, checkpoint is skipped
    int    window_predictor_min_secs_left = 10;        // no fires in the death zone
    std::string window_predictor_source = "WPRED";     // vp.orders.source marker
    std::vector<std::string> window_predictor_assets;  // empty = all
    std::vector<int> window_predictor_horizons;        // empty = all (300/900/3600)
    std::vector<std::string> window_predictor_pairs;   // exact "ASSET:HORIZON" allowlist; empty = assets x horizons
    bool   window_predictor_pm_flow = true;            // feed PM trade-flow imbalance into the ensemble
    int    window_predictor_depth_topn = 5;            // book depth levels for imbalance feature
    bool   window_predictor_vote_mode = true;           // model + oracle + Coinbase majority direction
    double window_predictor_vote_min_delta_pct = 0.005; // ignore sub-noise oracle/CB window moves
    bool   window_predictor_require_unanimous = false;  // require all three votes to select the same side
    double window_predictor_min_abs_oracle_delta_pct = 0.0;
    double window_predictor_min_win_return_pct = 0.0;   // net return if the selected share settles at $1
    double window_predictor_min_net_edge = 0.0;         // model fair probability - fee-adjusted entry cost
    double window_predictor_size_max_mult = 2.0;        // upward-only multiplier; configured amount is floor
    int    window_predictor_calibration_min_samples = 30; // settled samples per bucket before the calibration gate bites (0 = off)
    double window_predictor_calibration_margin = 0.02;  // gate when bucket realized win rate < mean predicted - margin

    // ── Adaptive Confidence-Timed Entry (ADAPTIVE mode) ──────────────────────────
    // Instead of firing at a fixed time, the bot tracks the predictor's confidence
    // trajectory and enters when confidence crosses the user-specified threshold.
    // Higher threshold → later entry (more certain); lower threshold → earlier entry.
    // This is the "specify % certainty, bot figures out when to buy" feature.
    bool   adaptive_mode = false;                      // enable adaptive timed entries
    double adaptive_target_conf = 0.90;                // target confidence (0..1). 0.90 = 90% certain
    int    adaptive_min_secs_left = 30;                // never enter this close to expiry
    int    adaptive_max_secs_left = 7200;              // never enter this early in window; cockpit max
    int    adaptive_trajectory_window_ms = 30000;      // lookback window for conf slope (ms)
    double adaptive_slope_threshold = 0.01;            // conf/sec slope must exceed this (rising confidence)
    double adaptive_conf_trend_min = 0.02;             // confidence must have risen this much from trajectory low
    int    adaptive_trajectory_samples = 10;           // min samples before adaptive can fire
    bool   adaptive_require_predictor_agree = true;     // predictor must agree with oracle direction
    double adaptive_oracle_agree_margin = 0.015;       // predictor_up margin for oracle agreement
    bool   adaptive_use_wpred_payload = true;           // store full predictor payload in order jsonb
};
struct RiskConfig {
    double max_order_usd             = 5.00;   // $5 per order cap
    double max_deployed_usd          = 20.00;  // $20 total at once
    double max_per_window_usd        = 5.00;   // $5 per window cap
    double min_order_usd             = 1.00;    // Polymarket minimum per bet
    double clob_min_shares_fallback  = 5.00;
    double wallet_balance_warn_above = 1000.00;
    double daily_loss_kill_pct       = 0.0;     // deprecated; daily loss kill disabled fleet-wide
    // Grace period after startup before the daily-kill arms. Lets any restored/
    // inherited positions settle or be exited WITHOUT counting toward the session
    // loss (otherwise a restart that liquidates an underwater inherited book
    // insta-trips the kill). The kill baseline is (re)sampled when grace ends.
    int    daily_kill_grace_secs     = 90;
    int    daily_kill_check_interval_ms = 5000;
    int    autopilot_heartbeat_secs  = 5;
};
struct WalletConfig {
    std::string credentials_file = "secrets/polymarket_apikey.json"; // per-wallet L2 credential bundle
    std::string private_key_file;   // path to file containing hex private key
    std::string api_key;            // CLOB HMAC API key
    std::string api_secret;         // CLOB HMAC API secret (base64url)
    std::string api_passphrase;     // CLOB HMAC passphrase
    bool        neg_risk = true;    // BTC/ETH UP/DOWN are neg_risk markets
    int         signature_type = 1; // 0=EOA, 1=POLY_PROXY (browser), 2=POLY_GNOSIS_SAFE
    double      starting_balance_usd = 100.00;
    std::string builder_code;       // bytes32 hex — embedded in every signed order
};
struct TuiConfig {
    int  fps_ac      = 30;
    int  fps_battery = 10;
    bool truecolor   = true;
    bool confirm_default = true;
};
struct DataCollectionConfig {
    bool  oracle_ticks_enabled       = true;
    bool  binance_ticks_enabled      = true;
    bool  coinbase_ticks_enabled     = true;
    bool  window_events_enabled      = true;
    bool  features_enabled           = true;
    int   features_write_interval_ms = 1000;  // ms between predictor feature snapshots
    bool  book_snapshots_enabled     = true;  // vp.pm_book_snapshots — 26 GB/6wk, disable to save disk
    bool  pm_trades_enabled          = true;  // vp.pm_trades — 9 GB, disable to save disk
    // Log rotation - prevents disk fill crashes
    int   log_max_bytes              = 100'000'000;  // 100 MB per file
    int   log_keep_files             = 5;            // rotate 5 files
    // Disk space monitoring - degrade gracefully when low
    int   disk_space_warn_mb        = 500;   // warn when free space below this
    int   disk_space_kill_mb        = 100;   // disable data collection below this
    int   disk_check_interval_ms    = 5000;
    double disk_recovery_multiplier = 2.0;
    int   candle_store_max_bars_per_asset_timeframe = 2'750'000;
    int   trade_flow_max_records_per_symbol = 50'000;
};

struct RuntimeConfig {
    // Multi-instance tag: stamped on logs/DB orders/session reports so several
    // bots with different configs can run and be compared. CLI --instance wins.
    std::string instance_label;
    TradeMode   mode         = TradeMode::Paper;
    std::string log_level    = "info";
    std::string journal_dir  = "./journal";
    int         config_hot_reload_interval_ms = 2000;
    int         cockpit_command_poll_ms = 1000;
    int         wallet_balance_refresh_ms = 30000;
    int         live_heartbeat_ms = 5000;
    int         position_snapshot_interval_ms = 30000;
};

struct Config {
    RuntimeConfig       runtime;
    PgConfig            postgres;
    PolymarketConfig    polymarket;
    BinanceConfig       binance;
    PolygonConfig       polygon;
    LlmConfig           llm;
    StrategyHot         strategy;
    RiskConfig          risk;
    TuiConfig           tui;
    WalletConfig        wallet;
    DataCollectionConfig data_collection;

    static Config load(const std::string& path);
    void          dump_summary() const;     // log a human readable summary
};

} // namespace vp
