#pragma once
// PolySparQ++ TUI — notcurses cockpit.
//
// Public API is a snapshot interface: the engine writes the latest state to
// the Tui's shared snapshot under a mutex; the renderer pulls a copy at
// 30 fps (10 fps on battery) and paints diff cells.
#include "core/types.hpp"
#include "feeds/polymarket_rest.hpp"
#include "engine/forecast.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vp {

class OrderBook;

struct WatchlistRow {
    engine::Forecast reference_forecast;
    engine::Forecast fast_forecast;
    bool cb_data_ready = false;
    std::string condition_id;
    std::string asset;          // BTC / ETH
    int         horizon_s = 0;
    double      up_price = 0;
    double      down_price = 0;
    double      up_bid = 0;
    double      up_ask = 0;
    double      down_bid = 0;
    double      down_ask = 0;
    int64_t     window_close_ms = 0;
    double      target_price = 0;
    double      asset_price = 0;
    double      gap_usd = 0;     // signed: asset - target
    double      gap_pct = 0;
    double      oracle_delta_pct = 0; // same as gap_pct but from oracle feed if available
    double      cb_delta_pct = 0;     // Coinbase move since window open — LEADS the oracle by ~1.5-3s
    char        signal_glyph = '-';   // '+' buy / '-' wait / 'X' avoid
    // Open price tracking (first book tick seen for this market)
    double      open_up_price = 0;
    double      open_down_price = 0;
    // ── Polymarket COIN price (oracle-based — what determines who wins) ──
    // coin_open_price = Chainlink RTDS oracle at window open (= the strike).
    // coin_now_price  = Chainlink RTDS oracle latest tick.
    // 0 means "not ticked yet" → renderer prints "—".
    double      coin_open_price = 0;
    double      coin_now_price  = 0;
    // Momentum: $/s over last 10s (+ = rising price)
    double      up_velocity = 0;
    double      down_velocity = 0;
    // Last ~30 UP price ticks for sparkline
    std::vector<double> up_price_hist;
    // ── Multi-signal prediction snapshot (engine writes; UI reads) ──
    double      pred_up        = 0.5;  // ensemble probability UP wins
    double      pred_fair_up   = 0.5;  // Black-Scholes anchor
    double      pred_edge_up   = 0;
    double      pred_edge_down = 0;
    double      pred_conf      = 0;
    double      pred_rsi14     = 50;
    double      pred_macd_hist = 0;
    double      pred_bb_z      = 0;
    double      pred_oracle_vel_1m = 0;
    double      pred_oracle_accel  = 0;
    double      pred_volume_surge_ratio = 1.0;
    double      pred_candle_body   = 0;
    double      pred_trade_flow    = 0;
    double      pred_tech_score= 0;
    double      pred_rv_30d = 0;
    double      pred_rv_90d = 0;
    double      pred_rv_1y  = 0;
    double      pred_slope_7d_pct  = 0;
    double      pred_slope_30d_pct = 0;
    double      pred_slope_1y_pct  = 0;
    double      pred_history_regime_score = 0;
    // ── Signal strength tracking (for gauges and calibration) ──────
    double      pred_signal_strength_rsi    = 0;
    double      pred_signal_strength_macd   = 0;
    double      pred_signal_strength_bb     = 0;
    double      pred_signal_strength_slope  = 0;
    double      pred_signal_strength_micro  = 0;
    double      pred_signal_strength_oracle = 0;
    double      pred_signal_strength_trade  = 0;
    double      pred_signal_strength_tick   = 0;
    double      pred_signal_strength_pmflow = 0;
    double      pred_signal_strength_cb     = 0;
    double      pred_signal_strength_xmom   = 0;
    double      pred_signal_strength_vol    = 0;
    double      pred_signal_strength_body   = 0;
    double      pred_signal_strength_regime = 0;
    int         pred_signals_agreeing       = 0;
    int         pred_signals_total          = 0;
    // ── Additional indicators (Phase 2.0) ──────────────────────────
    double      pred_stoch_k = 50;         // Stochastic %K (0..100)
    double      pred_stoch_d = 50;         // Stochastic %D
    double      pred_adx     = 0;          // ADX (0..100, >25 = trending)
    double      pred_pdi     = 0;          // +DI
    double      pred_mdi     = 0;          // -DI
    double      pred_cci     = 0;          // CCI (-100..+100 typical)
    // ── Adaptive confidence trajectory (ADAPTIVE mode) ─────────────
    bool   pred_conf_trajectory_ready = false;
    double pred_conf_trajectory_slope = 0.0;
    int    pred_conf_trajectory_samples = 0;
    double pred_conf_low = 1.0;
    int64_t pred_adaptive_countdown_s = -1;
    // ── Binance bookTicker confirmation (Phase 6.3) ──
    // bt_mid_delta_pct = 2s move of the Binance top-of-book mid (%).
    // bt_imbalance     = (bidQty-askQty)/(bidQty+askQty), + = bid-heavy (UP pressure).
    // 0 values mean "no bookTicker data yet" — the gate treats that as pass-through.
    double      bt_mid_delta_pct = 0;
    double      bt_imbalance     = 0;
    // ── Oracle micro-dynamics for the digital FV edge (Phase 5.1) ──
    // per-second realized vol from the oracle tick series + last-move z.
    double      oracle_vol_per_sec = 0;
    double      oracle_jump_z      = 0;
    // ── Futures liquidation pressure (Phase 4.1) ──
    // Net notional (USD) of last-30s forceOrder events. Negative = SELL
    // liquidations dominating (downward pressure). 0 = no futures feed.
    double      liq_pressure_usd   = 0;
    // ── Polymarket orderbook imbalance (Phase 4.2) ──
    // imbalance_top1() = (bid_qty - ask_qty) / (bid_qty + ask_qty) at top level.
    // + = bid-heavy (UP pressure), - = ask-heavy (DOWN pressure).
    double      up_imbalance       = 0;
    double      down_imbalance     = 0;
};

struct ConnectionStatus {
    std::atomic<bool> pm_md{false};
    std::atomic<bool> pm_user{false};
    std::atomic<bool> binance{false};
    std::atomic<bool> rtds{false};
    std::atomic<bool> llm{false};
    std::atomic<bool> pg{false};
    std::atomic<bool> coinbase{false};
    std::atomic<int64_t> wire_to_proc_us{0};
};

struct TopBarSnap {
    std::string mode_label;     // LIVE/PAPER/REPLAY
    double      equity_usd = 0;
    double      day_pnl_usd = 0;
    double      wallet_start_usd = 0;   // starting paper wallet (for context)
    bool        autopilot_global = false;
    bool        kill_switch = false;
    bool        disk_full = false;      // disk space critically low
    int         trades_won = 0;
    int         trades_lost = 0;

    // ── Wallet strip (8 fields the user wants front-and-center) ──
    // All values are USD unless noted. Negative or 0 means "not yet
    // computable" — the renderer prints "—" in that case.
    double      wallet_total_if_wins = 0;   // cash + open shares × $1.00 (best case)
    double      wallet_cash_value    = 0;   // cash on hand if every open position settles to $0
    double      total_invested_cost  = 0;   // Σ open.shares × open.avg_price

    // Cross-source live price for the ACTIVE market's asset (BTC or ETH).
    // pm_coin_price MUST come from the polymarket.com oracle (Chainlink
    // RTDS) — never substitute Binance/Coinbase here. 0 = "—".
    std::string active_asset;          // "BTC" or "ETH" (empty if none)
    double      pm_coin_price       = 0;
    double      binance_coin_price  = 0;
    double      coinbase_coin_price = 0;

    // Window context — strike to beat, and the live UP/DOWN share asks.
    double      window_open_coin_price = 0;  // strike (target_price) of active market
    double      up_share_cost_live     = 0;  // best ask UP, fallback mid
    double      down_share_cost_live   = 0;  // best ask DOWN, fallback mid
};

// One settled trade — kept around so the user can always see "what just
// happened" even after the position vanishes.
struct RecentResultRow {
    int64_t     ts_ms = 0;
    std::string asset;          // BTC / ETH
    std::string side_label;     // UP / DN
    int         horizon_s = 0;
    double      shares = 0;
    double      entry_price = 0;
    double      settle_price = 0;
    double      pnl_usd = 0;
    bool        win = false;
};

struct ActiveMarketSnap {
    std::string condition_id;
    std::string asset;
    int         horizon_s = 0;
    double      up_price = 0, down_price = 0;
    double      up_microprice = 0, down_microprice = 0;
    double      asset_price = 0;
    double      gap_usd = 0, gap_pct = 0;
    double      gap_velocity_per_s = 0;
    int64_t     window_close_ms = 0;
    int64_t     window_open_ms = 0;
    double      target_price = 0;

    // Top-N order book for UP token
    std::vector<PriceLevel> up_bids;
    std::vector<PriceLevel> up_asks;

    // Mini history of asset price for surge sparkline (Binance)
    std::vector<double>     asset_recent;
    // UP share price open + history for scalp indicator
    double                  open_up_price = 0;
    double                  open_down_price = 0;
    double                  up_velocity = 0;      // $/s, + = rising
    std::vector<double>     up_price_hist;         // last 30 ticks

    // Decision panel (set by engine when present)
    std::string decision_text;   // e.g. "WAIT  conf 0.52  floor 0.65"
    std::string reason_top1;
    std::string reason_top2;
    std::string reason_top3;
    std::string would_change_mind;

    // ── Live cross-source price comparison ──────────────────────────
    double      coinbase_price = 0;     // BTC/ETH spot from Coinbase
    double      pm_open_price  = 0;     // = target_price (window strike)
    double      oracle_price   = 0;     // Chainlink RTDS current (settlement oracle)

    // ── Multi-signal prediction breakdown (engine writes; UI reads) ─
    double      pred_up        = 0.5;
    double      pred_fair_up   = 0.5;
    double      pred_edge_up   = 0;
    double      pred_edge_down = 0;
    double      pred_conf      = 0;
    double      pred_rsi14     = 50;
    double      pred_macd_hist = 0;
    double      pred_bb_z      = 0;
    double      pred_slope     = 0;
    double      pred_atr_pct   = 0;
    double      pred_vwap_dev  = 0;
    double      pred_micro_imb = 0;
    double      pred_micro_imb_down = 0;
    double      pred_oracle_vel_1m = 0;
    double      pred_oracle_accel  = 0;
    double      pred_volume_surge_ratio = 1.0;
    double      pred_candle_body   = 0;
    double      pred_trade_flow    = 0;
    double      pred_tech_score= 0;
    std::string pred_summary;
};

struct TuiSnapshot {
    TopBarSnap                 top;
    std::vector<WatchlistRow>  watchlist;
    ActiveMarketSnap           active;
    std::vector<std::string>   signal_history; // formatted lines, newest first
    std::vector<std::string>   alerts;
    std::string                status_hint;
    // Position panel
    double                     deployed_usd = 0;
    double                     max_deployed_usd = 3.0;
    std::vector<std::string>   position_lines; // formatted per-position strings
    // Settled trades, newest first (max ~12). Persistent record of W/L.
    std::vector<RecentResultRow> recent_results;
    // Which market is currently selected (condition_id)
    std::string                selected_condition_id;

    // ── TRADER'S BRIEF (LLM-authored situational update) ──
    // Refreshed every few minutes by a background advisor thread. Plain
    // text; the renderer wraps it into a colored box below the
    // prediction engine. ts_ms = wall-clock ms when the brief was
    // produced (renderer shows "updated Xm ago").
    std::string                trader_brief;
    int64_t                    trader_brief_ts_ms = 0;
    bool                       trader_brief_pending = false;
};

class Tui {
public:
    Tui();
    ~Tui();
    // headless=true skips notcurses entirely: no terminal, no input thread,
    // no rendering. The engine + logging run normally and run() blocks until
    // a quit signal. Used for unattended/background (supervisor-driven) runs.
    bool init(bool headless = false);
    void shutdown();

    // Snapshot updates from engine thread
    void update_snapshot(const TuiSnapshot& s);

    // Connection pip pointers (engine flips these; renderer reads atomically)
    ConnectionStatus& conn() noexcept { return *conn_; }

    // Run the render loop; blocks until shutdown() or quit key.
    // Returns when user pressed 'q' or shutdown signalled.
    void run();

    // Register a callback invoked in the input thread for trading keys.
    // Keys dispatched: B N S M K A space X
    void set_key_callback(std::function<void(char)> cb);

    // Was a quit keystroke received?
    bool wants_quit() const noexcept;

    // External quit request (engine/diag thread).
    void request_quit() noexcept;

    // Current highlighted watchlist row (set by ↑/↓ keys).
    int selected_row() const noexcept;

    // Splash drawn before init() if no terminal: noop for compatibility.
    void draw_splash(const char* version);

private:
    struct Impl;
    std::unique_ptr<Impl>       p_;
    std::unique_ptr<ConnectionStatus> conn_;
    std::mutex                  snap_mu_;
    TuiSnapshot                 snap_;
};

} // namespace vp
