#include "engine/recycler.hpp"
// PolySparQ++ — main entry point.
//
// Wires:
//   • Config + risk caps
//   • Postgres connection + async persist queue
//   • Polymarket REST market discovery (Gamma)
//   • Polymarket CLOB market-data WebSocket (live order books)
//   • Polymarket user WebSocket (live fills — live mode only)
//   • Binance trade stream (asset-price + surge feed)
//   • Rule engine (signal generation per watchlist row)
//   • PaperExecutor / LiveExecutor (selected by runtime.mode)
//   • LLM advisor (async, advisory only — never reaches executor)
//   • notcurses TUI (full cockpit: watchlist, book, positions, signal history)
//
// Threading:
//   • Feed WebSockets: each has its own io_context thread (inside WsClient).
//   • Persist thread: pops BoundedQueue<std::function<void()>> and calls Pg.
//   • Refresh thread: 200ms loop; rebuilds TuiSnapshot under SharedState mutex.
//   • TUI render/input thread: notcurses owns it inside tui.run().
//   • LLM advisor: detaches its own thread per query (self-limiting to 1 in-flight).
//   • All SharedState writes hold state.mu (coarse mutex — good enough for MVP).

#include "config/config.hpp"
#include "core/clock.hpp"
#include "core/cockpit_command.hpp"
#include "core/types.hpp"
#include "core/uuid.hpp"
#include "db/pg.hpp"
#include "db/persist_queue.hpp"
#include "engine/rules.hpp"
#include "engine/reference_forecasts.hpp"
#include "engine/candle_store.hpp"
#include "engine/predictor.hpp"
#include "engine/trade_flow_buffer.hpp"
#include "engine/pm_flow_buffer.hpp"
#include "engine/tick_buffer.hpp"
#include "engine/scalp_manager.hpp"
#include "exec/live.hpp"
#include "exec/paper.hpp"
#include "exec/positions.hpp"
#include "exec/gate_pipeline.hpp"
#include "risk/risk_manager.hpp"
#include "feeds/binance.hpp"
#include "feeds/coinbase_ws.hpp"
#include <array>
#include "feeds/polymarket_md.hpp"
#include "feeds/polymarket_user_ws.hpp"
#include "feeds/polymarket_rest.hpp"
#include "feeds/polymarket_rtds.hpp"
#include "core/oracle_buffer.hpp"
#include "llm/advisor.hpp"
#include "market/orderbook.hpp"
#include "tui/tui.hpp"
#include "net/http_client.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <fmt/format.h>

#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <csignal>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <deque>
#include <map>
#include <set>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
#include <unistd.h>

namespace {
std::atomic<bool> g_stop{false};
std::atomic<vp::Tui*> g_tui{nullptr};
void write_runtime_status(const std::string& instance, const std::string& session,
                          vp::TradeMode mode, const std::string& execution_mode,
                          bool uncertain_order_halt, bool autopilot, bool daily_kill, int64_t now_ms) noexcept {
    if (instance.empty() || !std::all_of(instance.begin(), instance.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '_' || c == '-';
        })) return;
    const auto path = std::filesystem::path("logs") /
                      ("polypulse-" + instance + ".runtime.json");
    const auto tmp = path.string() + "." + std::to_string(::getpid()) + ".tmp";
    try {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return;
        const auto safe_execution = execution_mode == "maker" || execution_mode == "hybrid"
            ? execution_mode : "taker";
        out << fmt::format(R"({{"schema_version":1,"instance":"{}","session_id":"{}","pid":{},"ts_ms":{},"mode":"{}","execution_mode":"{}","risk_state":"{}","autopilot":{},"daily_kill_fired":{}}})",
                           instance, session, ::getpid(), now_ms,
                           mode == vp::TradeMode::Live ? "LIVE" :
                           mode == vp::TradeMode::Replay ? "REPLAY" : "PAPER",
                           safe_execution,
                           uncertain_order_halt ? "ORDER_UNCERTAIN_HALT" : daily_kill ? "LOSS_LIMIT_HALT" : "NORMAL",
                           autopilot, daily_kill) << '\n';
        out.close();
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec) std::filesystem::remove(tmp, ec);
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
    }
}
void on_sig(int) {
    g_stop.store(true);
    if (auto* t = g_tui.load()) t->request_quit();
}

// Get available disk space in MB for the given path (or current dir if empty)
double get_free_space_mb(const std::string& path = ".") {
    try {
        auto space = std::filesystem::space(path);
        return (double)space.available / (1024.0 * 1024.0);
    } catch (...) {
        return -1.0;  // error
    }
}

struct SessionOrderRecord {
    int64_t ts_ms = 0;
    std::string client_order_id;
    std::string source;
    std::string condition_id;
    std::string asset;
    int horizon_s = 0;
    std::string token_id;
    vp::ShareSide side = vp::ShareSide::UP;
    vp::OrderDir dir = vp::OrderDir::Buy;
    double limit_price = 0.0;
    double fill_price = 0.0;
    double fill_shares = 0.0;
    double size_usd = 0.0;
    double fee_usd = 0.0;
    bool accepted = false;
    std::string status;
    std::string reject_reason;
};

std::tm local_tm_from_ms(int64_t ms) {
    std::time_t sec = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    localtime_r(&sec, &tm);
    return tm;
}

std::string wall_ts_compact(int64_t ms) {
    auto tm = local_tm_from_ms(ms);
    return fmt::format("{:04d}{:02d}{:02d}_{:02d}{:02d}{:02d}",
                       tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                       tm.tm_hour, tm.tm_min, tm.tm_sec);
}

std::string wall_ts_pretty(int64_t ms) {
    auto tm = local_tm_from_ms(ms);
    return fmt::format("{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}",
                       tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                       tm.tm_hour, tm.tm_min, tm.tm_sec);
}

std::string md_cell(std::string s) {
    for (auto& ch : s) {
        if (ch == '\n' || ch == '\r') ch = ' ';
        if (ch == '|') ch = '/';
    }
    return s;
}

// Read a private key from file; strip whitespace.
std::string load_private_key(const std::string& path) {
    if (path.empty()) return {};
    std::ifstream f(path);
    if (!f) { spdlog::warn("wallet: cannot open {}", path); return {}; }
    std::string key; std::getline(f, key);
    while (!key.empty() && (key.back() == '\n' || key.back() == '\r'
                             || key.back() == ' '))
        key.pop_back();
    // Strip 0x prefix if present
    if (key.size() >= 2 && key[0] == '0' && (key[1] == 'x' || key[1] == 'X'))
        key = key.substr(2);
    return key;
}

// Read LLM API key from file.
std::string load_file_str(const std::string& path) {
    if (path.empty()) return {};
    std::ifstream f(path);
    if (!f) return {};
    std::string s; std::getline(f, s);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}
} // namespace

namespace vp {

// Reduce a freshly discovered market list to the configured tradable universe.
// All policy values come from [polymarket] in polypulse.toml.
inline std::vector<PmMarket> select_active_universe(
        std::vector<PmMarket> markets,
        const PolymarketConfig& pm_cfg,
        const std::vector<std::string>& blocked_assets,
        std::int64_t now_ms) {
    auto is_blocked = [&](const std::string& a) {
        for (const auto& b : blocked_assets) if (a == b) return true;
        return false;
    };
    // Phase 4.4: per-horizon block knob — instant hot-reloadable disable.
    auto horizon_blocked = [&](int horizon_s) {
        for (const auto& h : pm_cfg.blocked_horizons_seconds)
            if (h == horizon_s) return true;
        return false;
    };
    auto horizon_allowed = [&](int horizon_s) {
        return std::find(pm_cfg.universe_horizons_seconds.begin(),
                         pm_cfg.universe_horizons_seconds.end(),
                         horizon_s) != pm_cfg.universe_horizons_seconds.end();
    };
    const size_t max_windows_per_series =
        (pm_cfg.universe_max_windows_per_series > 0)
            ? (size_t)pm_cfg.universe_max_windows_per_series
            : std::numeric_limits<size_t>::max();
    const size_t max_universe_markets =
        (pm_cfg.universe_max_markets > 0)
            ? (size_t)pm_cfg.universe_max_markets
            : std::numeric_limits<size_t>::max();
    const int64_t prewarm_ahead_ms =
        std::max(0, pm_cfg.universe_prewarm_ahead_minutes) * 60LL * 1000LL;

    // Bucket eligible markets by (asset|horizon) series.
    std::unordered_map<std::string, std::vector<PmMarket>> series;
    for (auto& m : markets) {
        if (!horizon_allowed(m.horizon_seconds)) continue;
        if (horizon_blocked(m.horizon_seconds)) continue;
        if (is_blocked(m.asset)) continue;
        if (m.window_close_ms > 0 && m.window_close_ms <= now_ms) continue;  // already closed
        series[m.asset + "|" + std::to_string(m.horizon_seconds)].push_back(std::move(m));
    }
    std::vector<PmMarket> out;
    if (max_universe_markets != std::numeric_limits<size_t>::max())
        out.reserve(max_universe_markets);
    for (auto& kv : series) {
        auto& v = kv.second;
        std::sort(v.begin(), v.end(), [](const PmMarket& a, const PmMarket& b) {
            return a.window_close_ms < b.window_close_ms;
        });
        for (size_t i = 0; i < v.size(); ++i) {
            bool within_window_cap = i < max_windows_per_series;
            bool within_prewarm = v[i].window_open_ms > 0 &&
                v[i].window_open_ms <= now_ms + prewarm_ahead_ms;
            if (!within_window_cap && !within_prewarm) break;
            out.push_back(std::move(v[i]));
        }
    }
    // Global safety cap: keep the soonest-closing markets if somehow over budget.
    std::sort(out.begin(), out.end(), [](const PmMarket& a, const PmMarket& b) {
        return a.window_close_ms < b.window_close_ms;
    });
    if (out.size() > max_universe_markets) {
        out.resize(max_universe_markets);
    }
    return out;
}

// SharedState — single source of truth read by the TUI and engine.
// Feed callbacks and the executor write under mu.
struct SharedState {
    std::mutex mu;
    std::vector<PmMarket> markets;
    std::unordered_map<std::string, std::shared_ptr<OrderBook>> book_up;
    std::unordered_map<std::string, std::shared_ptr<OrderBook>> book_down;
    std::unordered_map<std::string, std::string> token_to_cond;
    std::unordered_map<std::string, ShareSide>   token_to_side;
    // Asset prices (Binance) — keyed by "BTC"|"ETH"|"SOL"|"XRP"
    double asset_price(const std::string& a) const { return price_map_.count(a) ? price_map_.at(a) : 0.0; }
    double& asset_price(const std::string& a) { return price_map_[a]; }
    std::vector<double> asset_recent(const std::string& a) const { return recent_map_.count(a) ? recent_map_.at(a) : std::vector<double>{}; }
    std::vector<double>& asset_recent(const std::string& a) { return recent_map_[a]; }
    // Asset prices (Coinbase) — used for cross-source comparison only.
    double cb_asset_price(const std::string& a) const { return cb_price_map_.count(a) ? cb_price_map_.at(a) : 0.0; }
    double& cb_asset_price(const std::string& a) { return cb_price_map_[a]; }
    // Oracle prices (Polymarket RTDS / Chainlink relay)
    double oracle_price(const std::string& a) const { return oracle_map_.count(a) ? oracle_map_.at(a) : 0.0; }
    double& oracle_price(const std::string& a) { return oracle_map_[a]; }
    std::unordered_map<std::string, double> price_map_;
    std::unordered_map<std::string, std::vector<double>> recent_map_;
    std::unordered_map<std::string, double> cb_price_map_;
    std::unordered_map<std::string, double> oracle_map_;
    // Active selection
    int active_index = 0;
    // Positions snapshot (updated by executor fills)
    std::vector<Position> positions_snap;
    double deployed_usd = 0;
    double net_pnl_usd  = 0;    // realized - fees (updated after each fill)
    int    trades_won   = 0;
    int    trades_lost  = 0;
    // Settled trade results (newest first, max 12). Populated by the
    // paper-settle loop. Survives the position disappearing so the user
    // can always see "what just happened" on the dashboard.
    struct RecentResult {
        int64_t     ts_ms = 0;
        std::string asset;          // BTC / ETH
        ShareSide   side = ShareSide::UP;
        int         horizon_s = 0;
        double      shares = 0;
        double      entry_price = 0;
        double      settle_price = 0;
        double      pnl_usd = 0;    // (settle - entry) * shares
        bool        win = false;
    };
    std::vector<RecentResult> recent_results;
    // Signal history (newest first, max 15 lines)
    std::vector<std::string> signal_history;
    // LLM advisory text for active market
    std::string llm_advice;
    // ── AI Trader's Brief (periodic LLM session commentary) ──
    // Refreshed every few minutes by a background thread; rendered in a
    // dedicated panel under the prediction engine. Distinct from
    // llm_advice (which is keypress-triggered per-market).
    std::string trader_brief;
    int64_t     trader_brief_ts_ms = 0;
    std::atomic<bool> trader_brief_pending{false};
    // Kill switch + autopilot
    std::atomic<bool> kill_switch{false};
    std::atomic<bool> autopilot{false};
    std::atomic<bool> daily_kill_fired{false};  // reset via X key
    // Disk space monitoring
    std::atomic<bool> disk_full{false};         // true when disk critically low
    // Counters for autopilot throttle
    int64_t last_autopilot_order_ms = 0;
    int consecutive_losses = 0;
    int64_t pause_until_ms = 0;
    // Startup timestamp — enforce delay before first autopilot trade
    int64_t startup_ts_ms = 0;
    int64_t last_startup_log_ms = 0;
    // Mode label
    std::string mode_label;
    // Wallet baseline used for display math (cash_avail = baseline + net_pnl - open_cost).
    // Paper: 100.00. Live: actual on-chain USDC.e at startup
    // (set from cfg.wallet.starting_balance_usd).
    double wallet_baseline_usd = 100.00;
    // Holding reward yield — 4% APY on idle USDC, applied continuously in paper mode.
    // Idle cash = wallet_baseline_usd + net_pnl_usd - deployed_usd.
    // Yield accrues to net_pnl_usd so it appears as "free money" in the P&L.
    double yield_accrued_usd = 0.0;
    int64_t last_yield_ts_ms = 0;
    // Runtime risk caps (after clamp). Used by TUI for display.
    double runtime_max_deployed_usd = 80.00;
    // Open prices: first book tick seen for each market
    std::unordered_map<std::string, double> market_open_up;
    std::unordered_map<std::string, double> market_open_down;
    // UP price tick history: last 60 ticks (200ms cadence = 12s) per market
    std::unordered_map<std::string, std::deque<std::pair<int64_t,double>>> up_price_hist;
    // DOWN price tick history (symmetric with up_price_hist)
    std::unordered_map<std::string, std::deque<std::pair<int64_t,double>>> down_price_hist;
    // Entry counter per window to prevent runaway buys (reset on each new window)
    std::unordered_map<std::string, int> entries_per_window;
    std::unordered_map<std::string, int64_t> entries_per_window_reset; // window_close_ms snapshot
    // Latest predictor read per market (condition_id -> {pred_up, pred_conf}).
    // Written in the refresh loop after predict(); read by the momentum-flip
    // exit sweep so it can act on the model turning against an open position.
    std::unordered_map<std::string, std::pair<double,double>> latest_pred;
    // Full predictor-output cache per market (condition_id -> {output, ts_ms}).
    // Throttles the expensive ensemble: the sweep recomputes a market only when
    // its cache is older than predictor_refresh_ms AND a per-sweep budget remains
    // (see cfg.strategy.predict_budget_per_sweep). Prevents the global-lock
    // starvation that froze on_book / the orderbook. Price-sensitive edges are
    // recomputed each sweep against the live mid, so entries stay responsive.
    struct CachedPred { engine::PredictorOutput out; int64_t ts_ms = 0; };
    std::unordered_map<std::string, CachedPred> pred_cache;
    // ── Confidence trajectory for ADAPTIVE mode ──────────────────────
    // Per-market rolling samples of (ts_ms, confidence). Used to detect
    // rising confidence and determine the optimal entry time.
    struct ConfSample { int64_t ts_ms; double conf; };
    struct ConfidenceTrajectory {
        std::deque<ConfSample> samples;
        int64_t window_open_ms = 0;
        int64_t window_close_ms = 0;
        double low_conf = 1.0;
        int64_t low_conf_ts_ms = 0;
        double current_slope = 0.0;       // conf/sec
        bool entry_ready = false;
        int64_t entry_countdown_s = -1;   // -1 = not yet crossed
        void reset(int64_t open_ms, int64_t close_ms) {
            samples.clear();
            window_open_ms = open_ms;
            window_close_ms = close_ms;
            low_conf = 1.0;
            low_conf_ts_ms = open_ms;
            current_slope = 0.0;
            entry_ready = false;
            entry_countdown_s = -1;
        }
        void push(int64_t ts_ms, double conf) {
            samples.push_back({ts_ms, conf});
            if (conf < low_conf) {
                low_conf = conf;
                low_conf_ts_ms = ts_ms;
            }
        }
        void update_slope(int64_t now_ms, int64_t trajectory_window_ms) {
            if (samples.size() < 2) {
                current_slope = 0.0;
                return;
            }
            // Find the sample at the start of the trajectory window
            auto cutoff = now_ms - trajectory_window_ms;
            auto it = samples.begin();
            while (it != samples.end() && it->ts_ms < cutoff) ++it;
            if (it == samples.end() || std::next(it) == samples.end()) {
                current_slope = 0.0;
                return;
            }
            double dt = (samples.back().ts_ms - it->ts_ms) / 1000.0;
            if (dt <= 0.0) {
                current_slope = 0.0;
                return;
            }
            double dconf = samples.back().conf - it->conf;
            current_slope = dconf / dt; // conf per second
        }
    };
    std::unordered_map<std::string, ConfidenceTrajectory> conf_trajectories;
    // Active condition id — set by rebuild_snapshot from filtered watchlist
    std::string active_cond_id;
    // Per-token last book update timestamps (for staleness detection).
    // Updated inside BookUpdate callback under state.mu.
    std::unordered_map<std::string, int64_t> last_book_update_ms;
    // Tokens whose pushed best_bid_ask disagreed with our locally-built ladder
    // (>= ~1.5 ticks). The REST seeder drains this set and re-snapshots exactly
    // those books — targeted desync recovery (the permanent stale-book fix).
    std::unordered_set<std::string> bbo_desync_tokens;
    // Trend filter: tracks which side has won recent settlements.
    // +1 = UP won, -1 = DOWN won. Used to penalize the predictor
    // edge on the losing side so we don't keep buying a reversing trend.
    std::deque<int> settlement_trend;  // newest first, +1=UP win, -1=DOWN win
    // Stale-warn rate limiter: maps token_id -> last warn timestamp (used outside lock)
    // Accessed only by the refresh thread under state.mu.
    std::unordered_map<std::string, int64_t> last_stale_warn_ms;
    // Aggregated health log timestamp (refresh thread only).
    int64_t last_ob_health_log_ms = 0;
    // ── Binance bookTicker (Phase 6.3): sub-100ms top-of-book per asset ──
    // Written by the Binance WS callback; read by row builders (refresh +
    // autopilot sweeps) under state.mu. Feeds the book-level confirmation
    // gate on oracle-lag entries.
    struct BtSnapshot {
        double bid = 0, bid_q = 0, ask = 0, ask_q = 0;
        int64_t ts_ms = 0;
        std::deque<std::pair<int64_t, double>> mid_hist;  // (ts_ms, mid), capped
    };
    std::unordered_map<std::string, BtSnapshot> book_ticker;  // key = "BTC"/"ETH"
    // ── Edge-persistence tracking (Phase 5.6) ──
    // key = cond_id|side → {first_seen_ms, last_seen_ms}. Updated on EVERY
    // sweep sighting of a signal (outside the gate pipeline so short-circuit
    // on earlier gates cannot starve the anti-flicker tracking).
    std::unordered_map<std::string, std::pair<int64_t, int64_t>> signal_persist;
    // ── Futures liquidation pressure (Phase 4.1) ──
    // (ts_ms, signed_notional_usd): SELL-liquidations negative (price down),
    // BUY-liquidations positive (price up). 30s rolling net = row.liq_pressure_usd.
    std::unordered_map<std::string, std::deque<std::pair<int64_t, double>>> liq_events;
};

// Book lookup helper — must be called under state.mu.
static const OrderBook* book_lookup_locked(const SharedState& st,
                                            const std::string& cond_id,
                                            ShareSide side) {
    if (side == ShareSide::UP) {
        auto it = st.book_up.find(cond_id);
        if (it != st.book_up.end() && it->second) return it->second.get();
    } else {
        auto it = st.book_down.find(cond_id);
        if (it != st.book_down.end() && it->second) return it->second.get();
    }
    return nullptr;
}

static void rebuild_snapshot(SharedState& st,
                              const StrategyHot& strat,
                              const PolymarketConfig& pm_cfg,
                              vp::core::OracleBuffer& oracle_buf,
                              vp::core::OracleBuffer& cb_buf,
                              vp::engine::CandleStore& candles,
                              vp::engine::TradeFlowBuffer& trade_flow,
                              vp::engine::PmFlowBuffer& pm_flow,
                              vp::engine::TickBuffer& tick_buf,
                              TuiSnapshot& out) {
    out.top.mode_label       = st.mode_label;
    out.top.equity_usd       = st.deployed_usd;
    out.top.day_pnl_usd      = st.net_pnl_usd;
    // Wallet baseline = configured starting bankroll (paper: $100; live:
    // actual on-chain USDC.e at startup, configured via [wallet]
    // starting_balance_usd in TOML or set from secrets at boot).
    out.top.wallet_start_usd = st.wallet_baseline_usd;
    out.top.autopilot_global = st.autopilot.load();
    out.top.kill_switch      = st.kill_switch.load();
    out.top.disk_full        = st.disk_full.load();
    out.top.trades_won       = st.trades_won;
    out.top.trades_lost      = st.trades_lost;

    // ── Wallet strip math ───────────────────────────────────────────
    //   total_invested_cost = Σ open.shares × open.avg_price  (== state.deployed_usd)
    //   cash_available      = wallet_start + net_pnl − total_invested_cost
    //   wallet_total_if_wins = cash_available + Σ open.shares × $1.00
    //   wallet_cash_value    = cash_available + Σ open.shares × $0.00
    {
        double open_shares = 0.0;
        double open_cost   = 0.0;
        for (const auto& p : st.positions_snap) {
            if (p.shares <= strat.dust_position_shares) continue;
            open_shares += p.shares;
            open_cost   += p.shares * p.avg_price;
        }
        out.top.total_invested_cost  = open_cost;
        double cash_avail            = st.wallet_baseline_usd
                                       + st.net_pnl_usd
                                       - open_cost;
        out.top.wallet_cash_value    = cash_avail;
        out.top.wallet_total_if_wins = cash_avail + open_shares * 1.00;
    }

    int64_t now_ms = Clock::wall_ns() / 1'000'000LL;

    out.watchlist.clear();
    out.watchlist.reserve(st.markets.size());
    const int64_t closed_display_grace_ms =
        std::max(0, pm_cfg.closed_market_display_grace_ms);
    for (const auto& m : st.markets) {
        // Only show markets in the TUI that have already opened; future markets have no alpha
        if (m.window_open_ms <= 0 || m.window_open_ms > now_ms) continue;
        if (m.window_close_ms > 0 &&
            now_ms > m.window_close_ms + closed_display_grace_ms) continue;
        WatchlistRow row;
        row.condition_id    = m.condition_id;
        row.asset           = m.asset;
        row.horizon_s       = m.horizon_seconds;
        row.window_close_ms = m.window_close_ms;
        row.target_price    = m.target_price;
        row.asset_price     = st.asset_price(m.asset);
        // oracle_delta_pct = (oracle_now - oracle_at_window_open) / oracle_at_window_open * 100
        // This is the true oracle-lag signal: how far has the Chainlink oracle moved
        // since the 5/15-min window opened? The CLOB takes ~55s to reprice; we front-run it.
        {
            double window_open_ts_s = (double)m.window_open_ms / 1000.0;
            double oracle_now      = oracle_buf.latest(m.reference_key());
            double oracle_at_open  = oracle_buf.lookup_at_or_before(m.reference_key(), window_open_ts_s);
            // Fallback: if no tick before window open, use the earliest tick available.
            // This handles the common case where the RTDS connects after the window opened.
            if (oracle_at_open <= 0)
                oracle_at_open = 0; // wait for the actual opening reference
            // ── COIN price (Polymarket-truth, not Binance/Coinbase) ──
            //   open = strike; now  = live. These drive the watchlist
            //   "COIN-OPEN / COIN-NOW" columns and the wallet-strip
            //   "WINDOW COIN OPEN PRICE (TO BEAT)" field.
            row.coin_open_price = oracle_at_open;   // 0 if not seeded yet
            row.coin_now_price  = oracle_now;       // 0 if no tick yet
            if (oracle_at_open > 0 && oracle_now > 0)
                row.oracle_delta_pct = 100.0 * (oracle_now - oracle_at_open) / oracle_at_open;
            else
                row.oracle_delta_pct = 0.0; // no oracle history yet for this window
            // ── Coinbase-lead delta: same window-open-anchored move, computed
            // from the Coinbase WS ticker. Coinbase LEADS the oracle by ~1.5-3s
            // (Chainlink aggregates USD venues, Coinbase dominant), so this is
            // the oracle's NEXT print — the earliest tradeable signal.
            // IMPORTANT: anchor to the ORACLE's window-open price when we have
            // it, so the two deltas are directly comparable (the small static
            // Coinbase/Chainlink basis cancels out of the MOVE either way, but
            // a same-source anchor avoids a spurious offset right at open).
            {
                double cb_now = cb_buf.latest(m.asset);
                double cb_at_open = cb_buf.lookup_at_or_before(m.asset, window_open_ts_s);

            if (cb_at_open > 0 && cb_now > 0)
                row.cb_delta_pct = 100.0 * (cb_now - cb_at_open) / cb_at_open;
            else
                row.cb_delta_pct = 0.0;
            // ── Binance bookTicker confirmation fields (Phase 6.3) ──
            if (auto bti = st.book_ticker.find(m.asset); bti != st.book_ticker.end()) {
                const auto& bt = bti->second;
                if (bt.mid_hist.size() >= 2) {
                    const auto& back = bt.mid_hist.back();
                    for (auto rit = bt.mid_hist.rbegin(); rit != bt.mid_hist.rend(); ++rit) {
                        if (back.first - rit->first >= 2000) {
                            if (rit->second > 0)
                                row.bt_mid_delta_pct =
                                    100.0 * (back.second - rit->second) / rit->second;
                            break;
                        }
                    }
                }
                double sz = bt.bid_q + bt.ask_q;
                if (sz > 0 && bt.bid > 0 && bt.ask > 0)
                    row.bt_imbalance = (bt.bid_q - bt.ask_q) / sz;
            }
            // ── Oracle micro-dynamics for digital FV (Phase 5.1) ──
            // ── Futures liquidation pressure: 30s rolling net notional (Phase 4.1) ──
            if (auto lit = st.liq_events.find(m.asset); lit != st.liq_events.end()) {
                double net = 0.0;
                for (auto rit = lit->second.rbegin(); rit != lit->second.rend(); ++rit) {
                    if (now_ms - rit->first > 30'000) break;
                    net += rit->second;
                }
                row.liq_pressure_usd = net;
            }
            } // end Coinbase-lead block
        } // end oracle/bookTicker block
        if (auto it = st.book_up.find(m.condition_id);
            it != st.book_up.end() && it->second) {
            auto bb = it->second->best_bid(), ba = it->second->best_ask();
            row.up_bid = bb.price;
            row.up_ask = ba.price;
            row.up_price = (bb.price + ba.price) > 0
                ? 0.5 * (bb.price + ba.price)
                : (bb.price > 0 ? bb.price : ba.price);
            row.up_imbalance = it->second->imbalance_top1();
        }
        if (auto it = st.book_down.find(m.condition_id);
            it != st.book_down.end() && it->second) {
            auto bb = it->second->best_bid(), ba = it->second->best_ask();
            row.down_bid = bb.price;
            row.down_ask = ba.price;
            row.down_price = (bb.price + ba.price) > 0
                ? 0.5 * (bb.price + ba.price)
                : (bb.price > 0 ? bb.price : ba.price);
            row.down_imbalance = it->second->imbalance_top1();
        }

        // Open price: latch on first book tick seen
        {
            auto& ou = st.market_open_up[m.condition_id];
            if (ou == 0.0 && row.up_price > 0) ou = row.up_price;
            row.open_up_price = ou;
            auto& od = st.market_open_down[m.condition_id];
            if (od == 0.0 && row.down_price > 0) od = row.down_price;
            row.open_down_price = od;
        }

        // UP price history — append current tick, keep last 60 entries (~12s)
        {
            auto& hist = st.up_price_hist[m.condition_id];
            if (row.up_price > 0) {
                hist.push_back({now_ms, row.up_price});
                while ((int)hist.size() > std::max(1, strat.price_history_max_points))
                    hist.pop_front();
            }
            // Velocity: change over configured lookback.
            if (hist.size() >= 2) {
                const auto& back = hist.back();
                for (auto rit = hist.rbegin(); rit != hist.rend(); ++rit) {
                    if (back.first - rit->first >=
                            std::max(1, strat.price_velocity_lookback_ms)) {
                        double dt = (back.first - rit->first) / 1000.0;
                        row.up_velocity = (back.second - rit->second) / dt;
                        break;
                    }
                }
            }
            // DOWN price history — symmetric velocity computation
            {
                auto& dhist = st.down_price_hist[m.condition_id];
                if (row.down_price > 0) {
                    dhist.push_back({now_ms, row.down_price});
                    while ((int)dhist.size() > std::max(1, strat.price_history_max_points))
                        dhist.pop_front();
                }
                if (dhist.size() >= 2) {
                    const auto& back = dhist.back();
                    for (auto rit = dhist.rbegin(); rit != dhist.rend(); ++rit) {
                        if (back.first - rit->first >=
                                std::max(1, strat.price_velocity_lookback_ms)) {
                            double dt = (back.first - rit->first) / 1000.0;
                            row.down_velocity = (back.second - rit->second) / dt;
                            break;
                        }
                    }
                }
            }
            // Last 30 prices for sparkline
            int nh = std::min<int>(std::max(1, strat.sparkline_points), (int)hist.size());
            row.up_price_hist.resize(nh);
            for (int hi = 0; hi < nh; ++hi)
                row.up_price_hist[hi] = hist[hist.size() - nh + hi].second;
        }

        // Feed TickBuffer with current mid-prices (so velocity data is available
        // even between Polymarket MD WebSocket updates)
        {
            int64_t now_us = now_ms * 1000LL;
            if (row.up_price > 0)
                tick_buf.on_price(m.condition_id, "UP", row.up_price, now_us);
            if (row.down_price > 0)
                tick_buf.on_price(m.condition_id, "DOWN", row.down_price, now_us);
        }

        // Rule engine for signal glyph. Skip signal generation if the book is
        // older than the configured display/signal stale threshold.
        bool book_stale = false;
        {
            const int64_t stale_ms = std::max(1, pm_cfg.book_health_stale_ms);
            auto check_tok = [&](const std::string& tok_id) -> bool {
                auto it = st.last_book_update_ms.find(tok_id);
                if (it == st.last_book_update_ms.end()) {
                    auto& last = st.last_stale_warn_ms[tok_id];
                    if (now_ms - last > stale_ms) {
                        spdlog::warn("orderbook never updated: token={} market={}",
                                     tok_id, m.condition_id);
                        last = now_ms;
                    }
                    return true;
                }
                int64_t age_ms = now_ms - it->second;
                if (age_ms > stale_ms) {
                    auto& last = st.last_stale_warn_ms[tok_id];
                    if (now_ms - last > stale_ms) {
                        spdlog::warn("orderbook stale: token={} age={}ms market={}",
                                     tok_id, age_ms, m.condition_id);
                        last = now_ms;
                    }
                    return true;
                }
                return false;
            };
            book_stale = check_tok(m.yes_token_id) || check_tok(m.no_token_id);
        }
        if (book_stale) {
            row.signal_glyph = 'S';  // S = stale
        } else {
            vp::engine::populate_reference_forecasts(row,m,oracle_buf,cb_buf,candles,now_ms);
            auto sig = rules::evaluate(row, strat, now_ms);
            if (sig) {
            row.signal_glyph = sig->glyph;
            spdlog::debug("signal: {} {} {} conf={:.2f} delta={:+.3f}% {}",
                row.asset, row.horizon_s == 300 ? "5m" : "15m",
                sig->glyph == '+' ? "BUY_UP" : "BUY_DN",
                sig->confidence, row.oracle_delta_pct, sig->reason);
        } else {
                row.signal_glyph = (row.up_price > strat.hard_share_price_ceil) ? 'X' : '-';
            }
        } // end book_stale check

        // ── Multi-signal predictor (Black-Scholes + technicals + microstructure) ──
        // Cheap (≈ O(closes_window)); runs every 200ms per market on the
        // refresh thread. Components feed both the watchlist row glyph
        // colouring and the active-panel breakdown.
        {
            vp::engine::PredictorInput pin;
            pin.asset       = row.asset;
            pin.horizon_s   = row.horizon_s;
            pin.secs_left   = (row.window_close_ms - now_ms) / 1000;
            pin.asset_price = row.asset_price;
            pin.reference_price = oracle_buf.latest(m.asset + ":SPOT");
            pin.reference_age_s = now_ms / 1000.0 - oracle_buf.latest_timestamp(m.asset + ":SPOT");
            pin.settlement_window_s = m.settlement_window_s;
            pin.fee_rate = strat.taker_fee_rate;
            pin.fee_exponent = strat.fee_exponent;
            // Use the real oracle price at window open as the strike
            double window_open_ts_s = (double)m.window_open_ms / 1000.0;
            pin.strike      = oracle_buf.lookup_at_or_before(m.reference_key(), window_open_ts_s);
            if (pin.strike <= 0) pin.strike = row.target_price;  // fallback
            pin.coinbase_price = st.cb_asset_price(row.asset);
            pin.up_mid    = row.up_price;
            pin.down_mid  = row.down_price;
            // Top-of-book sizes for microstructure imbalance (both sides)
            if (auto it = st.book_up.find(m.condition_id);
                it != st.book_up.end() && it->second) {
                auto bb = it->second->best_bid(), ba = it->second->best_ask();
                pin.up_bid    = bb.price; pin.up_bid_sz = bb.size;
                pin.up_ask    = ba.price; pin.up_ask_sz = ba.size;
            }
            if (auto it = st.book_down.find(m.condition_id);
                it != st.book_down.end() && it->second) {
                auto bb = it->second->best_bid(), ba = it->second->best_ask();
                pin.down_bid    = bb.price; pin.down_bid_sz = bb.size;
                pin.down_ask    = ba.price; pin.down_ask_sz = ba.size;
            }
            // Oracle velocity / acceleration (from OracleBuffer)
            pin.oracle_vel_1m_pct = oracle_buf.velocity_roc_pct(m.reference_key(), 60);
            pin.oracle_vel_5m_pct = oracle_buf.velocity_roc_pct(m.reference_key(), 300);
            pin.oracle_accel_pct  = oracle_buf.acceleration_pct(m.reference_key(), 60);
            // Trade flow bias from Binance aggressor tracking
            pin.trade_flow_bias = trade_flow.buy_sell_imbalance(vp::engine::asset_to_symbol(row.asset), 5);
            // PM token trade-flow imbalance (tick-rule classified)
            pin.pm_flow_bias    = pm_flow.imbalance_secs(m.condition_id, 10);
            pin.pm_flow_enabled = strat.window_predictor_pm_flow;
            // Cross-asset 5m momentum (ETH↔BTC reference drift)
            {
                const char* other = (row.asset == "BTC") ? "ETH" : "BTC";
                auto oc = candles.copy_recent_closes(other, engine::Timeframe::OneMinute, 6);
                if (oc.size() >= 6 && oc[oc.size() - 6] > 0)
                    pin.other_mom_5m_pct =
                        100.0 * (oc.back() - oc[oc.size() - 6]) / oc[oc.size() - 6];
            }
            // Sub-second tick momentum from TickBuffer
            pin.tick_velocity_1s = tick_buf.velocity_us(m.condition_id, "UP");
            pin.tick_velocity_3s = tick_buf.velocity_us(m.condition_id, "UP", 3'000'000);
            // Feed recent 1m data for fast intraday indicators plus long 60m
            // history for multi-year volatility/trend regime features.
            pin.closes_1m = candles.copy_recent_closes(row.asset, engine::Timeframe::OneMinute,
                strat.predictor_recent_1m_bars);
            pin.bars_1m   = candles.copy_recent_bars(row.asset, engine::Timeframe::OneMinute,
                strat.predictor_recent_1m_ohlcv_bars);
            // Higher timeframe data for multi-timeframe analysis
            pin.closes_5m  = candles.copy_recent_closes(row.asset, engine::Timeframe::FiveMinutes, strat.predictor_5m_closes);
            pin.closes_15m = candles.copy_recent_closes(row.asset, engine::Timeframe::FifteenMinutes, strat.predictor_15m_closes);
            pin.closes_60m = candles.copy_recent_closes(row.asset, engine::Timeframe::SixtyMinutes,
                strat.predictor_long_60m_bars);
            pin.bars_5m    = candles.copy_recent_bars(row.asset, engine::Timeframe::FiveMinutes, strat.predictor_5m_bars);
            pin.bars_15m   = candles.copy_recent_bars(row.asset, engine::Timeframe::FifteenMinutes, strat.predictor_15m_bars);
            pin.bars_60m   = candles.copy_recent_bars(row.asset, engine::Timeframe::SixtyMinutes, strat.predictor_60m_bars);
            if (!pin.bars_1m.empty()) pin.candle_age_s = (now_ms - pin.bars_1m.back().ts_ms - 60000) / 1000.0;
            auto pred = vp::engine::predict(pin);
            row.pred_up        = pred.prediction;
            row.pred_fair_up   = pred.fair_value_up;
            row.pred_edge_up   = pred.edge_up;
            row.pred_edge_down = pred.edge_down;
            row.pred_conf      = pred.confidence;
            row.pred_rsi14     = pred.parts.rsi14;
            row.pred_macd_hist = pred.parts.macd_hist;
            row.pred_bb_z      = pred.parts.bb_z;
            row.pred_tech_score= pred.parts.tech_score;
            row.pred_oracle_vel_1m = pred.parts.oracle_vel_1m_pct;
            row.pred_oracle_accel  = pred.parts.oracle_accel_pct;
            row.pred_volume_surge_ratio = pred.parts.volume_surge_ratio;
            row.pred_candle_body   = pred.parts.candle_body_conviction;
            row.pred_trade_flow    = pred.parts.trade_flow_bias;
            row.pred_rv_30d        = pred.parts.rv_30d;
            row.pred_rv_90d        = pred.parts.rv_90d;
            row.pred_rv_1y         = pred.parts.rv_1y;
            row.pred_slope_7d_pct  = pred.parts.slope_7d_pct;
            row.pred_slope_30d_pct = pred.parts.slope_30d_pct;
            row.pred_slope_1y_pct  = pred.parts.slope_1y_pct;
            row.pred_history_regime_score = pred.parts.history_regime_score;
            row.pred_signal_strength_rsi    = pred.parts.signal_strength_rsi;
            row.pred_signal_strength_macd   = pred.parts.signal_strength_macd;
            row.pred_signal_strength_bb     = pred.parts.signal_strength_bb;
            row.pred_signal_strength_slope  = pred.parts.signal_strength_slope;
            row.pred_signal_strength_micro  = pred.parts.signal_strength_micro;
            row.pred_signal_strength_oracle = pred.parts.signal_strength_oracle;
            row.pred_signal_strength_trade  = pred.parts.signal_strength_trade;
            row.pred_signal_strength_tick   = pred.parts.signal_strength_tick;
            row.pred_signal_strength_pmflow = pred.parts.signal_strength_pmflow;
            row.pred_signal_strength_cb     = pred.parts.signal_strength_cb;
            row.pred_signal_strength_xmom   = pred.parts.signal_strength_xmom;
            row.pred_signal_strength_vol    = pred.parts.signal_strength_vol;
            row.pred_signal_strength_body   = pred.parts.signal_strength_body;
            row.pred_signal_strength_regime = pred.parts.signal_strength_regime;
            row.pred_signals_agreeing       = pred.parts.signals_agreeing;
            row.pred_signals_total          = pred.parts.signals_total;
            row.pred_stoch_k = pred.parts.stoch_k;
            row.pred_stoch_d = pred.parts.stoch_d;
            row.pred_adx     = pred.parts.adx;
            row.pred_pdi     = pred.parts.pdi;
            row.pred_mdi     = pred.parts.mdi;
            row.pred_cci     = pred.parts.cci;
            // ── Adaptive confidence trajectory tracking ─────────────────
            if (strat.adaptive_mode) {
                auto& traj = st.conf_trajectories[m.condition_id];
                if (traj.window_open_ms != m.window_open_ms || traj.window_close_ms != m.window_close_ms) {
                    traj.reset(m.window_open_ms, m.window_close_ms);
                }
                traj.push(now_ms, pred.confidence);
                traj.update_slope(now_ms, strat.adaptive_trajectory_window_ms);
                // Check adaptive entry readiness
                double slope = traj.current_slope;
                double conf_rise = pred.confidence - traj.low_conf;
                bool enough_samples = (int)traj.samples.size() >= strat.adaptive_trajectory_samples;
                bool target_hit = pred.confidence >= strat.adaptive_target_conf;
                bool rising = slope > strat.adaptive_slope_threshold;
                bool enough_rise = conf_rise >= strat.adaptive_conf_trend_min;
                int64_t secs_left = (m.window_close_ms - now_ms) / 1000;
                bool in_time_window = secs_left >= strat.adaptive_min_secs_left &&
                                      secs_left <= strat.adaptive_max_secs_left;
                traj.entry_ready = enough_samples && target_hit && rising && enough_rise && in_time_window;
                traj.entry_countdown_s = target_hit ? secs_left : -1;
                // Attach trajectory info to row for rule engine
                row.pred_conf_trajectory_ready = traj.entry_ready;
                row.pred_conf_trajectory_slope = slope;
                row.pred_conf_trajectory_samples = (int)traj.samples.size();
                row.pred_conf_low = traj.low_conf;
                row.pred_adaptive_countdown_s = traj.entry_countdown_s;
            }
        }

        out.watchlist.push_back(std::move(row));
    }

    // Active market panel
    out.active = {};
    out.selected_condition_id.clear();
    if (!out.watchlist.empty()) {
        int idx = std::clamp(st.active_index, 0, (int)out.watchlist.size() - 1);
        const auto& wr = out.watchlist[idx];
        // Update active_cond_id so key callback can find the right market
        st.active_cond_id = wr.condition_id;
        out.selected_condition_id = wr.condition_id;

        out.active.condition_id    = wr.condition_id;
        out.active.asset           = wr.asset;
        out.active.horizon_s       = wr.horizon_s;
        out.active.up_price        = wr.up_price;
        out.active.down_price      = wr.down_price;
        out.active.asset_price     = wr.asset_price;
        // PM STRIKE = oracle dollar price at window open (the value the
        // outcome is decided against). Polymarket BTC/ETH UP-DOWN questions
        // don't carry a $NNNN strike in the question text, so the parsed
        // target_price is always 0 — use coin_open_price instead.
        out.active.target_price    = wr.coin_open_price;
        out.active.window_close_ms = wr.window_close_ms;
        out.active.gap_usd         = wr.gap_usd;
        out.active.gap_pct         = wr.gap_pct;
        out.active.asset_recent    = st.asset_recent(wr.asset);
        out.active.open_up_price   = wr.open_up_price;
        out.active.open_down_price = wr.open_down_price;
        out.active.up_velocity     = wr.up_velocity;
        out.active.up_price_hist   = wr.up_price_hist;

        // Cross-source live prices for comparison panel
        out.active.coinbase_price  = st.cb_asset_price(wr.asset);
        out.active.pm_open_price   = wr.coin_open_price;  // oracle@open, not parsed text
        out.active.oracle_price    = wr.coin_now_price;   // current Chainlink RTDS

        // ── Wallet-strip cross-source row (active market only) ──
        // pm_coin_price MUST be the polymarket.com Chainlink oracle —
        // never substitute Binance/Coinbase. 0.0 = "not ticked yet" →
        // renderer prints "—".
        out.top.active_asset           = wr.asset;
        out.top.pm_coin_price          = wr.coin_now_price;       // oracle.latest()
        out.top.binance_coin_price     = st.asset_price(wr.asset);
        out.top.coinbase_coin_price    = out.active.coinbase_price;
        // WINDOW COIN OPEN PRICE = oracle price at window_open_ms (the
        // strike that determines who wins). NOT the parsed-from-text
        // target_price (Polymarket UP/DOWN questions don't include a $
        // strike — the price at open IS the strike).
        out.top.window_open_coin_price = wr.coin_open_price;

        // Live UP/DOWN share asks (fallback to mid). These mirror what
        // a buy click would actually pay right now.
        if (auto it = st.book_up.find(wr.condition_id);
            it != st.book_up.end() && it->second) {
            auto ba = it->second->best_ask();
            out.top.up_share_cost_live = (ba.price > 0) ? ba.price : wr.up_price;
        } else {
            out.top.up_share_cost_live = wr.up_price;
        }
        if (auto it = st.book_down.find(wr.condition_id);
            it != st.book_down.end() && it->second) {
            auto ba = it->second->best_ask();
            out.top.down_share_cost_live = (ba.price > 0) ? ba.price : wr.down_price;
        } else {
            out.top.down_share_cost_live = wr.down_price;
        }

        // Prediction breakdown (carry over from watchlist row)
        out.active.pred_up         = wr.pred_up;
        out.active.pred_fair_up    = wr.pred_fair_up;
        out.active.pred_edge_up    = wr.pred_edge_up;
        out.active.pred_edge_down  = wr.pred_edge_down;
        out.active.pred_conf       = wr.pred_conf;
        out.active.pred_rsi14      = wr.pred_rsi14;
        out.active.pred_macd_hist  = wr.pred_macd_hist;
        out.active.pred_bb_z       = wr.pred_bb_z;
        out.active.pred_tech_score      = wr.pred_tech_score;
        out.active.pred_oracle_vel_1m   = wr.pred_oracle_vel_1m;
        out.active.pred_oracle_accel    = wr.pred_oracle_accel;
        out.active.pred_volume_surge_ratio = wr.pred_volume_surge_ratio;
        out.active.pred_candle_body     = wr.pred_candle_body;
        out.active.pred_trade_flow      = wr.pred_trade_flow;
        out.active.pred_summary         = fmt::format(
            "P={:.0f}% FV={:.0f}% edge={:+.2f}/{:+.2f} RSI={:.0f} MACDh={:+.4f} BBz={:+.2f} tech={:+.2f} hist={:+.2f}",
            wr.pred_up*100, wr.pred_fair_up*100, wr.pred_edge_up, wr.pred_edge_down,
            wr.pred_rsi14, wr.pred_macd_hist, wr.pred_bb_z, wr.pred_tech_score,
            wr.pred_history_regime_score);

        if (auto it = st.book_up.find(wr.condition_id);
            it != st.book_up.end() && it->second) {
            const auto& b = *it->second;
            auto up_bids_copy = b.copy_bids();
            auto up_asks_copy = b.copy_asks();
            int n = std::min<int>(10, (int)up_bids_copy.size());
            int m = std::min<int>(10, (int)up_asks_copy.size());
            out.active.up_bids.assign(up_bids_copy.begin(), up_bids_copy.begin() + n);
            out.active.up_asks.assign(up_asks_copy.begin(), up_asks_copy.begin() + m);
        }

        // Signal explanation
        auto sig = rules::evaluate(wr, strat, now_ms);
        if (sig && (sig->glyph == '+' || sig->glyph == 'v')) {
            std::string dir = (sig->side == ShareSide::UP) ? "BUY UP" : "BUY DOWN";
            out.active.decision_text = fmt::format("{} (conf {:.0f}%)", dir,
                                                    sig->confidence * 100);
            out.active.reason_top1 = sig->reason;
            out.active.reason_top2 = fmt::format("rec size ${:.2f}", sig->rec_size_usd);
            out.active.reason_top3 = fmt::format("limit {:.3f}", sig->rec_limit_price);
        } else if (wr.up_price > strat.hard_share_price_ceil) {
            out.active.decision_text = "AVOID - price above configured cap";
            out.active.reason_top1 = fmt::format("up={:.3f} > {:.2f}", wr.up_price,
                                                  strat.hard_share_price_ceil);
        } else {
            out.active.decision_text = "WAIT — conditions not met";
            if (wr.up_price > 0)
                out.active.reason_top1 = fmt::format("up={:.3f}", wr.up_price);
            if (wr.gap_usd != 0)
                out.active.reason_top2 = fmt::format("gap=${:+.2f}", wr.gap_usd);
        }

        // LLM advice (if any)
        if (!st.llm_advice.empty())
            out.active.decision_text = "[LLM] " + st.llm_advice;
    }

    // Position panel
    out.deployed_usd     = st.deployed_usd;
    out.max_deployed_usd = st.runtime_max_deployed_usd;
    out.position_lines   = {};
    for (const auto& p : st.positions_snap) {
        if (p.shares <= strat.dust_position_shares) continue;
        out.position_lines.push_back(
            fmt::format("{} {} {:>4}m  {:.4f}sh @ {:.3f}  ${:.2f}",
                p.asset, to_string(p.side), p.horizon_s / 60,
                p.shares, p.avg_price,
                p.shares * p.avg_price));
    }

    // Signal history + alerts
    out.signal_history = st.signal_history;
    // Recent settled trades — persistent W/L record for the dashboard.
    out.recent_results.clear();
    out.recent_results.reserve(st.recent_results.size());
    for (const auto& r : st.recent_results) {
        RecentResultRow row;
        row.ts_ms        = r.ts_ms;
        row.asset        = r.asset;
        row.side_label   = (r.side == ShareSide::UP) ? "UP" : "DN";
        row.horizon_s    = r.horizon_s;
        row.shares       = r.shares;
        row.entry_price  = r.entry_price;
        row.settle_price = r.settle_price;
        row.pnl_usd      = r.pnl_usd;
        row.win          = r.win;
        out.recent_results.push_back(row);
    }
    out.alerts.clear();
    if (st.kill_switch.load())
        out.alerts.push_back("!! KILL SWITCH ACTIVE — no new buys !!");
    if (st.autopilot.load())
        out.alerts.push_back(">> AUTOPILOT ON");
    // Trader's Brief (LLM session commentary)
    out.trader_brief         = st.trader_brief;
    out.trader_brief_ts_ms   = st.trader_brief_ts_ms;
    out.trader_brief_pending = st.trader_brief_pending.load();
    out.status_hint.clear();
}

} // namespace vp

// ── Oracle failover state (file scope: touched by RTDS / Coinbase-WS /
// Binance callbacks and the Coinbase REST poller, which live in different
// lexical scopes). Priority chain: Chainlink RTDS > Coinbase WS > Binance.
// Coinbase outranks Binance because Chainlink's */usd streams aggregate USD
// venues with Coinbase dominant (Binance trades the USDT pair — basis error).
namespace {
struct OracleFailover {
    std::atomic<int64_t> last_rtds_ms{0};    // last real Chainlink tick
    std::atomic<int64_t> last_cb_ws_ms{0};   // last Coinbase WS ticker tick
    std::atomic<bool>    active{false};
    std::array<std::atomic<int64_t>, 4> last_synth_ms{};   // BTC,ETH,SOL,XRP
    static int idx(const std::string& a) {
        return a == "BTC" ? 0 : a == "ETH" ? 1 : a == "SOL" ? 2 : 3;
    }
};
OracleFailover oracle_failover;
constexpr int64_t kOracleFailoverMs   = 15000;  // RTDS silence before failover engages
constexpr int64_t kCoinbaseFreshMs    = 5000;   // CB WS fresher than this outranks Binance
constexpr int64_t kFailoverSynthGapMs = 500;    // throttle synthesized ticks per asset
} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT,  on_sig);
    std::signal(SIGTERM, on_sig);

    // Args FIRST (the instance label decides the log file path).
    std::string cfg_path = "config/polypulse.toml";
    std::string instance_label;   // multi-instance: tag for logs/DB/report
    bool headless = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--config" && i + 1 < argc) {
            cfg_path = argv[++i];
        } else if (std::string(argv[i]).starts_with("--config=")) {
            cfg_path = argv[i] + 9; // skip "--config="
        } else if (std::string(argv[i]) == "--instance" && i + 1 < argc) {
            instance_label = argv[++i];
        } else if (std::string(argv[i]).starts_with("--instance=")) {
            instance_label = argv[i] + 11;
        } else if (std::string(argv[i]) == "--headless") {
            headless = true;        // no TUI: for unattended/background runs
        }
    }

    // Logging: file only (TUI owns stdout/stderr). Per-instance log file so
    // parallel strategy instances don't interleave.
    std::filesystem::create_directories("logs");
    const std::string log_path = instance_label.empty()
        ? std::string("logs/polypulse.log")
        : "logs/polypulse-" + instance_label + ".log";
    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
        log_path, false);
    file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    auto logger = std::make_shared<spdlog::logger>("vp", file_sink);
    spdlog::set_default_logger(logger);
    spdlog::flush_on(spdlog::level::info);

    vp::Config cfg;
    try { cfg = vp::Config::load(cfg_path); }
    catch (const std::exception& e) {
        std::fprintf(stderr, "config load failed (%s): %s\n", cfg_path.c_str(), e.what());
        return 2;
    }
    // Config can also set the label ([runtime] instance_label) — CLI wins.
    if (instance_label.empty() && !cfg.runtime.instance_label.empty())
        instance_label = cfg.runtime.instance_label;
    auto configure_file_logger = [&]() {
        auto rotating_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            log_path,
            std::max(1024, cfg.data_collection.log_max_bytes),
            std::max(1, cfg.data_collection.log_keep_files),
            false);
        rotating_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
        auto rotating_logger = std::make_shared<spdlog::logger>("vp", rotating_sink);
        rotating_logger->flush_on(spdlog::level::info);
        spdlog::set_default_logger(rotating_logger);
        spdlog::flush_on(spdlog::level::info);
    };
    configure_file_logger();
    spdlog::set_level(spdlog::level::from_str(cfg.runtime.log_level));
    cfg.dump_summary();

    const int64_t session_start_ms = vp::Clock::wall_ns() / 1'000'000LL;
    const std::string session_id = vp::core::make_uuid();
    spdlog::info("session: id={} instance='{}' start={} mode={}",
                 session_id, instance_label, wall_ts_pretty(session_start_ms),
                 cfg.runtime.mode == vp::TradeMode::Live ? "LIVE" : "PAPER");

    // Hard-cap validation removed — all limits are now configurable via TOML.

    // Load wallet private key from file
    std::string private_key_hex = load_private_key(cfg.wallet.private_key_file);
    if (cfg.runtime.mode == vp::TradeMode::Live &&
        (private_key_hex.empty() || cfg.wallet.api_key.empty() ||
         cfg.wallet.api_key == "PENDING" || cfg.wallet.api_secret.empty() ||
         cfg.wallet.api_passphrase.empty())) {
        spdlog::critical("LIVE startup refused: wallet signing or CLOB credentials incomplete");
        std::fprintf(stderr, "LIVE startup refused: wallet credentials incomplete\n");
        return 5;
    }
    // Load LLM API key from file (or fallback)
    std::string llm_api_key = load_file_str(cfg.llm.api_key_file);
    if (llm_api_key.empty()) llm_api_key = load_file_str("./config/llm.key");
    cfg.llm.api_key = llm_api_key;

    // Postgres
    std::unique_ptr<vp::Pg> pg;
    try { pg = std::make_unique<vp::Pg>(cfg.postgres.conninfo); }
    catch (const std::exception& e) {
        std::fprintf(stderr, "postgres connect failed: %s\n", e.what());
        return 3;
    }
    if (!pg->ping()) { std::fprintf(stderr, "postgres ping failed\n"); return 4; }
    spdlog::info("postgres OK");
    pg->set_session_context(session_id, instance_label);
    std::atomic<bool> uncertain_order_halt{
        cfg.runtime.mode == vp::TradeMode::Live &&
        pg->has_uncertain_live_order_for_instance()};
    if (uncertain_order_halt.load())
        spdlog::critical("unresolved LIVE uncertain order: autonomous trading halted pending venue reconciliation");
    // Create the vp.strategy_runs row for this session immediately so even
    // sessions that never trade are accounted for.
    pg->upsert_strategy_run(instance_label, session_id, 0, 0, 0, 0, 0.0, 0.0,
                            false);
    // Sessions killed without a clean shutdown leave their run row open;
    // stamp them closed so only the live session has ended_at IS NULL.
    pg->close_stale_strategy_runs(instance_label, session_id);

    for (const auto& sym : {"BTCUSDT", "ETHUSDT", "SOLUSDT", "XRPUSDT"}) {
        long n = pg->candles_count(sym);
        auto [lo, hi] = pg->candles_range(sym);
        spdlog::info("candles_1m[{}]: {} rows  [{} .. {}]", sym, n, lo, hi);
    }

    // Persist queue — one background thread drains it and calls pg methods.
    vp::BoundedQueue<std::function<void()>> persist_q;
    std::thread persist_th([&] {
        std::function<void()> task;
        while (!g_stop.load()) {
            if (persist_q.pop(task, 400))
                task();
        }
        // Drain remaining
        while (persist_q.pop(task, 0)) task();
    });

    vp::SharedState state;
    state.mode_label = (cfg.runtime.mode == vp::TradeMode::Live) ? "LIVE" : "PAPER";
    // Wallet baseline is set from config starting_balance_usd.
    // In live mode this is overridden with the on-chain /balance fetch below.
    // In paper mode this is the paper bankroll shown in the TUI.
    state.wallet_baseline_usd = cfg.wallet.starting_balance_usd;
    state.runtime_max_deployed_usd = cfg.risk.max_deployed_usd;
    state.autopilot.store(cfg.strategy.autopilot_on_start && !uncertain_order_halt.load());
    if (cfg.strategy.autopilot_on_start && !uncertain_order_halt.load())
        spdlog::info("autopilot: ON (autopilot_on_start=true)");

    // Time-indexed oracle price buffer — keyed by asset ("BTC"/"ETH").
    // Enables oracle_delta_pct = (oracle_now - oracle_at_window_open) / oracle_at_window_open.
    vp::core::OracleBuffer oracle_buf;
    // Coinbase price history — the LEADING signal (oracle follows it by ~1.5-3s).
    // Same structure as oracle_buf so window-open lookups work identically.
    vp::core::OracleBuffer cb_buf;

    // ── Multi-signal prediction engine: rolling 1-min OHLCV bars per asset ──
    // Bootstrapped from the 5-year history in vp.candles_1m, then fed by
    // live Binance trade ticks. Indicators (RSI/MACD/BB/ATR/VWAP/RV) and
    // the Black-Scholes binary fair value all read snapshots from here.
    vp::engine::CandleStore candle_store(
        cfg.data_collection.candle_store_max_bars_per_asset_timeframe);
    // The 5-year candle history is consumed ONLY by the multi-signal predictor
    // (and the trend filter, which reads predictor edge). When both are off, the
    // hundreds-of-MB bootstrap is pure waste — skip it so a single instance fits
    // a fraction of the footprint and 20+ can run on one box. Live Binance ticks
    // still accrue fresh bars; predict() is safe with short/empty series. If the
    // predictor is enabled later via hot-reload, the reload path bootstraps then.
    const bool candle_history_needed =
        cfg.strategy.recycler_mode!="off" || cfg.strategy.predictor_mode || cfg.strategy.trend_filter_enabled ||
        cfg.strategy.window_predictor_mode || cfg.strategy.digital_fv_mode ||
        cfg.strategy.oracle_lag_mode || cfg.strategy.endgame_mode;
    if (candle_history_needed) {
        candle_store.bootstrap_from_pg(*pg);
    } else {
        spdlog::info("candle_store: history bootstrap SKIPPED "
                     "(predictor_mode=off, trend_filter_enabled=off) — saving "
                     "memory; live bars accrue from Binance ticks. Enable "
                     "predictor_mode to load history.");
    }
    vp::engine::TradeFlowBuffer trade_flow(
        cfg.data_collection.trade_flow_max_records_per_symbol);
    vp::engine::PmFlowBuffer pm_flow;
    vp::engine::TickBuffer tick_buffer(cfg.strategy.tick_buffer_max_age_us,
                                       cfg.strategy.tick_buffer_max_entries);
    vp::engine::ScalpManager scalp_manager(tick_buffer);

    // Tracks which {asset, window_open_ms} pairs have been seeded into the oracle buffer.
    // Updated on each oracle tick; new windows that open after startup are also seeded here.
    // Accessed only from the RTDS WS callback thread — no external mutex needed.
    std::set<std::pair<std::string, int64_t>> oracle_seeded_windows;
    // Restarting mid-window can leave the in-memory buffer without that window's
    // strike. Cache one DB lookup per market (retrying missing rows once/second).
    std::map<std::string, std::pair<double, int64_t>> recycler_strikes;

    // Positions + executors
    vp::Positions positions;

    const bool paper_mode = (state.mode_label == "PAPER");
    if (paper_mode) {
        spdlog::warn("paper startup: fresh session id={} balance=${:.2f}; "
                     "historical paper DB positions are ignored for this run",
                     session_id, state.wallet_baseline_usd);
    } else {
        // Restore open positions from fills table (session continuity across restarts).
        // OracleBuffer seeding for settlement is handled by the RTDS callback and
        // settlement-loop on-demand seeding — we don't seed from Binance here because
        // that would contaminate the Chainlink-only oracle history with cross-source noise.
        auto rows = pg->load_fills_for_session(state.mode_label);
        const int64_t boot_ms = vp::Clock::wall_ns() / 1'000'000LL;
        int restored = 0, restored_expired = 0;
        for (const auto& r : rows) {
            vp::ShareSide s = (r.side_label == "up") ? vp::ShareSide::UP : vp::ShareSide::DOWN;
            vp::OrderDir  d = (r.direction  == "buy") ? vp::OrderDir::Buy : vp::OrderDir::Sell;
            positions.apply_fill(r.condition_id, r.asset, r.horizon_s, r.token_id,
                                 s, d, r.price, r.shares, r.window_close_ms, r.fee_usd);
            ++restored;
            if (r.window_close_ms > 0 && r.window_close_ms <= boot_ms)
                ++restored_expired;
        }
        if (restored > 0)
            spdlog::info("restored {} recent fill rows ({} already closed; settlement will reconcile)",
                         restored, restored_expired);
    }

    // Legacy daily-loss kill state is retained for status/API compatibility.
    // Config parsing forces daily_loss_kill_pct=0 fleet-wide, including hot reloads.
    std::atomic<double> session_start_net_pnl{positions.net_pnl_usd()};
    bool   kill_baseline_set     = false;

    // WPRED calibration ledger: filled checkpoint entries → settled outcomes,
    // per (pair, fraction, probability bucket). Written by the autopilot sweep
    // and the settlement sweep below — both run on this refresh thread, so no
    // extra locking.
    vp::engine::WpredCalibration wpred_cal;

    std::mutex session_records_mu;
    std::vector<SessionOrderRecord> session_records;
    // Accepted order fills this session (excludes settlements) — feeds the
    // fills column of the vp.strategy_runs row.
    std::atomic<long> session_fill_count{0};
    auto record_session_order = [&](const vp::OrderRequest& req, const vp::OrderResult& res) {
        SessionOrderRecord rec;
        rec.ts_ms = vp::Clock::wall_ns() / 1'000'000LL;
        rec.client_order_id = res.client_order_id;
        rec.source = req.source;
        rec.condition_id = req.condition_id;
        rec.asset = req.asset;
        rec.horizon_s = req.horizon_s;
        rec.token_id = req.token_id;
        rec.side = req.side;
        rec.dir = req.dir;
        rec.limit_price = req.limit_price;
        rec.fill_price = res.fill_price;
        rec.fill_shares = res.fill_shares;
        rec.size_usd = req.size_usd;
        rec.fee_usd = res.fee_usd;
        rec.accepted = res.accepted;
        rec.status = res.status.empty() ? (res.accepted ? "filled" : "rejected") : res.status;
        rec.reject_reason = res.reject_reason;
        std::lock_guard<std::mutex> lk(session_records_mu);
        session_records.push_back(std::move(rec));
    };
    auto record_session_fill = [&](SessionOrderRecord rec) {
        rec.ts_ms = vp::Clock::wall_ns() / 1'000'000LL;
        rec.accepted = true;
        std::lock_guard<std::mutex> lk(session_records_mu);
        session_records.push_back(std::move(rec));
    };

        auto journal = vp::Journal{
            [&](const vp::OrderRequest& req, const vp::OrderResult& res) {
                if (res.status == "uncertain") {
                    uncertain_order_halt.store(true);
                    state.autopilot.store(false);
                    spdlog::critical("ambiguous order submission: autonomous trading halted until venue reconciliation");
                }
                record_session_order(req, res);
                if (res.accepted && res.fill_shares > 0) ++session_fill_count;
            const std::string ml = state.mode_label;  // set once at startup
            // Persist order row (accepted or rejected)
            persist_q.push_wait([pg=pg.get(), ml, coid=res.client_order_id, req, res]() noexcept {
                const std::string execution_mode = req.execution_mode == vp::ExecutionMode::Maker
                    ? "maker" : (req.execution_mode == vp::ExecutionMode::Hybrid ? "hybrid" : "taker");
                pg->insert_order(coid, res.exchange_order_id, execution_mode, ml,
                                 req.condition_id, req.token_id,
                                 req.side, req.dir,
                                 res.order_price > 0 ? res.order_price :
                                     (res.fill_price > 0 ? res.fill_price : req.limit_price),
                                 res.order_shares > 0 ? res.order_shares :
                                     (res.fill_shares > 0 ? res.fill_shares : req.size_shares),
                                 req.size_usd,
                                 res.status.empty() ? (res.accepted ? "filled" : "rejected") : res.status,
                                 req.source, res.reject_reason,
                                 res.fill_price,res.accepted ? res.fill_shares : 0,res.fee_usd,
                                 req.manual_market_question, req.outcome_label);
            });
            if (res.accepted && res.fill_shares > 0) {
                spdlog::info("FILL {} {} {}@{:.3f}=${:.2f}  src={}",
                    vp::to_string(req.dir), vp::to_string(req.side),
                    res.fill_shares, res.fill_price,
                    res.fill_shares * res.fill_price, req.source);
            } else if (!res.accepted) {
                if (res.status == "uncertain")
                    spdlog::error("UNCERTAIN ORDER {}: {}", req.source, res.reject_reason);
                else
                    spdlog::warn("REJECT {}: {}", req.source, res.reject_reason);
            } else {
                spdlog::info("ORDER {} {} status={} {:.2f}@{:.3f} id={}",
                    vp::to_string(req.dir), vp::to_string(req.side), res.status,
                    res.order_shares, res.order_price,
                    res.exchange_order_id.empty() ? res.client_order_id : res.exchange_order_id);
            }
        }
    };

    journal.before_submit = [&](const vp::OrderRequest& req, const std::string& intent_id) {
        const std::string execution = req.execution_mode == vp::ExecutionMode::Maker ? "maker" :
            req.execution_mode == vp::ExecutionMode::Hybrid ? "hybrid" : "taker";
        return pg->create_order_intent(intent_id,state.mode_label,req.condition_id,req.token_id,
            req.side,req.dir,execution,req.size_usd,req.size_shares,req.limit_price,req.source);
    };

    auto paper = std::make_unique<vp::PaperExecutor>(
        positions, journal,
        cfg.risk.max_order_usd, cfg.risk.max_deployed_usd,
        cfg.risk.max_per_window_usd, cfg.risk.min_order_usd,
        cfg.risk.clob_min_shares_fallback);

    // Live executor — only constructed if creds are available.
    std::unique_ptr<vp::LiveExecutor> live_exec;
    if (cfg.runtime.mode == vp::TradeMode::Live) {
        {
            live_exec = std::make_unique<vp::LiveExecutor>(
                positions, journal,
                cfg.risk.max_order_usd, cfg.risk.max_deployed_usd,
                cfg.risk.max_per_window_usd, cfg.risk.min_order_usd,
                cfg.risk.clob_min_shares_fallback,
                cfg.polymarket.clob_rest_url,
                cfg.polymarket.proxy_address,
                cfg.polymarket.signer_address,
                cfg.wallet.api_key, cfg.wallet.api_secret, cfg.wallet.api_passphrase,
                private_key_hex,
                cfg.wallet.signature_type,
                cfg.wallet.neg_risk,
                cfg.wallet.builder_code);

            // Auto-fetch live pUSD balance immediately after executor is ready.
            // Only override the TOML starting_balance_usd if CLOB returns a
            // non-zero balance (it can be 0 when the wallet hasn't deposited
            // into the CLOB yet, even though USDC.e sits on-chain).
            double bal = live_exec->fetch_balance();
            if (bal > 0.0) {
                state.wallet_baseline_usd = bal;
            } else if (cfg.wallet.starting_balance_usd > 0.0) {
                state.wallet_baseline_usd = cfg.wallet.starting_balance_usd;
            }

            // Re-adopt this instance's persisted resting orders. Heartbeats
            // normally cancel quotes after a crash, but if the restart is fast
            // enough for an order to survive, the first maintenance poll
            // reconciles its exact matched amount before taking any action.
            for (const auto& o : pg->load_open_orders_for_instance()) {
                vp::OrderRequest req;
                req.condition_id = o.condition_id;
                req.token_id = o.token_id;
                req.asset = o.asset;
                req.horizon_s = o.horizon_s;
                req.side = o.side_label == "UP" ? vp::ShareSide::UP : vp::ShareSide::DOWN;
                req.dir = vp::OrderDir::Buy;
                req.source = o.source;
                req.size_usd = o.price * o.original_shares;
                req.limit_price = o.price;
                req.window_close_ms = o.window_close_ms;
                req.neg_risk = cfg.wallet.neg_risk;
                req.execution_mode = cfg.strategy.entry_order_mode == "hybrid"
                    ? vp::ExecutionMode::Hybrid : vp::ExecutionMode::Maker;
                req.maker_improve_ticks = std::max(0, cfg.strategy.maker_improve_ticks);
                req.maker_reprice_ms = std::max(100, cfg.strategy.maker_reprice_ms);
                req.maker_poll_ms = std::max(100, cfg.strategy.maker_poll_ms);
                req.maker_max_age_ms = std::max(0, cfg.strategy.maker_max_age_ms);
                req.maker_cancel_before_close_ms =
                    std::max(0, cfg.strategy.maker_cancel_before_close_ms);
                live_exec->adopt_open_order(req, o.client_order_id, o.exchange_order_id,
                                            o.price, o.original_shares,
                                            o.matched_shares, o.placed_ms);
            }

            // ── Cold-start warm-up (Phase 1.6) ──────────────────────────
            // Pre-populate per-token CLOB caches (tick-size, neg-risk,
            // min-order-size) in a background thread. Markets keep arriving
            // from Gamma discovery; we snapshot the token list after a short
            // grace so the REST discovery pass has landed, then warm each
            // token once. The first autopilot order then submits with
            // cache-hits only instead of 3 sequential REST lookups.
            std::thread([le = live_exec.get(), &state]() {
                // Grace: let Gamma discovery + first REST book seed land.
                for (int i = 0; i < 60 && !g_stop.load(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                if (g_stop.load() || !le) return;
                std::vector<std::string> tokens;
                {
                    std::lock_guard<std::mutex> lk(state.mu);
                    tokens.reserve(state.markets.size() * 2);
                    for (const auto& m : state.markets) {
                        if (!m.yes_token_id.empty()) tokens.push_back(m.yes_token_id);
                        if (!m.no_token_id.empty())  tokens.push_back(m.no_token_id);
                    }
                }
                if (tokens.empty()) {
                    spdlog::warn("cold-start: no tokens discovered yet — skipping warm-up");
                    return;
                }
                spdlog::info("cold-start: warming {} token caches in background", tokens.size());
                le->warm_token_caches(tokens);
            }).detach();
        }
    }

    auto effective_equity_from_state = [&]() -> double {
        double wallet = 0.0;
        double net_pnl = 0.0;
        {
            std::lock_guard<std::mutex> lk(state.mu);
            wallet = state.wallet_baseline_usd;
            net_pnl = state.net_pnl_usd;
        }
        double equity = wallet + net_pnl;
        if (!std::isfinite(equity) || equity <= 0.0)
            equity = cfg.wallet.starting_balance_usd;
        if (!std::isfinite(equity) || equity <= 0.0)
            equity = wallet;
        if (!std::isfinite(equity) || equity <= 0.0)
            equity = cfg.strategy.min_size_usd;
        return std::max(0.0, equity);
    };

    auto runtime_deployed_cap = [&]() -> double {
        double cap = cfg.risk.max_deployed_usd;
        if (cfg.strategy.equity_max_deployed_pct > 0.0) {
            cap = std::min(cap,
                           effective_equity_from_state() *
                           cfg.strategy.equity_max_deployed_pct);
        }
        return std::max(0.0, cap);
    };

    auto apply_runtime_limits = [&](const char* reason) {
        const double max_deployed = runtime_deployed_cap();
        {
            std::lock_guard<std::mutex> lk(state.mu);
            state.runtime_max_deployed_usd = max_deployed;
        }
        if (live_exec) {
            live_exec->set_limits(
                cfg.risk.max_order_usd, max_deployed,
                cfg.risk.max_per_window_usd, cfg.risk.min_order_usd,
                cfg.risk.clob_min_shares_fallback);
        }
        paper->set_limits(
            cfg.risk.max_order_usd, max_deployed,
            cfg.risk.max_per_window_usd, cfg.risk.min_order_usd,
            cfg.risk.clob_min_shares_fallback);
        spdlog::info("risk limits applied ({}): max_order=${:.2f} per_window=${:.2f} "
                     "max_deployed=${:.2f} equity=${:.2f}",
                     reason, cfg.risk.max_order_usd, cfg.risk.max_per_window_usd,
                     max_deployed, effective_equity_from_state());
    };

    apply_runtime_limits("startup");

    // Book lookup helper (called under state.mu from key callback)
    auto book_lookup = [&](const std::string& cond_id, vp::ShareSide side)
        -> const vp::OrderBook* {
        return vp::book_lookup_locked(state, cond_id, side);
    };

    // Per-(market,direction) order-submission cooldown. Polymarket reserves the
    // whole market balance for an in-flight order (docs: maxOrderSize = balance −
    // Σ open orders), and dedupes identical orders — so the exit loops re-firing a
    // sell on the same position ~1×/sec before the prior FAK clears get rejected
    // ("Invalid order payload") and spam the log. Throttle re-submits per (cid,dir)
    // so only one order per position is outstanding at a time. HUMAN_* / DAILY_KILL
    // are exempt (manual override / emergency must always go through).
    std::unordered_map<std::string, int64_t> last_submit_ms;
    // Per-token quarantine: a token that returns "Invalid order payload" (opaque
    // CLOB-side rejection) or "no match" (no liquidity) is skipped for a while so
    // the bot stops hammering it and focuses on markets that actually fill — this
    // is what keeps the order flow / dashboard clean. HUMAN_* exempt.
    std::unordered_map<std::string, int64_t> token_quarantine_ms;
    // Submit order helper — selects executor, adds signal history entry.
    // Returns the OrderResult so callers can gate post-submit bookkeeping
    // (e.g. ScalpManager::on_exit) on res.accepted, preventing ghost
    // positions when a SELL fill fails on the CLOB.
    auto do_submit = [&](vp::OrderRequest req) -> vp::OrderResult {
        auto manual_order = [](const std::string& source) {
            return source.rfind("HUMAN", 0) == 0 || source == "FLATTEN";
        };
        if (uncertain_order_halt.load() && !manual_order(req.source) &&
            req.source != "DAILY_KILL") {
            vp::OrderResult blocked;
            blocked.client_order_id = vp::core::make_uuid();
            blocked.status = "rejected";
            blocked.terminal = true;
            blocked.reject_class = vp::OrderResult::RejectClass::RiskCap;
            blocked.reject_reason = "unresolved uncertain LIVE order; autonomous trading halted";
            if (journal.on_order) journal.on_order(req, blocked);
            return blocked;
        }
        auto autonomous_sell_enabled = [&]() {
            if (manual_order(req.source)) return true;
            if (req.source == "DAILY_KILL") return cfg.risk.daily_loss_kill_pct > 0.0;
            if (req.source == "ORACLE_REVERSAL") return cfg.strategy.oracle_reversal_exit;
            if (req.source == "ORACLE_PANIC") return cfg.strategy.oracle_panic_exit;
            if (req.source == "MOMENTUM_FLIP") return cfg.strategy.momentum_flip_exit;
            if (req.source.rfind("SCALP_EXIT_", 0) == 0) {
                const std::string reason = req.source.substr(std::string("SCALP_EXIT_").size());
                const bool v2_reversal = cfg.strategy.scalp_v2_enabled &&
                                         cfg.strategy.scalp_v2_reversal_exit;
                const bool v2_trailing = cfg.strategy.scalp_v2_enabled &&
                                         cfg.strategy.scalp_v2_trailing_stop;
                const bool v2_profit = cfg.strategy.scalp_v2_enabled &&
                                       cfg.strategy.scalp_v2_profit_take;
                const bool v3_reversal = cfg.strategy.scalp_v3_enabled &&
                                         cfg.strategy.scalp_v3_reversal_exit;
                const bool v3_trailing = cfg.strategy.scalp_v3_enabled &&
                                         cfg.strategy.scalp_v3_trailing_stop;
                const bool v3_profit = cfg.strategy.scalp_v3_enabled &&
                                       cfg.strategy.scalp_v3_profit_take;
                if (reason == "MOMENTUM_REVERSAL") return v2_reversal || v3_reversal;
                if (reason == "TRAILING_STOP") return v2_trailing || v3_trailing;
                if (reason == "PROFIT_TAKEN") return v2_profit || v3_profit;
                return false;
            }
            if (req.source == "PROFIT_TAKE") return cfg.strategy.profit_take_mode;
            if (req.source == "STOP_LOSS") return cfg.strategy.stop_loss_mode;
            if (req.source == "TIMED_EXIT") return cfg.strategy.timed_exit_secs > 0;
            return true;
        };
        if (req.dir == vp::OrderDir::Sell && !autonomous_sell_enabled()) {
            vp::OrderResult blocked;
            blocked.reject_reason =
                "autonomous sell source disabled by config: " + req.source;
            spdlog::warn("blocked {} sell: {}", req.source, blocked.reject_reason);
            return blocked;
        }

        // WPRED checkpoints are exempt from cooldown/quarantine: one deliberate
        // buy per window+fraction, and a cooldown keyed cid:B (shared with rule
        // entries) would silently drop the dataset order.
        const bool exempt = req.source.rfind("RECYCLER_",0)==0 || manual_order(req.source) || req.source == "DAILY_KILL" ||
                            (!cfg.strategy.window_predictor_source.empty() &&
                             req.source == cfg.strategy.window_predictor_source);
        int64_t nowm = vp::Clock::wall_ns() / 1'000'000LL;
        if (!exempt) {
            // Skip quarantined tokens (recently rejected) to avoid reject-spam.
            auto qit = token_quarantine_ms.find(req.token_id);
            if (qit != token_quarantine_ms.end() && nowm < qit->second) return {};
            std::string k = req.condition_id + (req.dir == vp::OrderDir::Buy ? ":B" : ":S");
            auto it = last_submit_ms.find(k);
            const int64_t order_cooldown_ms =
                std::max(0, cfg.strategy.order_cooldown_ms);
            if (it != last_submit_ms.end() && nowm - it->second < order_cooldown_ms)
                return {};     // silently throttled — keep pos snapshot intact, no CLOB call, no DB/log spam
            last_submit_ms[k] = nowm;
        }
        // Kill switch removed 2026-04-26 by owner request.
        // Inject fee model from config (paper executor uses this; live exec ignores it)
        req.fee_rate     = cfg.strategy.taker_fee_rate;
        req.fee_exponent = cfg.strategy.fee_exponent;
        req.entry_cross_ticks = cfg.strategy.entry_cross_ticks;
        // Maker policy is opt-in and applies only to autonomous entries. Manual
        // buys and every sell/exit preserve immediate taker semantics.
        if (req.dir == vp::OrderDir::Buy && !manual_order(req.source) && req.source.rfind("RECYCLER_",0)!=0) {
            if (cfg.strategy.entry_order_mode == "maker")
                req.execution_mode = vp::ExecutionMode::Maker;
            else if (cfg.strategy.entry_order_mode == "hybrid")
                req.execution_mode = vp::ExecutionMode::Hybrid;
            else
                req.execution_mode = vp::ExecutionMode::Taker;
            req.maker_improve_ticks = std::max(0, cfg.strategy.maker_improve_ticks);
            req.maker_reprice_ms = std::max(100, cfg.strategy.maker_reprice_ms);
            req.maker_poll_ms = std::max(100, cfg.strategy.maker_poll_ms);
            req.maker_max_age_ms = std::max(0, cfg.strategy.maker_max_age_ms);
            req.maker_cancel_before_close_ms =
                std::max(0, cfg.strategy.maker_cancel_before_close_ms);
        }

        if (!live_exec && req.dir==vp::OrderDir::Sell && req.execution_mode==vp::ExecutionMode::Taker) {
            for (const auto& q:paper->open_orders())
                if(q.req.condition_id==req.condition_id && q.req.side==req.side)
                    paper->cancel_order(q.client_order_id);
        }

        if (req.dir==vp::OrderDir::Buy &&
            (req.source=="RECYCLER_SNIPER_ENTRY" || req.source=="RECYCLER_RANGE_ENTRY")) {
            const std::string trace=fmt::format(
                R"({{"model":"reference-v3","midpoint":{:.8f},"limit_price":{:.8f},"reference_price":{:.8f},"strike":{:.8f},"vol_per_sec":{:.12f},"fair_probability_side":{:.8f},"probability_up":{:.8f},"probability_up_sensitivity_low":{:.8f},"probability_up_sensitivity_high":{:.8f},"estimated_net_edge":{:.8f},"reference_age_ms":{}}})",
                req.decision_midpoint,req.limit_price,req.decision_reference_price,
                req.decision_reference_strike,req.decision_reference_vol_per_sec,
                req.decision_fair_probability,req.decision_model_up_probability,
                req.decision_model_up_low,req.decision_model_up_high,req.decision_edge,
                req.decision_reference_age_ms);
            persist_q.push_wait([pg=pg.get(),req,trace]() noexcept {
                pg->insert_signal(req.condition_id,req.side,req.dir,
                    req.decision_fair_probability,"BUY",req.size_usd,req.limit_price,
                    "RECYCLER_SETTLEMENT_EDGE_PASS","reference-v3",trace);
            });
        }

        // Capture pre-fill avg entry for this (cond_id, side) so we can record
        // realized P&L when this is a closing SELL (profit-take / endgame / human).
        double pre_avg_price = 0.0;
        if (req.dir == vp::OrderDir::Sell) {
            for (const auto& p : positions.snapshot()) {
                if (p.condition_id == req.condition_id && p.side == req.side) {
                    pre_avg_price = p.avg_price;
                    break;
                }
            }
        }

        // ── Central risk checks (vp::risk) — BUY entries only. Exits and
        // manual orders must always pass: a stop-loss that can be vetoed is
        // not a stop. Checkpoint entries share the same loss/exposure checks.
        if (req.dir == vp::OrderDir::Buy && !manual_order(req.source)) {
            vp::risk::RiskInput rin;
            rin.order_notional_usd = req.size_usd;
            rin.orders_this_cycle = 0;  // per-window entry count lives in the sweep gates
            rin.now_ms = vp::Clock::wall_ns() / 1'000'000LL;
            {
                const auto snap = positions.snapshot();
                rin.deployed_usd = positions.total_deployed_usd();
                for (const auto& p : snap) {
                    if (p.shares <= cfg.strategy.dust_position_shares) continue;
                    ++rin.open_positions_count;
                    if (p.condition_id == req.condition_id) {
                        ++rin.positions_in_this_market;
                        rin.exposure_this_market_usd += p.shares * p.avg_price;
                    }
                }
            }
            {
                std::lock_guard<std::mutex> lk(state.mu);
                rin.wallet_baseline_usd = state.wallet_baseline_usd;
                rin.net_pnl_usd = state.net_pnl_usd;
                rin.consecutive_losses = state.consecutive_losses;
                rin.loss_pause_until_ms = state.pause_until_ms;
                rin.daily_loss_basis_usd = state.runtime_max_deployed_usd;
            }
            rin.loss_period_start_net_pnl_usd = session_start_net_pnl.load();
            rin.cfg.global_daily_loss_pct = cfg.risk.daily_loss_kill_pct;
            rin.cfg.trader_daily_loss_pct = cfg.risk.daily_loss_kill_pct;
            rin.cfg.consecutive_loss_halt = cfg.strategy.consecutive_loss_pause;
            rin.cfg.consecutive_loss_cooldown_s = cfg.strategy.consecutive_pause_secs;
            rin.cfg.max_orders_per_cycle = cfg.strategy.max_entries_per_window;
            rin.cfg.max_notional_per_order_usd = cfg.risk.max_order_usd;
            rin.cfg.max_gross_exposure_usd = cfg.risk.max_deployed_usd;
            rin.cfg.max_deployed_usd = cfg.risk.max_deployed_usd;
            rin.cfg.max_exposure_per_market_usd = cfg.strategy.max_per_window_usd;
            const auto checks = vp::risk::run_all_checks(rin);
            const std::string failures = vp::risk::aggregate_failures(checks);
            if (failures != "all_passed" && !failures.empty()) {
                vp::OrderResult rr;
                rr.client_order_id = vp::core::make_uuid();
                rr.accepted = false;
                rr.terminal = true;
                rr.status = "rejected";
                rr.reject_class = vp::OrderResult::RejectClass::RiskCap;
                rr.reject_reason = "risk manager: " + failures;
                spdlog::warn("RISK BLOCK {}: {}", req.source, failures);
                if (journal.on_order) journal.on_order(req, rr);
                return rr;
            }
        }

        vp::OrderResult res;
        if (live_exec)
            res = live_exec->submit(req, book_lookup);
        else
            res = paper->submit(req, book_lookup);

        // Quarantine tokens that reject, so the bot stops hammering them (the
        // opaque "Invalid order payload" persists per-token; "no match" is thin
        // liquidity). Keeps the order flow clean; healthy markets keep trading.
        if (!exempt && !res.accepted) {
            if (res.reject_reason.find("Invalid order payload") != std::string::npos)
                token_quarantine_ms[req.token_id] =
                    nowm + std::max(0, cfg.strategy.token_quarantine_payload_ms);
            else if (res.reject_reason.find("no orders found to match") != std::string::npos)
                token_quarantine_ms[req.token_id] =
                    nowm + std::max(0, cfg.strategy.token_quarantine_no_match_ms);
            // Budget-cap rejects (window/deployed/min-share-vs-cap) only clear when
            // a position settles or is sold, so retrying every order_cooldown_ms is
            // pure reject spam (155 rejects in one session report). Back off.
            else if (res.reject_reason.find("> cap") != std::string::npos)
                token_quarantine_ms[req.token_id] =
                    nowm + std::max(0, cfg.strategy.token_quarantine_no_match_ms);
        }

        // Update positions snapshot
        {
            std::lock_guard<std::mutex> lk(state.mu);
            state.positions_snap = positions.snapshot();
            state.deployed_usd   = positions.total_deployed_usd();
            state.net_pnl_usd    = positions.net_pnl_usd();
            auto st = positions.trade_stats();
            state.trades_won     = st.wins;
            state.trades_lost    = st.losses;

            // Record SELL fills as RecentResult so the dashboard reflects
            // profit-take / endgame / manual exits — not just window-close
            // settlements (which are dominated by losses).
            if (res.accepted && req.dir == vp::OrderDir::Sell &&
                res.fill_shares > 0 && pre_avg_price > 0) {
                vp::SharedState::RecentResult rr;
                rr.ts_ms        = vp::Clock::wall_ns() / 1'000'000LL;
                rr.asset        = req.asset;
                rr.side         = req.side;
                rr.horizon_s    = req.horizon_s;
                rr.shares       = res.fill_shares;
                rr.entry_price  = pre_avg_price;
                rr.settle_price = res.fill_price;
                rr.pnl_usd      = (res.fill_price - pre_avg_price) * res.fill_shares
                                  - res.fee_usd;
                rr.win          = (rr.pnl_usd > 0);
                state.recent_results.insert(state.recent_results.begin(), rr);
                if (state.recent_results.size() > 12)
                    state.recent_results.resize(12);

                // Enforce the configured loss-streak cooldown. The state fields
                // and autopilot pause check existed, but no exit path ever updated
                // them, so consecutive_loss_pause was previously a no-op.
                if (rr.win) {
                    state.consecutive_losses = 0;
                } else {
                    ++state.consecutive_losses;
                    if (cfg.strategy.consecutive_loss_pause > 0 &&
                        state.consecutive_losses >=
                            cfg.strategy.consecutive_loss_pause) {
                        state.pause_until_ms = std::max(
                            state.pause_until_ms,
                            rr.ts_ms + (int64_t)std::max(
                                0, cfg.strategy.consecutive_pause_secs) * int64_t{1000});
                        spdlog::warn(
                            "autopilot paused after {} consecutive loss(es) until {}",
                            state.consecutive_losses, state.pause_until_ms);
                    }
                }
            }

            // Signal history entry
            std::string entry;
            if (res.accepted && res.resting) {
                entry = fmt::format("[REST] {} {} {:.2f}sh@{:.3f}",
                    vp::to_string(req.dir), vp::to_string(req.side),
                    res.order_shares, res.order_price);
            } else if (res.accepted) {
                // Manual HH:MM:SS — fmt chrono not available in this version
                auto now = std::chrono::system_clock::now();
                auto t = std::chrono::system_clock::to_time_t(now);
                std::tm tm_buf{};
                localtime_r(&t, &tm_buf);
                entry = fmt::format("[{:02d}:{:02d}:{:02d}] {} {} {:.4f}sh@{:.3f}",
                    tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                    vp::to_string(req.dir), vp::to_string(req.side),
                    res.fill_shares, res.fill_price);
            } else {
                entry = fmt::format("[REJECT] {} {}: {}",
                    vp::to_string(req.dir), vp::to_string(req.side),
                    res.reject_reason);
            }
            state.signal_history.insert(state.signal_history.begin(), entry);
            if (state.signal_history.size() > 15)
                state.signal_history.resize(15);
        }
        if (res.accepted && res.fill_shares > 0)
            apply_runtime_limits("fill");
        return res;
    };

    // LLM advisor
    vp::LlmConfig llm_cfg;
    llm_cfg.url     = cfg.llm.url + "/chat/completions";
    llm_cfg.model   = cfg.llm.model;
    llm_cfg.api_key = cfg.llm.api_key;

    auto llm = std::make_unique<vp::LlmAdvisor>(llm_cfg,
        [&state](std::string advice) {
            std::lock_guard<std::mutex> lk(state.mu);
            state.llm_advice = std::move(advice);
        });

    // (AI Trader's Brief LLM loop temporarily disabled — was causing
    // shutdown segfault. Panel still renders with placeholder text so
    // the UI changes are visible. Will re-add with proper lifetime
    // management once the watchlist/wallet-strip changes are confirmed.)

    // TUI
    vp::Tui tui;
    if (!tui.init(headless)) spdlog::error("TUI init failed — log-only mode");
    g_tui.store(&tui);
    tui.conn().pg.store(true);

    // Key callback — dispatched from TUI input thread for B/N/S/M/K/A/L/X/space.
    tui.set_key_callback([&](char k) {
        // All trading keys require state.mu; LLM query reads state without it.
        std::string active_cond, active_asset, active_tok_up, active_tok_dn;
        int active_horizon = 0; int64_t active_close_ms = 0;
        double up_ask = 0, dn_ask = 0;
        bool active_neg_risk = true;
        double active_tick_size = 0.01;
        {
            std::lock_guard<std::mutex> lk(state.mu);
            // Use active_cond_id (set by rebuild_snapshot from filtered watchlist)
            // rather than state.active_index into the unfiltered markets array.
            const std::string& cid = state.active_cond_id;
            auto mit = std::find_if(state.markets.begin(), state.markets.end(),
                [&](const vp::PmMarket& m){ return m.condition_id == cid; });
            if (mit != state.markets.end()) {
                const auto& m = *mit;
                active_cond    = m.condition_id;
                active_asset   = m.asset;
                active_horizon = m.horizon_seconds;
                active_close_ms= m.window_close_ms;
                active_tok_up  = m.yes_token_id;
                active_tok_dn  = m.no_token_id;
                active_neg_risk = m.neg_risk;
                active_tick_size = m.tick_size;
                if (auto it = state.book_up.find(active_cond);
                    it != state.book_up.end() && it->second)
                    up_ask = it->second->best_ask().price;
                if (auto it = state.book_down.find(active_cond);
                    it != state.book_down.end() && it->second)
                    dn_ask = it->second->best_ask().price;
            }
        }
        if (active_cond.empty()) return;

        k = (char)toupper((unsigned char)k);

        if (k == 'B') {
            // BUY UP
            vp::OrderRequest req;
            req.condition_id   = active_cond;
            req.asset          = active_asset;
            req.horizon_s      = active_horizon;
            req.token_id       = active_tok_up;
            req.side           = vp::ShareSide::UP;
            req.dir            = vp::OrderDir::Buy;
            req.size_usd       = cfg.strategy.per_click_size_usd;
            req.limit_price    = std::min(cfg.strategy.hard_share_price_ceil,
                                          up_ask * (1.0 + cfg.strategy.manual_buy_slippage_pct) +
                                          cfg.strategy.manual_buy_slippage_extra);
            req.source         = "HUMAN_KEYPRESS";
            req.window_close_ms= active_close_ms;
            req.neg_risk       = active_neg_risk;
            req.tick_size      = active_tick_size;
            do_submit(req);
        } else if (k == 'N') {
            // BUY DOWN
            vp::OrderRequest req;
            req.condition_id   = active_cond;
            req.asset          = active_asset;
            req.horizon_s      = active_horizon;
            req.token_id       = active_tok_dn;
            req.side           = vp::ShareSide::DOWN;
            req.dir            = vp::OrderDir::Buy;
            req.size_usd       = cfg.strategy.per_click_size_usd;
            req.limit_price    = std::min(cfg.strategy.hard_share_price_ceil,
                                          dn_ask * (1.0 + cfg.strategy.manual_buy_slippage_pct) +
                                          cfg.strategy.manual_buy_slippage_extra);
            req.source         = "HUMAN_KEYPRESS";
            req.window_close_ms= active_close_ms;
            req.neg_risk       = active_neg_risk;
            req.tick_size      = active_tick_size;
            do_submit(req);
        } else if (k == 'S') {
            // SELL UP (whole position)
            vp::OrderRequest req;
            req.condition_id   = active_cond;
            req.asset          = active_asset;
            req.horizon_s      = active_horizon;
            req.token_id       = active_tok_up;
            req.side           = vp::ShareSide::UP;
            req.dir            = vp::OrderDir::Sell;
            req.size_shares    = 0;  // means "all"
            req.source         = "HUMAN_KEYPRESS";
            req.window_close_ms= active_close_ms;
            req.neg_risk       = active_neg_risk;
            req.tick_size      = active_tick_size;
            do_submit(req);
        } else if (k == 'M') {
            // SELL DOWN (whole position)
            vp::OrderRequest req;
            req.condition_id   = active_cond;
            req.asset          = active_asset;
            req.horizon_s      = active_horizon;
            req.token_id       = active_tok_dn;
            req.side           = vp::ShareSide::DOWN;
            req.dir            = vp::OrderDir::Sell;
            req.size_shares    = 0;
            req.source         = "HUMAN_KEYPRESS";
            req.window_close_ms= active_close_ms;
            req.neg_risk       = active_neg_risk;
            req.tick_size      = active_tick_size;
            do_submit(req);
        } else if (k == 'K') {
            // KILL SWITCH removed 2026-04-26 — 'K' now flattens active market only,
            // does NOT block future buys.
            spdlog::info("K: flattening active market (kill switch disabled)");
            for (auto side : {vp::ShareSide::UP, vp::ShareSide::DOWN}) {
                vp::OrderRequest req;
                req.condition_id   = active_cond;
                req.asset          = active_asset;
                req.horizon_s      = active_horizon;
                req.token_id       = (side == vp::ShareSide::UP) ? active_tok_up : active_tok_dn;
                req.side           = side;
                req.dir            = vp::OrderDir::Sell;
                req.size_shares    = 0;
                req.source         = "FLATTEN";
                req.window_close_ms= active_close_ms;
                req.neg_risk       = active_neg_risk;
                req.tick_size      = active_tick_size;
                do_submit(req);
            }
        } else if (k == 'A') {
            // Toggle autopilot
            bool was = state.autopilot.load();
            if (!was && uncertain_order_halt.load()) {
                spdlog::error("autopilot ON refused: uncertain LIVE order unresolved");
                return;
            }
            state.autopilot.store(!was);
            spdlog::info("autopilot: {}", was ? "OFF" : "ON");
        } else if (k == 'L') {
            // LLM advisory query (async)
            double up_mid = 0, dn_mid = 0, asset_px = 0;
            {
                std::lock_guard<std::mutex> lk(state.mu);
                auto mit2 = std::find_if(state.markets.begin(), state.markets.end(),
                    [&](const vp::PmMarket& m){ return m.condition_id == state.active_cond_id; });
                if (mit2 != state.markets.end()) {
                    asset_px = state.asset_price(mit2->asset);
                    if (auto it = state.book_up.find(active_cond);
                        it != state.book_up.end() && it->second) {
                        auto b = it->second->best_bid(), a = it->second->best_ask();
                        up_mid = (b.price + a.price) / 2.0;
                    }
                    if (auto it = state.book_down.find(active_cond);
                        it != state.book_down.end() && it->second) {
                        auto b = it->second->best_bid(), a = it->second->best_ask();
                        dn_mid = (b.price + a.price) / 2.0;
                    }
                }
            }
            std::string ctx = fmt::format(
                "market={} asset={} horizon={}s up={:.3f} down={:.3f} "
                "asset_price={:.2f} deployed=${:.2f}",
                active_cond.substr(0, 16), active_asset, active_horizon,
                up_mid, dn_mid, asset_px,
                positions.total_deployed_usd());
            {
                std::lock_guard<std::mutex> lk(state.mu);
                state.llm_advice = "[LLM querying...]";
            }
            llm->query_async(ctx);
        } else if (k == 'X') {
            // Reset kill switch (also allows daily-loss re-arm)
            state.kill_switch.store(false);
            state.daily_kill_fired.store(false);
            spdlog::info("kill switch cleared (daily loss re-armed)");
        } else if (k == 'R') {
            // Reset paper wallet — clear all positions and zero all counters.
            // Only permitted in PAPER mode; a safeguard warning is logged.
            positions.reset();
            {
                std::lock_guard<std::mutex> lk(state.mu);
                state.net_pnl_usd  = 0;
                state.deployed_usd = 0;
                state.trades_won   = 0;
                state.trades_lost  = 0;
                state.recent_results.clear();
            }
            spdlog::warn("PAPER WALLET RESET — all positions and P&L cleared by user");
        }
    });

    // Polymarket REST + WS + Binance (started on bootstrap thread)
    vp::PolymarketRest rest(cfg.polymarket.clob_rest_url, cfg.polymarket.gamma_rest_url);
    std::unique_ptr<vp::PolymarketMD> pm_md;
    std::unique_ptr<vp::BinanceFeed>  binance;
    // Binance USDT-M futures WS — forceOrder (liquidation) stream only.
    // Phase 4.1: forced liquidations are the "smart money pressure" signal;
    // SELL-liquidations push price down (and vice versa). Empty URL → off.
    std::unique_ptr<vp::BinanceFeed>  binance_futures;
    std::unique_ptr<vp::CoinbaseWsFeed> coinbase_ws;
    std::unique_ptr<vp::PolymarketRtds> rtds;
    std::unique_ptr<vp::PolymarketUserWs> user_ws;
    // Rate-limiter for book snapshot persistence (1 Hz per token).
    // Lives at main() scope so the long-lived BookUpdate callback can reference it safely.
    std::unordered_map<std::string, std::int64_t> last_snap_ms;

    std::thread bootstrap_th([&] {
        // Market discovery
        auto markets = rest.discover_updown_markets();
        spdlog::info("discovered {} UP/DOWN markets", markets.size());
        // Bound the universe to the nearest few 5m/15m windows per series so the
        // pm-md subscription set never grows large enough for Polymarket to drop
        // the socket (the root cause of order-book disconnects). See
        // select_active_universe().
        {
            int64_t disc_now_ms = vp::Clock::wall_ns() / 1'000'000LL;
            size_t before = markets.size();
            markets = vp::select_active_universe(std::move(markets),
                                                 cfg.polymarket,
                                                 cfg.strategy.blocked_assets, disc_now_ms);
            spdlog::info("universe bound: {} markets selected from {} discovered "
                         "(<= {} windows/series, {} max, prewarm >= {}m)",
                         markets.size(), before,
                         cfg.polymarket.universe_max_windows_per_series,
                         cfg.polymarket.universe_max_markets,
                         cfg.polymarket.universe_prewarm_ahead_minutes);
        }
        {
            std::lock_guard<std::mutex> lk(state.mu);
            state.markets = std::move(markets);
            for (const auto& m : state.markets) {
                state.book_up[m.condition_id]   = std::make_shared<vp::OrderBook>();
                state.book_down[m.condition_id] = std::make_shared<vp::OrderBook>();
                state.token_to_cond[m.yes_token_id] = m.condition_id;
                state.token_to_cond[m.no_token_id]  = m.condition_id;
                state.token_to_side[m.yes_token_id] = vp::ShareSide::UP;
                state.token_to_side[m.no_token_id]  = vp::ShareSide::DOWN;
            }
        }
        // Upsert markets into Postgres (best-effort)
        for (const auto& m : state.markets) {
            persist_q.push([pg=pg.get(), m]() noexcept {
                pg->upsert_market(m.condition_id, m.asset, m.horizon_seconds,
                                  m.target_price, m.window_open_ms, m.window_close_ms,
                                  m.yes_token_id, m.no_token_id, m.question, m.slug, m.settlement_window_s);
            });
            // Log window opened event
            if (cfg.data_collection.window_events_enabled && m.window_open_ms > 0) {
                persist_q.push([pg=pg.get(), cid=m.condition_id]() noexcept {
                    pg->insert_window_event(cid, "opened", "");
                });
            }
        }

        // Polymarket MD WebSocket
        pm_md = std::make_unique<vp::PolymarketMD>(
            cfg.polymarket.clob_ws_url,
            [&state, &persist_q, &last_snap_ms, &tick_buffer, &cfg, pg=pg.get()]
            (const vp::PolymarketMD::BookUpdate& bu) {
                std::string cid;
                vp::ShareSide side = vp::ShareSide::UP;
                std::shared_ptr<vp::OrderBook> b;
                {
                    std::lock_guard<std::mutex> lk(state.mu);
                    auto cit = state.token_to_cond.find(bu.token_id);
                    if (cit == state.token_to_cond.end()) return;
                    auto sit = state.token_to_side.find(bu.token_id);
                    if (sit == state.token_to_side.end()) return;
                    cid = cit->second;
                    side = sit->second;
                    auto& slot = (side == vp::ShareSide::UP)
                        ? state.book_up[cid]
                        : state.book_down[cid];
                    if (!slot) slot = std::make_shared<vp::OrderBook>();
                    b = slot;
                }
                if (bu.is_snapshot) {
                    b->apply_snapshot(bu.bids, bu.asks);
                } else {
                    for (auto& l : bu.bids) b->apply_delta_bid(l.price, l.size);
                    for (auto& l : bu.asks) b->apply_delta_ask(l.price, l.size);
                }
                double tick_bb = b->best_bid().price;
                double tick_ba = b->best_ask().price;
                if (tick_bb > 0 && tick_ba > 0) {
                    double mid = (tick_bb + tick_ba) / 2.0;
                    int64_t now_us = vp::Clock::wall_ns() / 1000LL;
                    tick_buffer.on_price(cid,
                        side == vp::ShareSide::UP ? "UP" : "DOWN",
                        mid, now_us);
                }
                // Track last update time for staleness detection
                int64_t book_now_ms = bu.exchange_ms > 0
                    ? bu.exchange_ms
                    : vp::Clock::wall_ns() / 1'000'000LL;
                {
                    std::lock_guard<std::mutex> lk(state.mu);
                    auto cit = state.token_to_cond.find(bu.token_id);
                    if (cit != state.token_to_cond.end() && cit->second == cid)
                        state.last_book_update_ms[bu.token_id] = book_now_ms;
                }
                // Rate-limited persist: 1 snapshot/token/s
                int64_t now_ms = vp::Clock::wall_ns() / 1'000'000LL;
                auto& last = last_snap_ms[bu.token_id];
                if (cfg.data_collection.book_snapshots_enabled && now_ms - last >= 1000) {
                    last = now_ms;
                    auto snap_bids = b->copy_bids();
                    auto snap_asks = b->copy_asks();
                    vp::ShareSide snap_side = side;
                    double bb = b->best_bid().price, ba = b->best_ask().price;
                    persist_q.push([pg, cid, tok=bu.token_id,
                                    snap_side, bids=std::move(snap_bids),
                                    asks=std::move(snap_asks), bb, ba]() noexcept {
                        pg->insert_book_snapshot(cid, tok, snap_side, bids, asks, bb, ba);
                    });
                }
            },
            [&persist_q, pg=pg.get(), &state, &cfg, &pm_flow](const vp::PolymarketMD::TradeUpdate& tu) {
                // Tick-rule classification against the live book: the public
                // trade feed carries no taker side, so a print at/above the
                // ask is a buy, at/below the bid a sell, mid prints are
                // dropped. Sign convention: +1 supports UP (buy UP / sell
                // DOWN), -1 supports DOWN.
                {
                    std::lock_guard<std::mutex> lk(state.mu);
                    auto cit = state.token_to_cond.find(tu.token_id);
                    auto sit = state.token_to_side.find(tu.token_id);
                    if (cit != state.token_to_cond.end() &&
                        sit != state.token_to_side.end()) {
                        const std::string& cid = cit->second;
                        const vp::ShareSide side = sit->second;
                        auto& bookmap = (side == vp::ShareSide::UP)
                            ? state.book_up : state.book_down;
                        int sign = 0;
                        auto bit = bookmap.find(cid);
                        if (bit != bookmap.end() && bit->second) {
                            const double bb = bit->second->best_bid().price;
                            const double ba = bit->second->best_ask().price;
                            if (ba > 0 && tu.price >= ba) {
                                sign = (side == vp::ShareSide::UP) ? +1 : -1;
                            } else if (bb > 0 && tu.price <= bb) {
                                sign = (side == vp::ShareSide::UP) ? -1 : +1;
                            }
                        }
                        if (sign != 0)
                            pm_flow.on_trade(cid, tu.size, sign, tu.exchange_ms);
                    }
                }
                if (!cfg.data_collection.pm_trades_enabled) return;
                persist_q.push([pg, tu]() noexcept {
                    pg->insert_pm_trade(tu.condition_id, tu.token_id,
                                        tu.side_label,
                                        tu.price, tu.size, tu.exchange_ms);
                });
            },
            // best_bid_ask push (custom_feature_enabled): per-token freshness
            // heartbeat + ladder desync detector. Does NOT touch the ladder
            // (event carries no sizes).
            [&state, &tick_buffer](const vp::PolymarketMD::BboUpdate& bb) {
                int64_t now_ms = vp::Clock::wall_ns() / 1'000'000LL;
                std::string cid;
                vp::ShareSide side = vp::ShareSide::UP;
                double lad_bid = 0, lad_ask = 0;
                {
                    std::lock_guard<std::mutex> lk(state.mu);
                    auto cit = state.token_to_cond.find(bb.token_id);
                    if (cit == state.token_to_cond.end()) return;
                    auto sit = state.token_to_side.find(bb.token_id);
                    if (sit == state.token_to_side.end()) return;
                    cid  = cit->second;
                    side = sit->second;
                    state.last_book_update_ms[bb.token_id] =
                        bb.exchange_ms > 0 ? bb.exchange_ms : now_ms;
                    auto& bookmap = (side == vp::ShareSide::UP)
                        ? state.book_up : state.book_down;
                    auto bit = bookmap.find(cid);
                    if (bit != bookmap.end() && bit->second) {
                        lad_bid = bit->second->best_bid().price;
                        lad_ask = bit->second->best_ask().price;
                        // >= ~1.5 ticks off => our ladder is desynced (a 1-tick
                        // transient race with an in-flight delta is normal).
                        const double tol = 0.015;
                        bool desync =
                            (lad_bid > 0 && bb.best_bid > 0 && std::abs(lad_bid - bb.best_bid) > tol) ||
                            (lad_ask > 0 && bb.best_ask > 0 && std::abs(lad_ask - bb.best_ask) > tol);
                        if (desync) state.bbo_desync_tokens.insert(bb.token_id);
                    }
                }
                // Feed the scalp velocity buffer with the freshest mid.
                if (bb.best_bid > 0 && bb.best_ask > 0) {
                    tick_buffer.on_price(cid,
                        side == vp::ShareSide::UP ? "UP" : "DOWN",
                        (bb.best_bid + bb.best_ask) / 2.0,
                        now_ms * 1000LL);
                }
            },
            // on_state: clear book timestamps on disconnect so the stale-book
            // guard blocks all order firing until fresh WS deltas or REST seed
            // re-stamps them. Prevents FAK rejections on post-reconnect stale books.
            [&state](bool connected) {
                if (!connected) {
                    std::lock_guard<std::mutex> lk(state.mu);
                    state.last_book_update_ms.clear();
                }
            });
        pm_md->start();
        {
            std::vector<std::string> tokens;
            std::lock_guard<std::mutex> lk(state.mu);
            tokens.reserve(state.markets.size() * 2);
            int64_t now_ms = vp::Clock::wall_ns() / 1'000'000LL;
            // Subscribe only to non-closed, unblocked markets selected by the
            // bounded universe policy above. This keeps the live subscription
            // set small enough for a stable market-data socket.
            for (const auto& m : state.markets) {
                if (m.window_close_ms > 0 && m.window_close_ms <= now_ms) continue;
                bool blocked = false;
                for (const auto& b : cfg.strategy.blocked_assets)
                    if (m.asset == b) { blocked = true; break; }
                if (blocked) continue;
                tokens.push_back(m.yes_token_id);
                tokens.push_back(m.no_token_id);
            }
            pm_md->subscribe(tokens);
            spdlog::info("pm-md initial subscribe: {} tokens ({} markets of {} discovered)",
                         tokens.size(), tokens.size() / 2, state.markets.size());
        }
        tui.conn().pm_md.store(true);

        // ── Oracle ingestion + RTDS failover ─────────────────────────────
        // 2026-06-10: Polymarket's RTDS (ws-live-data) went silent service-wide
        // (~00:17 EDT — verified: ALL topics return zero frames while the WS
        // handshake succeeds; subscribe format re-checked against current docs).
        // Without oracle ticks the whole bot is blind: no oracle-lag entries,
        // no oracle-gated scalps, and paper settlement can't book W/L.
        // Failover: when RTDS has been quiet > kOracleFailoverMs, synthesize
        // oracle ticks from the Binance trade stream (same underlying market
        // Chainlink aggregates; Binance typically LEADS the oracle). RTDS
        // reclaims priority on its first fresh tick.
        {
            using namespace std::chrono;
            oracle_failover.last_rtds_ms = duration_cast<milliseconds>(
                system_clock::now().time_since_epoch()).count();
        }

        // Shared oracle tick path (called from the RTDS thread and, in
        // failover, from the Binance thread — seeded-window set is guarded
        // by state.mu). static: this setup code runs on the one-shot bootstrap
        // thread, but the feed callbacks outlive that stack; the captured refs
        // are main()-scope objects that outlive the feeds.
        static std::function<void(const vp::OracleTick&, const char*)> ingest_oracle_tick =
            [&state, &oracle_buf, &oracle_seeded_windows, &tui, &cfg,
             &persist_q, pg=pg.get()](const vp::OracleTick& t, const char* source) {
                // Reference streams have distinct keys in memory and storage.
                // Spot failover cannot overwrite an official TWAP observation.
                if (t.window_s > 0 || std::string_view(source) == "chainlink_rtds") {
                    const std::string key = t.asset + (t.window_s > 0 ? ":TWAP" + std::to_string(t.window_s) : ":SPOT");
                    oracle_buf.add(key, t.ts_ms / 1000.0, t.price);
                    if (cfg.data_collection.oracle_ticks_enabled)
                        persist_q.push([pg, key, price=t.price, ts_ms=t.ts_ms]() noexcept {
                            pg->insert_oracle_tick(key, price, ts_ms, "chainlink_twap");
                        });
                    if (t.window_s > 0) return;
                }
                struct SeedTarget { int64_t open_ms; std::string condition_id; };
                std::vector<SeedTarget> windows_to_seed;
                {
                    std::lock_guard<std::mutex> lk(state.mu);
                    state.oracle_price(t.asset) = t.price;
                    tui.conn().rtds.store(true);

                    using namespace std::chrono;
                    int64_t now_ms = duration_cast<milliseconds>(
                        system_clock::now().time_since_epoch()).count();
                    for (const auto& m : state.markets) {
                        if (m.asset != t.asset) continue;
                        if (m.window_open_ms <= 0 || m.window_open_ms > now_ms) continue;
                        if (m.window_close_ms <= now_ms) continue;
                        if (oracle_seeded_windows.insert({t.asset, m.window_open_ms}).second)
                            windows_to_seed.push_back({m.window_open_ms, m.condition_id});
                    }
                }

                // Log first oracle tick for unseeded windows (research tracking).
                // DO NOT artificially seed — ticks keep their natural timestamps;
                // lookup_at_or_before finds the first real tick after window open.
                for (const auto& target : windows_to_seed) {
                    spdlog::info("oracle window opened: {} @{} (first tick at ${:.2f}, src={})",
                                 t.asset, target.open_ms, t.price, source);
                    if (cfg.data_collection.window_events_enabled) {
                        std::string seed_payload = fmt::format(
                            R"({{"price":{},"ts_ms":{},"source":"{}"}})",
                            t.price, target.open_ms, source);
                        persist_q.push([pg, cid=target.condition_id,
                                        p=std::move(seed_payload)]() noexcept {
                            pg->insert_window_event(cid, "first_oracle_seed", p);
                        });
                    }
                }

                // Append the tick at its NATURAL timestamp.
                oracle_buf.add(t.asset, (double)t.ts_ms / 1000.0, t.price);

                if (cfg.data_collection.oracle_ticks_enabled) {
                    persist_q.push([pg, asset=t.asset, price=t.price,
                                    ts_ms=t.ts_ms, source=std::string(source)]() noexcept {
                        pg->insert_oracle_tick(asset, price, ts_ms, source);
                    });
                }
            };

        // Binance trade stream
        binance = std::make_unique<vp::BinanceFeed>(
            cfg.binance.ws_url,
            std::vector<std::string>{"btcusdt@trade", "ethusdt@trade", "solusdt@trade", "xrpusdt@trade",
                                     "btcusdt@bookTicker", "ethusdt@bookTicker"},
            [&state, &tui, &candle_store, &trade_flow, &cfg, &persist_q,
             pg=pg.get()](const vp::AssetTick& t) {
                std::string asset = vp::engine::symbol_to_asset(t.symbol);

                // ── Futures forceOrder (Phase 4.1): liquidation pressure ──
                // (Sent on the futures feed only, but the parser is shared.)
                if (t.is_force_order) {
                    if (asset.empty()) return;
                    using namespace std::chrono;
                    int64_t now_ms = duration_cast<milliseconds>(
                        system_clock::now().time_since_epoch()).count();
                    std::lock_guard<std::mutex> lk(state.mu);
                    // Signed notional: SELL-liquidation negative, BUY positive.
                    double signed_usd = (t.buyer_is_maker ? +1.0 : -1.0)
                                      * t.price * t.qty;
                    auto& ev = state.liq_events[asset];
                    ev.emplace_back(t.exchange_ms > 0 ? t.exchange_ms : now_ms,
                                    signed_usd);
                    while (ev.size() > 400) ev.pop_front();
                    return;
                }

                // ── bookTicker (Phase 6.3): store the sub-100ms top-of-book
                // snapshot + mid history. NO candle/flow ingestion — book
                // quotes are not trades and would pollute the 1m OHLC bars.
                if (t.is_book_ticker) {
                    if (asset.empty()) return;
                    using namespace std::chrono;
                    int64_t now_ms = duration_cast<milliseconds>(
                        system_clock::now().time_since_epoch()).count();
                    std::lock_guard<std::mutex> lk(state.mu);
                    auto& bt = state.book_ticker[asset];
                    if (t.bid_price > 0 && t.ask_price > 0) {
                        bt.bid = t.bid_price; bt.bid_q = t.bid_qty;
                        bt.ask = t.ask_price; bt.ask_q = t.ask_qty;
                        bt.ts_ms = t.exchange_ms;
                        bt.mid_hist.emplace_back(now_ms, 0.5 * (t.bid_price + t.ask_price));
                        while (bt.mid_hist.size() > 60) bt.mid_hist.pop_front();
                    }
                    return;
                }
                {
                    std::lock_guard<std::mutex> lk(state.mu);
                    if (!asset.empty()) {
                        state.asset_price(asset) = t.price;
                        state.asset_recent(asset).push_back(t.price);
                        if (state.asset_recent(asset).size() > 80) state.asset_recent(asset).erase(state.asset_recent(asset).begin());
                    }
                    tui.conn().binance.store(true);
                    if (!asset.empty()) {
                        candle_store.on_tick(asset, t.price, t.qty, t.buyer_is_maker, t.exchange_ms);
                    }
                    trade_flow.on_trade(t.symbol, t.qty, !t.buyer_is_maker, t.exchange_ms);
                    // Persist binance tick (best-effort, via persist queue)
                    if (cfg.data_collection.binance_ticks_enabled) {
                        persist_q.push([pg, sym=t.symbol, price=t.price,
                                        qty=t.qty, maker=t.buyer_is_maker,
                                        exch_ms=t.exchange_ms]() noexcept {
                            pg->insert_binance_tick(sym, price, qty, maker, exch_ms);
                        });
                    }
                }
                // Oracle failover (LAST resort): if RTDS is silent AND the
                // Coinbase WS is also stale, feed the strategy from Binance
                // (throttled). Must run OUTSIDE state.mu — the ingest path
                // takes that lock itself. Coinbase outranks Binance: Chainlink
                // aggregates USD venues (Coinbase dominant); Binance is the
                // USDT pair with a basis error.
                if (asset.empty()) return;
                using namespace std::chrono;
                int64_t now_ms = duration_cast<milliseconds>(
                    system_clock::now().time_since_epoch()).count();
                if (now_ms - oracle_failover.last_rtds_ms.load(std::memory_order_relaxed)
                        < kOracleFailoverMs) return;
                if (now_ms - oracle_failover.last_cb_ws_ms.load(std::memory_order_relaxed)
                        < kCoinbaseFreshMs) return;
                auto& synth_ms = oracle_failover.last_synth_ms[OracleFailover::idx(asset)];
                int64_t prev = synth_ms.load(std::memory_order_relaxed);
                if (now_ms - prev < kFailoverSynthGapMs) return;
                if (!synth_ms.compare_exchange_strong(prev, now_ms)) return;
                if (!oracle_failover.active.exchange(true))
                    spdlog::warn("oracle FAILOVER ACTIVE: RTDS silent > {}s — synthesizing oracle ticks from Binance",
                                 kOracleFailoverMs / 1000);
                vp::OracleTick synth{asset, t.price,
                                     t.exchange_ms > 0 ? t.exchange_ms : now_ms};
                ingest_oracle_tick(synth, "binance_failover");
            });
        binance->start();

        // ── Binance USDT-M futures forceOrder feed (Phase 4.1) ──────────
        // Liquidation events = forced-position unwinds: SELL-liquidations
        // mechanically push price down, BUY-liquidations push it up. Feeds
        // the multi-source directional vote. Empty URL → disabled.
        if (!cfg.binance.futures_ws_url.empty()) {
            binance_futures = std::make_unique<vp::BinanceFeed>(
                cfg.binance.futures_ws_url,
                std::vector<std::string>{"btcusdt@forceOrder", "ethusdt@forceOrder"},
                [&state, &cfg, &persist_q, pg=pg.get()](const vp::AssetTick& t) {
                    if (!t.is_force_order) return;
                    using namespace std::chrono;
                    auto dash = t.symbol.find("USDT");
                    if (dash == std::string::npos) return;
                    std::string asset = t.symbol.substr(0, dash);
                    int64_t now_ms = duration_cast<milliseconds>(
                        system_clock::now().time_since_epoch()).count();
                    std::lock_guard<std::mutex> lk(state.mu);
                    double signed_usd = (t.buyer_is_maker ? +1.0 : -1.0)
                                      * t.price * t.qty;
                    auto& ev = state.liq_events[asset];
                    ev.emplace_back(t.exchange_ms > 0 ? t.exchange_ms : now_ms,
                                    signed_usd);
                    while (ev.size() > 400) ev.pop_front();
                    // Persist liquidation events for research (cheap, rare).
                    if (cfg.data_collection.binance_ticks_enabled) {
                        persist_q.push([pg, asset, px=t.price, q=t.qty,
                                        buy=t.buyer_is_maker,
                                        exch_ms=t.exchange_ms]() noexcept {
                            pg->insert_binance_tick(asset + "USDT-F", px, q,
                                                   !buy, exch_ms);
                        });
                    }
                });
            binance_futures->start();
            spdlog::info("binance futures forceOrder feed: {}",
                         cfg.binance.futures_ws_url);
        }

        // Coinbase Exchange websocket ticker — THE LEADING SIGNAL. Chainlink's
        // */usd streams aggregate USD venues with Coinbase dominant, so the
        // Polymarket oracle follows Coinbase by ~1.5-3s (Jacob's live
        // observation, 2026-06-10). Feeds cb_buf (coinbase-lead sniper), the
        // TUI/cockpit price, the DB (1s/asset throttle), and oracle failover
        // (preferred over Binance when RTDS is down).
        coinbase_ws = std::make_unique<vp::CoinbaseWsFeed>(
            cfg.polymarket.coinbase_ws_url,
            std::vector<std::string>{"BTC-USD", "ETH-USD", "SOL-USD", "XRP-USD"},
            [&state, &tui, &cb_buf, &cfg, &persist_q, pg=pg.get()](const vp::AssetTick& t) {
                // "BTC-USD" -> "BTC"
                auto dash = t.symbol.find('-');
                if (dash == std::string::npos) return;
                std::string asset = t.symbol.substr(0, dash);
                using namespace std::chrono;
                int64_t now_ms = duration_cast<milliseconds>(
                    system_clock::now().time_since_epoch()).count();
                oracle_failover.last_cb_ws_ms.store(now_ms, std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> lk(state.mu);
                    state.cb_asset_price(asset) = t.price;
                    tui.conn().coinbase.store(true);
                }
                cb_buf.add(asset, (double)t.exchange_ms / 1000.0, t.price);
                // Persist at most 1 tick/s/asset (ticker can be 10+/s on BTC).
                static std::array<std::atomic<int64_t>, 4> last_db_ms{};
                auto& slot = last_db_ms[OracleFailover::idx(asset)];
                int64_t prev = slot.load(std::memory_order_relaxed);
                if (cfg.data_collection.coinbase_ticks_enabled &&
                    now_ms - prev >= 1000 &&
                    slot.compare_exchange_strong(prev, now_ms)) {
                    persist_q.push([pg, asset, price=t.price,
                                    ts_ms=t.exchange_ms]() noexcept {
                        pg->insert_coinbase_tick(asset, price, ts_ms);
                    });
                }
                // Oracle failover, tier 2: RTDS silent => Coinbase becomes the oracle.
                if (now_ms - oracle_failover.last_rtds_ms.load(std::memory_order_relaxed)
                        < kOracleFailoverMs) return;
                auto& synth_ms = oracle_failover.last_synth_ms[OracleFailover::idx(asset)];
                int64_t sprev = synth_ms.load(std::memory_order_relaxed);
                if (now_ms - sprev < kFailoverSynthGapMs) return;
                if (!synth_ms.compare_exchange_strong(sprev, now_ms)) return;
                if (!oracle_failover.active.exchange(true))
                    spdlog::warn("oracle FAILOVER ACTIVE: RTDS silent > {}s — synthesizing oracle ticks from Coinbase",
                                 kOracleFailoverMs / 1000);
                vp::OracleTick synth{asset, t.price,
                                     t.exchange_ms > 0 ? t.exchange_ms : now_ms};
                ingest_oracle_tick(synth, "coinbase_failover");
            });
        coinbase_ws->start();

        // Oracle feed (Polymarket RTDS — Chainlink prices, no auth needed)
        if (cfg.polymarket.rtds_enabled && !cfg.polymarket.rtds_ws_url.empty()) {
            spdlog::info("rtds feed: {}", cfg.polymarket.rtds_ws_url);
            rtds = std::make_unique<vp::PolymarketRtds>(
                cfg.polymarket.rtds_ws_url,
                [](const vp::OracleTick& t) {
                    using namespace std::chrono;
                    if (t.window_s == 0) oracle_failover.last_rtds_ms.store(
                        duration_cast<milliseconds>(
                            system_clock::now().time_since_epoch()).count(),
                        std::memory_order_relaxed);
                    if (t.window_s == 0 && oracle_failover.active.exchange(false))
                        spdlog::warn("oracle failover OFF — RTDS resumed, Chainlink ticks back in charge");
                    ingest_oracle_tick(t, "chainlink_rtds");
                });
            rtds->start();
        }

        // User WebSocket — live mode only (fills + order status)
        if (live_exec && !cfg.polymarket.signer_address.empty()
            && cfg.polymarket.signer_address != "0xPENDING"
            && cfg.wallet.api_key != "PENDING") {
            user_ws = std::make_unique<vp::PolymarketUserWs>(
                cfg.polymarket.clob_user_ws_url,
                cfg.wallet.api_key, cfg.wallet.api_secret, cfg.wallet.api_passphrase,
                cfg.polymarket.signer_address,
                [&positions, &state](const vp::PolymarketUserWs::FillUpdate& f) {
                    spdlog::info("WS fill: order={} tok={} {:.4f}@{:.3f}",
                        f.order_id, f.token_id.substr(0,12), f.size, f.price);
                    // Update positions from WS fill
                    {
                        std::lock_guard<std::mutex> lk(state.mu);
                        state.positions_snap = positions.snapshot();
                        state.deployed_usd   = positions.total_deployed_usd();
                        // Update P&L and win/loss counters from position tracking.
                        // Positions internally tracks all fills and computes
                        // net_pnl_usd / trade_stats from them — no need to
                        // re-derive from individual fills here.
                        state.net_pnl_usd    = positions.net_pnl_usd();
                        auto st             = positions.trade_stats();
                        state.trades_won    = st.wins;
                        state.trades_lost   = st.losses;
                    }
                });
            user_ws->start();
            tui.conn().pm_user.store(true);
        }
    });

    // Autopilot: check rule signals and submit if autopilot active
    // (runs in the refresh thread every 200ms — same frequency as TUI)
    int64_t last_autopilot_fire_ms = 0;
    int64_t last_autopilot_diag_ms = 0;
    int64_t last_prune_ms = 0;
    int     diag_sweeps_total = 0;
    int     diag_sweeps_with_signal = 0;
    int64_t diag_max_lock_ms = 0;   // max state.mu hold-time per heartbeat window (diag)
    int     diag_skip_price  = 0;   // delta OK, but price above ceil or below floor
    int     diag_skip_stale  = 0;   // signal OK, but book too stale to trust
    int     diag_skip_spread = 0;   // signal + fresh book, but spread too wide
    // ── Phase 1.3: per-code skip counters — one meaning per code. The legacy
    // diag_skip_* counters above are kept for heartbeat continuity; new code
    // populates both (skip_codes is the machine-readable source of truth).
    std::map<std::string, int> skip_codes;
    std::map<std::string, int64_t> rule_veto_last_log;
    // Per-position throttle for the timed-exit sweep (keyed by "condId|U" or "condId|D").
    std::unordered_map<std::string, int64_t> timed_exit_last_ms;

    // Coinbase REST poller — BACKUP only since the Coinbase WS ticker feed
    // (2026-06-10) became the primary Coinbase source. Skips entirely while
    // the WS is fresh; kicks in if the WS goes quiet >10s.
    std::thread coinbase_th([&] {
        while (!g_stop.load() && !tui.wants_quit()) {
            {
                using namespace std::chrono;
                int64_t now_ms = duration_cast<milliseconds>(
                    system_clock::now().time_since_epoch()).count();
                if (now_ms - oracle_failover.last_cb_ws_ms.load(std::memory_order_relaxed)
                        < 10000) {
                    for (int i = 0; i < 20 && !g_stop.load() && !tui.wants_quit(); ++i)
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
            }
            for (const auto& sym : {"BTC", "ETH", "SOL", "XRP"}) {
                std::string url = std::string("https://api.coinbase.com/v2/prices/")
                                + sym + "-USD/spot";
                auto r = vp::http::get(url, {}, 4000);
                if (r.status == 200 && !r.body.empty()) {
                    // Body: {"data":{"base":"BTC","currency":"USD","amount":"71234.56"}}
                    auto p = r.body.find("\"amount\":\"");
                    if (p != std::string::npos) {
                        p += 10;
                        auto q = r.body.find('"', p);
                        if (q != std::string::npos) {
                            try {
                                double px = std::stod(r.body.substr(p, q - p));
                                int64_t ts_ms = vp::Clock::wall_ns() / 1'000'000LL;
                                {
                                    std::lock_guard<std::mutex> lk(state.mu);
                                    state.cb_asset_price(std::string(sym)) = px;
                                    tui.conn().coinbase.store(true);
                                }
                                if (cfg.data_collection.coinbase_ticks_enabled) {
                                    persist_q.push([pg=pg.get(), asset=std::string(sym),
                                                    price=px, ts_ms]() noexcept {
                                        pg->insert_coinbase_tick(asset, price, ts_ms);
                                    });
                                }
                            } catch (...) {}
                        }
                    }
                }
            }
            for (int i = 0; i < 20 && !g_stop.load() && !tui.wants_quit(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    // Periodic refresh thread (200ms): rebuild snapshot, run autopilot.
    std::thread refresh_th([&] {
        using namespace std::chrono;
        int64_t last_pos_persist_ms    = 0;
        int64_t last_rest_discover_ms  = vp::Clock::wall_ns() / 1'000'000LL;
        int64_t last_kill_check_ms     = 0;
        int64_t last_balance_refresh_ms = 0;
        int64_t last_candle_refresh_ms = 0;
        int64_t last_cmd_check_ms      = 0;   // cockpit force-sell command poll
        int64_t last_heartbeat_ms      = 0;   // CLOB session heartbeat (live)
        std::string heartbeat_id;                    // returned by CLOB, must echo on next call
        int64_t last_cfg_check_ms      = 0;
        std::filesystem::file_time_type last_cfg_mtime{};   // zero = not yet seen
        // Track oracle seeds fetched on-demand inside settle loop (per-thread, no mutex needed).
        std::set<std::pair<std::string, int64_t>> settle_oracle_seeded;
        // Phase 4.6: rate-limiter for the "still deferred" warn per position.
        std::unordered_map<std::string, int64_t> settle_defer_warned;
        struct ResolutionCheck {
            std::future<std::optional<bool>> pending;
            int64_t retry_after_ms=0;
        };
        std::unordered_map<std::string,ResolutionCheck> venue_resolutions;
        // Anti-churn reversal tracking (this thread owns both reverse paths; no mutex needed).
        // cooldown: condition_id -> wall-ms before which we will NOT reverse again on it.
        std::unordered_map<std::string, int64_t> reverse_cooldown_until;
        // Per-(market,side) stop-loss cooldown: after a stop fires, block re-entry on that
        // exact (condition_id, side) pair for stop_loss_cooldown_secs to prevent churn spirals.
        // Key = condition_id + "|" + side_string. Lives on the autopilot thread; no mutex needed.
        std::unordered_map<std::string, int64_t> stop_cooldown_until;
        // WPRED checkpoint latch: "condition_id|frac_bp" -> window_close_ms the
        // checkpoint was consumed for. Exactly one attempt per window+fraction —
        // no retries of rejected orders, no late fires after grace expiry.
        std::unordered_map<std::string, int64_t> wpred_latch;
        // per-window reverse counter: condition_id -> {window_close_ms, count}.
        std::unordered_map<std::string, std::pair<int64_t,int>> reversals_this_window;
        // Returns true if a reverse into `cid` (closing at window_close_ms) is allowed
        // right now under the cooldown + per-window cap. Records the reverse if allowed.
        auto reverse_allowed = [&](const std::string& cid, int64_t window_close_ms,
                                   int64_t now_ms) -> bool {
            if (now_ms < reverse_cooldown_until[cid]) return false;
            auto& rw = reversals_this_window[cid];
            if (rw.first != window_close_ms) { rw.first = window_close_ms; rw.second = 0; }
            if (cfg.strategy.max_reversals_per_window > 0 &&
                rw.second >= cfg.strategy.max_reversals_per_window) return false;
            ++rw.second;
            reverse_cooldown_until[cid] =
                now_ms + (int64_t)cfg.strategy.reversal_cooldown_secs * 1000LL;
            return true;
        };
        // Returns true if `token_id`'s book updated within max_book_age_ms (fresh enough
        // to price an entry/reverse off of). Caller must NOT hold state.mu.
        auto book_fresh = [&](const std::string& token_id, int64_t now_ms) -> bool {
            std::lock_guard<std::mutex> lk(state.mu);
            auto it = state.last_book_update_ms.find(token_id);
            if (it == state.last_book_update_ms.end()) return false;
            return (now_ms - it->second) <= cfg.strategy.max_book_age_ms;
        };

        vp::Recycler recycler;
        int64_t last_recycler_log_ms=0;
        int64_t last_runtime_status_ms = 0;
        while (!g_stop.load() && !tui.wants_quit()) {
            int64_t now_ms = vp::Clock::wall_ns() / 1'000'000LL;
            if (now_ms - last_runtime_status_ms >= 2'000) {
                last_runtime_status_ms = now_ms;
                write_runtime_status(instance_label, session_id, cfg.runtime.mode,
                                     cfg.strategy.entry_order_mode,
                                     uncertain_order_halt.load(), state.autopilot.load(), state.daily_kill_fired.load(), now_ms);
            }

            // The first live minute is intentionally incomplete. Reconcile
            // with the shared archive before that omission splits the loaded
            // history and forces every forecast to warm up for 30 minutes.
            // Run outside state.mu and never fetch the multi-year archive.
            if (now_ms - last_candle_refresh_ms >= 30'000) {
                last_candle_refresh_ms = now_ms;
                candle_store.refresh_recent_from_pg(*pg, now_ms);
                now_ms = vp::Clock::wall_ns() / 1'000'000LL;
            }

            // Stamp startup time on first iteration for the startup-delay guard
            if (state.startup_ts_ms == 0) state.startup_ts_ms = now_ms;

            // ── Holding reward yield (Phase 5.5) ─────────────────────────────
            // In paper mode, simulate 4% APY on idle USDC. Idle cash =
            // wallet_baseline_usd + net_pnl_usd - deployed_usd (open_cost).
            // Yield accrues continuously to net_pnl_usd.
            if (cfg.runtime.mode == vp::TradeMode::Paper) {
                std::lock_guard<std::mutex> lk(state.mu);
                if (state.last_yield_ts_ms == 0) state.last_yield_ts_ms = now_ms;
                double dt_sec = (now_ms - state.last_yield_ts_ms) / 1000.0;
                if (dt_sec > 0) {
                    double idle_cash = state.wallet_baseline_usd
                                     + state.net_pnl_usd
                                     - state.deployed_usd;
                    if (idle_cash > 0) {
                        double yield = idle_cash * 0.04 * (dt_sec / (365.0 * 24.0 * 3600.0));
                        state.net_pnl_usd += yield;
                        state.yield_accrued_usd += yield;
                    }
                    state.last_yield_ts_ms = now_ms;
                }
            }

            // ── Prune dead (closed-window) markets ───────────────────────────
            // The market-refresh path only ADDS markets; closed windows were
            // never removed, so state.markets + the pm-md subscribe set grew
            // unbounded over a session. Past ~500 tokens Polymarket starts
            // closing the socket → reconnect storms → book-feed death. Every 30s
            // drop markets whose window closed > 3 min ago (well past settlement)
            // and that hold no open position, and unsubscribe their tokens.
            if (now_ms - last_prune_ms >
                    std::max(1, cfg.polymarket.market_prune_interval_ms)) {
                last_prune_ms = now_ms;
                const int64_t prune_grace_ms =
                    std::max(0, cfg.polymarket.market_prune_grace_ms);
                std::unordered_set<std::string> has_pos;
                for (const auto& p : positions.snapshot())
                    if (p.shares > cfg.strategy.dust_position_shares)
                        has_pos.insert(p.condition_id);
                std::vector<std::string> dead_tokens;
                {
                    std::lock_guard<std::mutex> lk(state.mu);
                    std::vector<vp::PmMarket> kept;
                    kept.reserve(state.markets.size());
                    for (auto& m : state.markets) {
                        bool closed = m.window_close_ms > 0 &&
                                      now_ms > m.window_close_ms + prune_grace_ms;
                        if (closed && !has_pos.count(m.condition_id)) {
                            dead_tokens.push_back(m.yes_token_id);
                            dead_tokens.push_back(m.no_token_id);
                            state.book_up.erase(m.condition_id);
                            state.book_down.erase(m.condition_id);
                            state.token_to_cond.erase(m.yes_token_id);
                            state.token_to_cond.erase(m.no_token_id);
                            state.token_to_side.erase(m.yes_token_id);
                            state.token_to_side.erase(m.no_token_id);
                            state.last_book_update_ms.erase(m.yes_token_id);
                            state.last_book_update_ms.erase(m.no_token_id);
                            state.pred_cache.erase(m.condition_id);
                            state.latest_pred.erase(m.condition_id);
                            state.entries_per_window.erase(m.condition_id);
                            state.entries_per_window_reset.erase(m.condition_id);
                            state.signal_persist.erase(m.condition_id + "|U");
                            state.signal_persist.erase(m.condition_id + "|D");
                        } else {
                            kept.push_back(std::move(m));
                        }
                    }
                    state.markets.swap(kept);
                }
                if (!dead_tokens.empty() && pm_md) {
                    pm_md->unsubscribe(dead_tokens);
                    spdlog::info("pruned {} dead-window markets ({} tokens) from feed",
                                 dead_tokens.size() / 2, dead_tokens.size());
                }
            }

            // ── Config hot-reload ────────────────────────────────────────────
            // Watch config/polypulse.toml's mtime; on change, reload every
            // section that this running process can safely apply without
            // rebuilding executors or reconnecting sockets.
            // Runs in this (the refresh) thread, the same thread that reads
            // cfg.strategy and calls submit(), so no extra locking is needed.
            if (now_ms - last_cfg_check_ms >=
                    std::max(1, cfg.runtime.config_hot_reload_interval_ms)) {
                last_cfg_check_ms = now_ms;
                try {
                    auto mt = std::filesystem::last_write_time(cfg_path);
                    if (mt != last_cfg_mtime) {
                        // Skip the first observation (boot): just record it.
                        if (last_cfg_mtime != std::filesystem::file_time_type{}) {
                            vp::Config fresh = vp::Config::load(cfg_path);
                            cfg.strategy = fresh.strategy;
                            cfg.risk     = fresh.risk;
                            cfg.data_collection = fresh.data_collection;
                            cfg.runtime.log_level = fresh.runtime.log_level;
                            cfg.runtime.config_hot_reload_interval_ms =
                                fresh.runtime.config_hot_reload_interval_ms;
                            cfg.runtime.cockpit_command_poll_ms =
                                fresh.runtime.cockpit_command_poll_ms;
                            cfg.runtime.wallet_balance_refresh_ms =
                                fresh.runtime.wallet_balance_refresh_ms;
                            cfg.runtime.live_heartbeat_ms =
                                fresh.runtime.live_heartbeat_ms;
                            cfg.runtime.position_snapshot_interval_ms =
                                fresh.runtime.position_snapshot_interval_ms;
                            cfg.polymarket.poll_seconds = fresh.polymarket.poll_seconds;
                            cfg.polymarket.universe_horizons_seconds =
                                fresh.polymarket.universe_horizons_seconds;
                            cfg.polymarket.universe_max_windows_per_series =
                                fresh.polymarket.universe_max_windows_per_series;
                            cfg.polymarket.universe_max_markets =
                                fresh.polymarket.universe_max_markets;
                            cfg.polymarket.universe_prewarm_ahead_minutes =
                                fresh.polymarket.universe_prewarm_ahead_minutes;
                            cfg.polymarket.market_prune_interval_ms =
                                fresh.polymarket.market_prune_interval_ms;
                            cfg.polymarket.market_prune_grace_ms =
                                fresh.polymarket.market_prune_grace_ms;
                            cfg.polymarket.closed_market_display_grace_ms =
                                fresh.polymarket.closed_market_display_grace_ms;
                            cfg.polymarket.book_health_log_interval_ms =
                                fresh.polymarket.book_health_log_interval_ms;
                            cfg.polymarket.book_health_stale_ms =
                                fresh.polymarket.book_health_stale_ms;
                            cfg.polymarket.rest_book_seed_interval_ms =
                                fresh.polymarket.rest_book_seed_interval_ms;
                            cfg.polymarket.rest_book_seed_log_interval_ms =
                                fresh.polymarket.rest_book_seed_log_interval_ms;
                            cfg.polymarket.rest_book_seed_min_age_ms =
                                fresh.polymarket.rest_book_seed_min_age_ms;
                            cfg.polymarket.blocked_horizons_seconds =
                                fresh.polymarket.blocked_horizons_seconds;
                            configure_file_logger();
                            spdlog::set_level(spdlog::level::from_str(cfg.runtime.log_level));
                            tick_buffer.set_limits(cfg.strategy.tick_buffer_max_age_us,
                                                   cfg.strategy.tick_buffer_max_entries);
                            candle_store.set_max_bars_per_asset_timeframe(
                                cfg.data_collection.candle_store_max_bars_per_asset_timeframe);
                            // If the predictor/trend filter was just enabled and we
                            // skipped the history bootstrap at startup to save memory,
                            // load it now (one-time, blocking — acceptable since the
                            // operator explicitly turned a candle consumer on).
                            if ((cfg.strategy.recycler_mode!="off" ||
                                 cfg.strategy.predictor_mode ||
                                 cfg.strategy.trend_filter_enabled ||
                                 cfg.strategy.window_predictor_mode || cfg.strategy.digital_fv_mode ||
                                 cfg.strategy.oracle_lag_mode || cfg.strategy.endgame_mode) &&
                                !candle_store.is_bootstrapped()) {
                                spdlog::warn("candle_store: Recycler/predictor "
                                             "enabled at runtime — bootstrapping "
                                             "history now (one-time)");
                                candle_store.bootstrap_from_pg(*pg);
                            }
                            trade_flow.set_max_records_per_symbol(
                                cfg.data_collection.trade_flow_max_records_per_symbol);
                            apply_runtime_limits("hot-reload");
                            double applied_max_deployed = 0.0;
                            {
                                std::lock_guard<std::mutex> lk(state.mu);
                                applied_max_deployed = state.runtime_max_deployed_usd;
                            }
                            spdlog::warn("config hot-reloaded: per_click=${:.2f} size=[{:.2f},{:.2f}] "
                                         "equity_sizing={} order_pct={:.1f}% per_window=${:.2f} "
                                         "max_order=${:.2f} max_deployed=${:.2f} oracle_dpct={:.3f} "
                                         "pred_edge={:.3f} stop_loss={:.0f}% price=[{:.2f},{:.2f}] "
                                         "same_side={} windows/series={} max_markets={} prewarm={}m",
                                         cfg.strategy.per_click_size_usd, cfg.strategy.min_size_usd,
                                         cfg.strategy.max_size_usd,
                                         cfg.strategy.equity_sizing_enabled ? "ON" : "OFF",
                                         cfg.strategy.equity_order_pct * 100.0,
                                         cfg.strategy.max_per_window_usd,
                                         cfg.risk.max_order_usd, applied_max_deployed,
                                         cfg.strategy.min_oracle_delta_pct, cfg.strategy.min_predictor_edge,
                                         cfg.strategy.stop_loss_pct,
                                         cfg.strategy.min_entry_share_price,
                                         cfg.strategy.oracle_max_token_price,
                                         cfg.strategy.max_same_side_positions,
                                         cfg.polymarket.universe_max_windows_per_series,
                                         cfg.polymarket.universe_max_markets,
                                         cfg.polymarket.universe_prewarm_ahead_minutes);
                        }
                        last_cfg_mtime = mt;
                    }
                } catch (const std::exception& e) {
                    // Malformed edit or transient read — keep the running config.
                    spdlog::error("config hot-reload skipped (kept current): {}", e.what());
                }
            }

            // ── Cockpit commands ─────────────────────────────────────────────
            // The read-only web cockpit (scripts/cockpit.py) drops small JSON
            // files into control/: force-sell of an open position (legacy) and
            // manual_buy (Phase 4.5). Poll + execute as HUMAN_OVERRIDE, then
            // delete the file.
            //
            // ── Phase 4.5: instance gating ──
            // Commands may carry "instance": "<label>". Only the targeted bot
            // consumes the file; non-target bots SKIP WITHOUT deleting so the
            // intended instance still finds it on its next poll. (Previously
            // any instance ate any command — instance A could steal
            // instance B's force-sell before B even saw it.)
            if (now_ms - last_cmd_check_ms >=
                    std::max(1, cfg.runtime.cockpit_command_poll_ms)) {
                last_cmd_check_ms = now_ms;
                std::error_code ec;
                const std::filesystem::path cmd_dir = "control";
                if (std::filesystem::exists(cmd_dir, ec)) {
                    for (auto& de : std::filesystem::directory_iterator(cmd_dir, ec)) {
                        const auto& fp = de.path();
                        if (fp.extension() != ".json") continue;
                        std::string body, line;
                        { std::ifstream f(fp); while (std::getline(f, line)) body += line; }
                        const auto command = vp::parse_cockpit_command(body);
                        if (!command) {
                            spdlog::warn("cockpit command rejected: invalid typed payload in {}", fp.filename().string());
                            std::filesystem::rename(fp, fp.string()+".invalid", ec);
                            continue;
                        }
                        // Instance gate — PARSE FIRST, delete only when ours.
                        const std::string target = command->instance;
                        if (!target.empty() && target != instance_label) {
                            spdlog::debug("cockpit cmd: not for us (target='{}') — leaving {}",
                                          target, fp.string());
                            continue;
                        }
                        std::filesystem::remove(fp, ec);   // consume once — we own this command

                        const std::string op = command->op;

                        if (op == "force_sell") {
                            const int64_t command_now_ms = vp::Clock::wall_ns() / 1'000'000LL;
                            if (command->ts_ms <= 0 || command->ts_ms > command_now_ms + 5'000 ||
                                command_now_ms - command->ts_ms > 30'000 ||
                                command->expected_mode != state.mode_label) {
                                spdlog::warn("cockpit force_sell expired or mode changed for {}",
                                             instance_label);
                                continue;
                            }
                        }

                        if (op == "cancel_open_orders") {
                            const auto n = live_exec ? live_exec->open_order_count()
                                                     : paper->open_order_count();
                            if (live_exec) live_exec->cancel_open_orders();
                            else           paper->cancel_open_orders();
                            spdlog::warn("cockpit CANCEL-OPEN-ORDERS instance={} count={}",
                                         instance_label, n);
                            continue;
                        }

                        if (op == "cancel_order") {
                            const int64_t command_now_ms = vp::Clock::wall_ns() / 1'000'000LL;
                            if (command->ts_ms <= 0 || command->ts_ms > command_now_ms + 5'000 ||
                                command_now_ms - command->ts_ms > 30'000 ||
                                command->expected_mode != state.mode_label) {
                                spdlog::warn("cockpit cancel-order expired or mode changed for {}",
                                             instance_label);
                                continue;
                            }
                            bool cancelled = false;
                            if (state.mode_label == "LIVE" && live_exec) {
                                cancelled = live_exec->cancel_open_order(
                                    command->exchange_order_id, command->client_order_id);
                            } else if (state.mode_label == "PAPER" &&
                                       command->exchange_order_id == command->client_order_id) {
                                cancelled = paper->cancel_order(command->client_order_id);
                            }
                            if (cancelled) {
                                spdlog::warn("cockpit ORDER-CANCELLED instance={} client_id={}",
                                             instance_label, command->client_order_id);
                            } else {
                                spdlog::warn("cockpit order cancellation was not acknowledged instance={} client_id={}",
                                             instance_label, command->client_order_id);
                            }
                            continue;
                        }

                        // ── manual_buy (Phase 4.5) ──
                        if (op == "autopilot") {
                            // Cockpit live control: flip the running instance's
                            // autopilot without a restart (same atomic the TUI
                            // toggle flips). Consumed once, instance-targeted.
                            const bool on = command->enabled;
                            if (on && (uncertain_order_halt.load() || state.daily_kill_fired.load())) {
                                spdlog::error("cockpit command: autopilot ON refused; unresolved order or session loss limit requires reconciliation/reset");
                                continue;
                            }
                            state.autopilot.store(on);
                            spdlog::info("cockpit command: autopilot {} (source=HUMAN_OVERRIDE)",
                                         on ? "ON" : "OFF");
                            continue;
                        }
                        if (op == "manual_buy") {
                            const std::string cid = command->condition_id;
                            const std::string side_s = command->side;
                            const double size_usd = command->size_usd;
                            const bool generic_market = !command->token_id.empty();
                            const bool is_sell = command->direction == "SELL";
                            const vp::OrderDir direction = is_sell
                                ? vp::OrderDir::Sell : vp::OrderDir::Buy;
                            const vp::ShareSide side =
                                (side_s == "UP") ? vp::ShareSide::UP : vp::ShareSide::DOWN;
                            const int64_t command_now_ms = vp::Clock::wall_ns() / 1'000'000LL;
                            vp::OrderRequest req;
                            req.condition_id = cid;
                            req.side = side;
                            req.dir = direction;
                            req.size_usd = size_usd;
                            req.size_shares = command->size_shares;
                            req.limit_price = command->limit_price;
                            req.source = "HUMAN_OVERRIDE";
                            req.asset = generic_market ? "MANUAL" : "";
                            req.token_id = command->token_id;
                            req.manual_market_question = command->question;
                            req.outcome_label = command->outcome_label;
                            req.neg_risk = cfg.wallet.neg_risk;
                            req.tick_size = 0.01;
                            auto reject_manual = [&](const std::string& reason) {
                                vp::OrderResult rejected;
                                rejected.client_order_id = vp::core::make_uuid();
                                rejected.status = "rejected";
                                rejected.terminal = true;
                                rejected.reject_class = vp::OrderResult::RejectClass::RiskCap;
                                rejected.reject_reason = reason;
                                if (journal.on_order) journal.on_order(req, rejected);
                                spdlog::warn("cockpit manual order rejected for {}: {}",
                                             instance_label, reason);
                            };
                            if (command->ts_ms > command_now_ms + 5'000 ||
                                command_now_ms - command->ts_ms > 30'000) {
                                reject_manual("manual order request expired before execution");
                                continue;
                            }
                            if (command->expected_mode != state.mode_label) {
                                reject_manual("runtime mode changed after order review");
                                continue;
                            }
                            if (uncertain_order_halt.load() || state.daily_kill_fired.load()) {
                                reject_manual("bot halted for order reconciliation or its session loss limit");
                                continue;
                            }
                            if (cid.empty() || (side_s != "UP" && side_s != "DOWN") ||
                                command->limit_price < 0.01 || command->limit_price > 0.99 ||
                                (is_sell && (!generic_market || command->size_shares <= 0.0)) ||
                                (!is_sell && (size_usd < cfg.risk.min_order_usd ||
                                              size_usd > cfg.risk.max_order_usd))) {
                                reject_manual("manual order no longer satisfies the bot's size or payload limits");
                                continue;
                            }
                            std::shared_ptr<vp::OrderBook> selected_book;
                            double quote = 0.0;
                            double tick_size = 0.01;
                            bool have_market = false;
                            if (generic_market) {
                                const auto books = rest.fetch_orderbooks({command->token_id});
                                const int64_t fetch_now_ms = vp::Clock::wall_ns() / 1'000'000LL;
                                const auto update = std::find_if(books.begin(), books.end(),
                                    [&](const vp::PolymarketMD::BookUpdate& book) {
                                        return book.token_id == command->token_id &&
                                               book.condition_id == cid;
                                    });
                                if (update == books.end() || update->exchange_ms <= 0 ||
                                    update->exchange_ms > fetch_now_ms + 5'000 ||
                                    fetch_now_ms - update->exchange_ms > 15'000) {
                                    reject_manual("bot could not fetch a fresh matching order book");
                                    continue;
                                }
                                std::lock_guard<std::mutex> lk(state.mu);
                                auto existing_condition = state.token_to_cond.find(command->token_id);
                                auto existing_side = state.token_to_side.find(command->token_id);
                                if ((existing_condition != state.token_to_cond.end() &&
                                     existing_condition->second != cid) ||
                                    (existing_side != state.token_to_side.end() &&
                                     existing_side->second != side)) {
                                    reject_manual("outcome token is already mapped to another market");
                                    continue;
                                }
                                auto& bookmap = side == vp::ShareSide::UP
                                    ? state.book_up : state.book_down;
                                auto& slot = bookmap[cid];
                                if (!slot) slot = std::make_shared<vp::OrderBook>();
                                selected_book = slot;
                                selected_book->apply_snapshot(update->bids, update->asks);
                                state.token_to_cond[command->token_id] = cid;
                                state.token_to_side[command->token_id] = side;
                                state.last_book_update_ms[command->token_id] = update->exchange_ms;
                                have_market = true;
                            } else {
                                if (is_sell) {
                                    reject_manual("crypto prediction-board markets support the existing close flow");
                                    continue;
                                }
                                std::lock_guard<std::mutex> lk(state.mu);
                                auto mit = std::find_if(state.markets.begin(), state.markets.end(),
                                    [&](const vp::PmMarket& market) { return market.condition_id == cid; });
                                if (mit != state.markets.end()) {
                                    req.asset = mit->asset;
                                    req.horizon_s = mit->horizon_seconds;
                                    req.token_id = side == vp::ShareSide::UP
                                        ? mit->yes_token_id : mit->no_token_id;
                                    req.neg_risk = mit->neg_risk;
                                    req.window_close_ms = mit->window_close_ms;
                                    tick_size = mit->tick_size > 0 ? mit->tick_size : 0.01;
                                    auto& bookmap = side == vp::ShareSide::UP
                                        ? state.book_up : state.book_down;
                                    auto bit = bookmap.find(cid);
                                    if (bit != bookmap.end()) selected_book = bit->second;
                                    auto update = state.last_book_update_ms.find(req.token_id);
                                    have_market = selected_book && update != state.last_book_update_ms.end() &&
                                        command_now_ms - update->second <= 15'000 &&
                                        command_now_ms - update->second >= -5'000;
                                }
                            }
                            if (!have_market || !selected_book) {
                                reject_manual("no fresh book is available for the selected outcome");
                                continue;
                            }
                            if (generic_market && pm_md) pm_md->subscribe({command->token_id});
                            const auto top = is_sell ? selected_book->best_bid()
                                                     : selected_book->best_ask();
                            quote = top.price;
                            if (quote <= 0.0 || top.size <= 0.0 ||
                                (is_sell ? command->limit_price > quote + 1e-9
                                         : command->limit_price + 1e-9 < quote)) {
                                reject_manual(is_sell
                                    ? "fresh bid is below the selected minimum sell price"
                                    : "fresh ask is above the selected maximum buy price");
                                continue;
                            }
                            // Snap BUY limits down so rounding can never exceed
                            // the approved maximum; sells stay at the requested
                            // minimum and are checked again by the executor.
                            const double px = is_sell ? command->limit_price
                                : std::floor(command->limit_price / tick_size + 1e-9) * tick_size;
                            req.limit_price = px;
                            req.tick_size = tick_size;
                            spdlog::warn("cockpit MANUAL-{} {} {} {} @limit {:.3f}{} "
                                         "(HUMAN_OVERRIDE)",
                                         is_sell ? "SELL" : "BUY",
                                         req.asset.empty() ? cid.substr(0, 12) : req.asset,
                                         command->outcome_label.empty() ? side_s : command->outcome_label,
                                         is_sell ? req.size_shares : req.size_usd,
                                         px, is_sell ? " shares" : " USD");
                            do_submit(req);
                            continue;
                        }

                        // ── legacy force_sell ──
                        std::string cid = command->condition_id, side_s = command->side;
                        if (cid.empty() || (side_s != "UP" && side_s != "DOWN")) {
                            spdlog::warn("cockpit cmd: ignoring malformed {}", fp.string());
                            continue;
                        }
                        vp::ShareSide side = (side_s == "UP") ? vp::ShareSide::UP : vp::ShareSide::DOWN;
                        bool found = false;
                        for (const auto& pos : positions.snapshot()) {
                            if (pos.condition_id != cid || pos.side != side ||
                                pos.shares < cfg.strategy.dust_position_shares) continue;
                            found = true;
                            vp::OrderRequest req;
                            req.condition_id  = pos.condition_id;
                            req.asset         = pos.asset;
                            req.horizon_s     = pos.horizon_s;
                            req.token_id      = pos.token_id;
                            req.side          = pos.side;
                            req.dir           = vp::OrderDir::Sell;
                            req.size_shares   = 0;            // sell the whole position
                            req.source        = "HUMAN_OVERRIDE";
                            req.window_close_ms = pos.window_close_ms;
                            {
                                std::lock_guard<std::mutex> lk(state.mu);
                                auto mit = std::find_if(state.markets.begin(), state.markets.end(),
                                    [&](const vp::PmMarket& m){ return m.condition_id == cid; });
                                if (mit != state.markets.end()) { req.neg_risk = mit->neg_risk; req.tick_size = mit->tick_size; }
                            }
                            spdlog::warn("cockpit FORCE-SELL {} {} {:.4f}sh @market (HUMAN_OVERRIDE)",
                                         pos.asset, vp::to_string(pos.side), pos.shares);
                            do_submit(req);
                            break;
                        }
                        if (!found)
                            spdlog::warn("cockpit force_sell: no open {} position for {}", side_s, cid.substr(0, 12));
                    }
                }
            }

            // Sync ScalpManager config from hot-reloadable strategy settings.
            // Without this, the ScalpManager always uses its compile-time
            // defaults and the TOML scalp_v2_* values are ignored.
            {
                auto& sc = scalp_manager.cfg;
                auto& ss = cfg.strategy;
                const bool scalp_v3 = ss.scalp_v3_enabled;
                const bool scalp_enabled = scalp_v3 || ss.scalp_v2_enabled;
                sc.enabled                = scalp_enabled;
                sc.velocity_thresh        = scalp_v3 ? ss.scalp_v3_velocity_thresh : ss.scalp_v2_velocity_thresh;
                sc.accel_min              = scalp_v3 ? ss.scalp_v3_accel_min : -1.0;
                sc.dominance_thresh       = scalp_v3 ? ss.scalp_v3_dominance_thresh : 0.0;
                sc.min_confidence         = scalp_v3 ? ss.scalp_v3_min_confidence : 0.0;
                sc.min_price              = scalp_v3 ? ss.scalp_v3_min_price : ss.scalp_v2_min_price;
                sc.max_price              = scalp_v3 ? ss.scalp_v3_max_price : ss.scalp_v2_max_price;
                sc.min_secs               = scalp_v3 ? ss.scalp_v3_min_secs : ss.scalp_v2_min_secs;
                sc.max_secs               = scalp_v3 ? ss.scalp_v3_max_secs : ss.scalp_v2_max_secs;
                sc.max_entries_per_window = scalp_v3 ? ss.scalp_v3_max_entries_per_window : ss.scalp_v2_max_entries_per_window;
                sc.momentum_reversal_exit = scalp_v3
                    ? ss.scalp_v3_reversal_exit
                    : (scalp_enabled && ss.scalp_v2_reversal_exit);
                sc.reversal_vel_thresh    = scalp_v3 ? ss.scalp_v3_reversal_thresh : ss.scalp_v2_reversal_thresh;
                sc.reversal_window_us     = scalp_v3 ? ss.scalp_v3_reversal_window_us : ss.scalp_v2_reversal_window_us;
                sc.reversal_min_profit    = scalp_v3 ? ss.scalp_v3_reversal_min_profit : 0.0;
                sc.min_hold_us            = (int64_t)(scalp_v3 ? ss.scalp_v3_min_hold_ms
                                                               : ss.scalp_v2_min_hold_ms) * 1000LL;
                sc.trailing_stop          = scalp_v3
                    ? ss.scalp_v3_trailing_stop
                    : (scalp_enabled && ss.scalp_v2_trailing_stop);
                sc.trail_start_loss       = scalp_v3 ? ss.scalp_v3_trail_start_loss : ss.scalp_v2_trail_start_loss;
                sc.trail_ratchet_pct      = scalp_v3 ? ss.scalp_v3_trail_ratchet_pct : ss.scalp_v2_trail_ratchet_pct;
                sc.trail_min_distance     = scalp_v3 ? ss.scalp_v3_trail_min_distance : ss.scalp_v2_trail_min_distance;
                sc.trail_stop_pct         = scalp_v3 ? ss.scalp_v3_trail_stop_pct     : 0.0;
                sc.profit_take            = scalp_v3
                    ? ss.scalp_v3_profit_take
                    : (scalp_enabled && ss.scalp_v2_profit_take);
                sc.profit_target          = scalp_v3 ? ss.scalp_v3_profit_target     : ss.scalp_v2_profit_target;
                sc.profit_target_pct      = scalp_v3 ? ss.scalp_v3_profit_target_pct : 0.0;
                sc.per_entry_usd          = scalp_v3 ? ss.scalp_v3_per_entry_usd : ss.scalp_v2_per_entry_usd;
                sc.cooldown_us            = scalp_v3 ? (int64_t)ss.scalp_v3_cooldown_ms * 1000LL : 0LL;
                sc.runner_enabled         = scalp_v3 && ss.scalp_v3_runner_enabled;
                sc.runner_min_bid         = ss.scalp_v3_runner_min_bid;
                sc.runner_min_profit      = ss.scalp_v3_runner_min_profit;
                sc.runner_max_secs_left   = ss.scalp_v3_runner_max_secs_left;
                sc.runner_velocity_floor  = ss.scalp_v3_runner_velocity_floor;
            }

            vp::TuiSnapshot snap;
            {
                std::lock_guard<std::mutex> lk(state.mu);
                // Sync the highlighted watchlist row → active_index so that
                // the center panel and trading keys (B/N/S/M) operate on
                // whichever market the user has selected with ↑/↓.
                state.active_index = tui.selected_row();
                vp::rebuild_snapshot(state, cfg.strategy, cfg.polymarket,
                                     oracle_buf, cb_buf, candle_store, trade_flow,
                                     pm_flow, tick_buffer, snap);
            }
            tui.update_snapshot(snap);

            // ── Persist feature snapshots (predictor output) for each active market ──
            // Includes full PredictorParts: RSI, MACD, BB, tech_score, oracle delta, etc.
            // Throttled by data_collection.features_write_interval_ms (default 1s).
            if (cfg.data_collection.features_enabled) {
                static thread_local int64_t last_feature_write_ms = 0;
                if (now_ms - last_feature_write_ms >= cfg.data_collection.features_write_interval_ms) {
                    last_feature_write_ms = now_ms;
                    auto sanitize = [](double v) -> double {
                        return (std::isnan(v) || std::isinf(v)) ? 0.0 : v;
                    };
                    for (const auto& row : snap.watchlist) {
                        if (row.condition_id.empty()) continue;
                        std::string features_json = fmt::format(
                            R"({{"pred_up":{},"pred_fair_up":{},"pred_edge_up":{},"pred_edge_down":{},)"
                            R"("pred_conf":{},"rsi14":{},"macd_hist":{},"bb_z":{},"tech_score":{},)"
                            R"("oracle_delta_pct":{},"up_price":{},"down_price":{},)"
                            R"("rv_30d":{},"rv_90d":{},"rv_1y":{},)"
                            R"("slope_7d_pct":{},"slope_30d_pct":{},"slope_1y_pct":{},)"
                            R"("history_regime_score":{},"asset_price":{}}})",
                            sanitize(row.pred_up), sanitize(row.pred_fair_up),
                            sanitize(row.pred_edge_up), sanitize(row.pred_edge_down),
                            sanitize(row.pred_conf), sanitize(row.pred_rsi14),
                            sanitize(row.pred_macd_hist), sanitize(row.pred_bb_z),
                            sanitize(row.pred_tech_score), sanitize(row.oracle_delta_pct),
                            sanitize(row.up_price), sanitize(row.down_price),
                            sanitize(row.pred_rv_30d), sanitize(row.pred_rv_90d),
                            sanitize(row.pred_rv_1y),
                            sanitize(row.pred_slope_7d_pct),
                            sanitize(row.pred_slope_30d_pct),
                            sanitize(row.pred_slope_1y_pct),
                            sanitize(row.pred_history_regime_score),
                            sanitize(row.asset_price));
                        persist_q.push([pg=pg.get(), cid=row.condition_id,
                                        j=std::move(features_json)]() noexcept {
                            pg->insert_feature(cid, j, 1);
                        });
                    }
                }
            }

            // Autopilot: if enabled and not kill-switched, fire on signals
            if (state.autopilot.load() && cfg.strategy.recycler_mode == "off" && !recycler.engaged()) {
                // Startup delay: wait for oracle ticks and window info before trading
                int64_t elapsed_s = (now_ms - state.startup_ts_ms) / 1000;
                if (elapsed_s < cfg.strategy.autopilot_startup_delay_secs) {
                    if ((now_ms - state.last_startup_log_ms) > 30000) {
                        state.last_startup_log_ms = now_ms;
                        spdlog::info("autopilot: startup delay — {}/{}s before first trade",
                                     elapsed_s, cfg.strategy.autopilot_startup_delay_secs);
                    }
                } else {
                    int64_t throttle = cfg.strategy.autopilot_throttle_ms;
                        bool paused = (now_ms < state.pause_until_ms);
                        if (!paused && (now_ms - last_autopilot_fire_ms) >= throttle) {
                    last_autopilot_fire_ms = now_ms;  // always advance; prevents re-entry until next throttle tick
                    // Fire on EVERY qualifying market in a single sweep, not just the first one.
                    // This is critical: different markets have different oracle deltas, and we
                    // want to catch them all. The throttle prevents spam, but within one sweep
                    // we process all markets.
                    struct DeferredTrade {
                        vp::OrderRequest req;
                        vp::SignalOut sig;
                        bool from_scalp = false;
                        double rank_score = 0.0;
                    };
                    std::vector<DeferredTrade> deferred_trades;
                    // WPRED checkpoint orders, collected under the lock below and
                    // submitted after it releases (do_submit takes state.mu).
                    struct WpredTrade {
                        vp::OrderRequest req;
                        double frac = 0;
                        double conf = 0;
                        double ask  = 0;
                        std::string reason;
                        std::string model_version;
                        std::string payload;
                        std::string cal_key;   // WPRED calibration bucket
                        std::string pos_key;   // condition_id|side for settle lookup
                        double cal_p   = 0;    // predicted side probability
                    };
                    std::vector<WpredTrade> wpred_trades;
                    // Purge latches for windows settled >1h ago (bounded growth).
                    for (auto it = wpred_latch.begin(); it != wpred_latch.end();) {
                        if (now_ms - it->second > 3'600'000) it = wpred_latch.erase(it);
                        else ++it;
                    }
                    std::unordered_set<std::string> traded_this_sweep;  // prevent duplicate condition_ids
                    double max_delta_pct = 0.0;  // diagnostics: track best oracle delta this sweep
                    int sweep_rows = 0, sweep_skip_open = 0, sweep_skip_ref = 0, sweep_skip_twap = 0;  // prefilter funnel
                    // Snapshot current positions to avoid doubling up in same market
                    auto open_pos = positions.snapshot();
                    int64_t lk_t0 = vp::Clock::wall_ns() / 1'000'000LL;  // diag: lock-hold timer
                    // Per-sweep predict() budget — hard-bounds how long state.mu is
                    // held (each predict() copies a year of candles + runs the full
                    // ensemble). Markets over budget reuse their cached ensemble.
                    int predict_budget = cfg.strategy.predict_budget_per_sweep;
                    {
                        std::lock_guard<std::mutex> lk(state.mu);
                        for (const auto& m : state.markets) {
                            // Skip markets that haven't opened yet — oracle delta is meaningless
                            if (m.window_open_ms <= 0 || m.window_open_ms > now_ms) { ++sweep_skip_open; continue; }
                            const auto reference_key = m.reference_key();
                            const double reference_age = now_ms / 1000.0 - oracle_buf.latest_timestamp(reference_key);
                            const double open_ref =
                                oracle_buf.lookup_at_or_before(reference_key, m.window_open_ms / 1000.0);
                            if (m.settlement_window_s < 0 || reference_age < -1 || reference_age > 10 ||
                                open_ref <= 0) {
                                // Once per (market, window): these sweep-level
                                // skips are otherwise completely invisible —
                                // this is how the whole fleet went silent on
                                // Sep 06 without a single log line.
                                static std::unordered_map<std::string, std::pair<int64_t, int>> skip_seen;
                                const std::string sk =
                                    fmt::format("{}|{}", m.condition_id, m.window_close_ms);
                                auto& sv = skip_seen[sk];
                                if (sv.first != m.window_close_ms) sv = {m.window_close_ms, 0};
                                if (sv.second == 0) {
                                    sv.second = 1;
                                    spdlog::info(
                                        "sweep skip {}: sws={} ref_age={:.1f}s open_ref={:.2f} key={}",
                                        m.condition_id.substr(0, 10),
                                        m.settlement_window_s, reference_age,
                                        open_ref, reference_key);
                                }
                                ++sweep_skip_ref;
                                continue;
                            }
                            // The final averaging interval requires a conditional
                            // TWAP model; do not reuse the spot endgame formula.
                            if (m.settlement_window_s > 0 &&
                                m.window_close_ms - now_ms <= m.settlement_window_s * 1000LL) {
                                ++sweep_skip_twap;
                                continue;
                            }
                            // Note: We allow re-entry for momentum scalping after position closure
                            // The position limit check is removed to enable multiple entries per window
                            ++sweep_rows;
                            vp::WatchlistRow row;
                            row.condition_id    = m.condition_id;
                            row.asset           = m.asset;
                            row.horizon_s       = m.horizon_seconds;
                            row.window_close_ms = m.window_close_ms;
                            row.target_price    = m.target_price;
                            row.asset_price     = state.asset_price(m.asset);
                            // ── Binance bookTicker confirmation fields (Phase 6.3) ──
                            if (auto bti = state.book_ticker.find(m.asset);
                                bti != state.book_ticker.end()) {
                                const auto& bt = bti->second;
                                if (bt.mid_hist.size() >= 2) {
                                    const auto& back = bt.mid_hist.back();
                                    for (auto rit = bt.mid_hist.rbegin();
                                         rit != bt.mid_hist.rend(); ++rit) {
                                        if (back.first - rit->first >= 2000) {
                                            if (rit->second > 0)
                                                row.bt_mid_delta_pct =
                                                    100.0 * (back.second - rit->second) / rit->second;
                                            break;
                                        }
                                    }
                                }
                                double sz = bt.bid_q + bt.ask_q;
                                if (sz > 0 && bt.bid > 0 && bt.ask > 0)
                                    row.bt_imbalance = (bt.bid_q - bt.ask_q) / sz;
                            }
                            // ── Oracle micro-dynamics for digital FV (Phase 5.1) ──
                            // ── Futures liquidation pressure: 30s rolling net notional (Phase 4.1) ──
                            if (auto lit = state.liq_events.find(m.asset);
                                lit != state.liq_events.end()) {
                                double net = 0.0;
                                for (auto rit = lit->second.rbegin();
                                     rit != lit->second.rend(); ++rit) {
                                    if (now_ms - rit->first > 30'000) break;
                                    net += rit->second;
                                }
                                row.liq_pressure_usd = net;
                            }
                            // oracle_delta_pct = (oracle_now - oracle_at_window_open) / oracle_at_window_open
                            {
                                double window_open_ts_s = (double)m.window_open_ms / 1000.0;
                                double oracle_now     = oracle_buf.latest(m.reference_key());
                                double oracle_at_open = oracle_buf.lookup_at_or_before(m.reference_key(), window_open_ts_s);
                                if (oracle_at_open <= 0)
                                    oracle_at_open = 0; // wait for the actual opening reference
                                if (oracle_at_open > 0 && oracle_now > 0)
                                    row.oracle_delta_pct = 100.0 * (oracle_now - oracle_at_open) / oracle_at_open;
                                else
                                    row.oracle_delta_pct = 0.0;
                                // The digital fair-value path needs the raw
                                // Chainlink strike/current values. The TUI row
                                // populated these already, but the autopilot row
                                // previously left both at zero, silently disabling
                                // digital-FV entries in headless trading.
                                row.coin_open_price = oracle_at_open;
                                row.coin_now_price  = oracle_now;

                                // Populate the Coinbase move on the trading row as
                                // well as the TUI row. This was previously omitted
                                // from the autopilot sweep, silently disabling CB
                                // voting/veto logic in actual decisions.
                                double cb_now = cb_buf.latest(m.asset);
                                double cb_at_open = cb_buf.lookup_at_or_before(
                                    m.asset, window_open_ts_s);

                                if (cb_at_open > 0 && cb_now > 0)
                                    row.cb_delta_pct = 100.0 * (cb_now - cb_at_open)
                                        / cb_at_open;
                                else
                                    row.cb_delta_pct = 0.0;
                            }
                            // Fallback: if book is empty (e.g. WS reconnect, new window
                            // not yet seeded), use the last known price from history.
                            auto book_up_it = state.book_up.find(m.condition_id);
                            if (book_up_it != state.book_up.end() && book_up_it->second) {
                                auto bb = book_up_it->second->best_bid(),
                                     ba = book_up_it->second->best_ask();
                                row.up_bid = bb.price;
                                row.up_ask = ba.price;
                                row.up_price = (bb.price + ba.price) > 0
                                    ? 0.5*(bb.price+ba.price) : (bb.price>0?bb.price:ba.price);
                            }
                            if (row.up_price <= 0) {
                                auto& hist = state.up_price_hist[m.condition_id];
                                if (!hist.empty()) row.up_price = hist.back().second;
                            }
                            auto book_dn_it = state.book_down.find(m.condition_id);
                            if (book_dn_it != state.book_down.end() && book_dn_it->second) {
                                auto bb = book_dn_it->second->best_bid(),
                                     ba = book_dn_it->second->best_ask();
                                row.down_bid = bb.price;
                                row.down_ask = ba.price;
                                row.down_price = (bb.price + ba.price) > 0
                                    ? 0.5*(bb.price+ba.price) : (bb.price>0?bb.price:ba.price);
                            }
                            if (row.down_price <= 0) {
                                auto& dhist = state.down_price_hist[m.condition_id];
                                if (!dhist.empty()) row.down_price = dhist.back().second;
                            }
                            // Velocity: change over last 10s (same logic as rebuild_snapshot)
                            {
                                auto& hist = state.up_price_hist[m.condition_id];
                                if (row.up_price > 0) {
                                    hist.push_back({now_ms, row.up_price});
                                    while ((int)hist.size() >
                                               std::max(1, cfg.strategy.price_history_max_points))
                                        hist.erase(hist.begin());
                                }
                                if (hist.size() >= 2) {
                                    const auto& back = hist.back();
                                    for (auto rit = hist.rbegin(); rit != hist.rend(); ++rit) {
                                        if (back.first - rit->first >=
                                                std::max(1, cfg.strategy.price_velocity_lookback_ms)) {
                                            double dt = (back.first - rit->first) / 1000.0;
                                            row.up_velocity = (back.second - rit->second) / dt;
                                            break;
                                        }
                                    }
                                }
                            }
                            // DOWN velocity: change over last 10s
                            {
                                auto& dhist = state.down_price_hist[m.condition_id];
                                if (row.down_price > 0) {
                                    dhist.push_back({now_ms, row.down_price});
                                    while ((int)dhist.size() >
                                               std::max(1, cfg.strategy.price_history_max_points))
                                        dhist.erase(dhist.begin());
                                }
                                if (dhist.size() >= 2) {
                                    const auto& back = dhist.back();
                                    for (auto rit = dhist.rbegin(); rit != dhist.rend(); ++rit) {
                                        if (back.first - rit->first >=
                                                std::max(1, cfg.strategy.price_velocity_lookback_ms)) {
                                            double dt = (back.first - rit->first) / 1000.0;
                                            row.down_velocity = (back.second - rit->second) / dt;
                                            break;
                                        }
                                    }
                                }
                            }
                            // Run multi-signal predictor on this candidate so
                            // rules::evaluate can fire on ensemble edge as well
                            // as oracle-lag. THROTTLED: recompute the (expensive)
                            // ensemble only when the per-market cache is older than
                            // predictor_refresh_ms and a per-sweep budget remains;
                            // otherwise reuse the cached output. This bounds the
                            // state.mu hold time so on_book / book updates aren't
                            // starved (the orderbook-freeze root cause).
                            {
                                int64_t cache_age = INT64_MAX;
                                auto pcit = state.pred_cache.find(m.condition_id);
                                if (pcit != state.pred_cache.end())
                                    cache_age = now_ms - pcit->second.ts_ms;
                                bool need_refresh = cache_age > cfg.strategy.predictor_refresh_ms;
                                vp::engine::PredictorOutput pr;
                                if (need_refresh && predict_budget > 0) {
                                --predict_budget;
                                vp::engine::PredictorInput pin;
                                pin.asset       = m.asset;
                                pin.horizon_s   = m.horizon_seconds;
                                pin.secs_left   = (m.window_close_ms - now_ms) / 1000;
                                pin.asset_price = row.asset_price;
                                pin.reference_price = oracle_buf.latest(m.asset + ":SPOT");
                                pin.reference_age_s = now_ms / 1000.0 - oracle_buf.latest_timestamp(m.asset + ":SPOT");
                                pin.settlement_window_s = m.settlement_window_s;
                                pin.fee_rate = cfg.strategy.taker_fee_rate;
                                pin.fee_exponent = cfg.strategy.fee_exponent;
                                // Use the real oracle price at window open as the strike
                                double window_open_ts_s = (double)m.window_open_ms / 1000.0;
                                pin.strike      = oracle_buf.lookup_at_or_before(m.reference_key(), window_open_ts_s);
                                if (pin.strike <= 0) pin.strike = m.target_price;  // fallback
                                pin.coinbase_price = state.cb_asset_price(m.asset);
                                pin.up_mid    = row.up_price;
                                pin.down_mid  = row.down_price;
                                if (auto it = state.book_up.find(m.condition_id);
                                    it != state.book_up.end() && it->second) {
                                    auto bb = it->second->best_bid(), ba = it->second->best_ask();
                                    pin.up_bid    = bb.price; pin.up_bid_sz = bb.size;
                                    pin.up_ask    = ba.price; pin.up_ask_sz = ba.size;
                                }
                                if (auto it = state.book_down.find(m.condition_id);
                                    it != state.book_down.end() && it->second) {
                                    auto bb = it->second->best_bid(), ba = it->second->best_ask();
                                    pin.down_bid    = bb.price; pin.down_bid_sz = bb.size;
                                    pin.down_ask    = ba.price; pin.down_ask_sz = ba.size;
                                }
                                pin.oracle_vel_1m_pct = oracle_buf.velocity_roc_pct(m.reference_key(), 60);
                                pin.oracle_vel_5m_pct = oracle_buf.velocity_roc_pct(m.reference_key(), 300);
                                pin.oracle_accel_pct  = oracle_buf.acceleration_pct(m.reference_key(), 60);
                                std::string flow_sym = vp::engine::asset_to_symbol(m.asset);
                                pin.trade_flow_bias = trade_flow.buy_sell_imbalance(flow_sym, 5);
                                pin.pm_flow_bias    = pm_flow.imbalance_secs(m.condition_id, 10);
                                pin.pm_flow_enabled = cfg.strategy.window_predictor_pm_flow;
                                // Cross-asset 5m momentum (ETH↔BTC reference drift)
                                {
                                    const char* other = (m.asset == "BTC") ? "ETH" : "BTC";
                                    auto oc = candle_store.copy_recent_closes(
                                        other, vp::engine::Timeframe::OneMinute, 6);
                                    if (oc.size() >= 6 && oc[oc.size() - 6] > 0)
                                        pin.other_mom_5m_pct = 100.0
                                            * (oc.back() - oc[oc.size() - 6])
                                              / oc[oc.size() - 6];
                                }
                                // Keep hot-loop copies bounded: 1m data feeds recent
                                // indicators; long-regime features use 60m closes from
                                // the full 5-year candle import.
                                pin.closes_1m = candle_store.copy_recent_closes(
                                    m.asset, vp::engine::Timeframe::OneMinute,
                                    cfg.strategy.predictor_recent_1m_bars);
                                pin.bars_1m   = candle_store.copy_recent_bars(
                                    m.asset, vp::engine::Timeframe::OneMinute,
                                    cfg.strategy.predictor_recent_1m_ohlcv_bars);
                                // Higher timeframe data for multi-timeframe analysis
                                pin.closes_5m  = candle_store.copy_recent_closes(m.asset, vp::engine::Timeframe::FiveMinutes,
                                    cfg.strategy.predictor_5m_closes);
                                pin.closes_15m = candle_store.copy_recent_closes(m.asset, vp::engine::Timeframe::FifteenMinutes,
                                    cfg.strategy.predictor_15m_closes);
                                pin.closes_60m = candle_store.copy_recent_closes(m.asset, vp::engine::Timeframe::SixtyMinutes,
                                    cfg.strategy.predictor_long_60m_bars);
                                pin.bars_5m    = candle_store.copy_recent_bars(m.asset, vp::engine::Timeframe::FiveMinutes,
                                    cfg.strategy.predictor_5m_bars);
                                pin.bars_15m   = candle_store.copy_recent_bars(m.asset, vp::engine::Timeframe::FifteenMinutes,
                                    cfg.strategy.predictor_15m_bars);
                                pin.bars_60m   = candle_store.copy_recent_bars(m.asset, vp::engine::Timeframe::SixtyMinutes,
                                    cfg.strategy.predictor_60m_bars);
                                if (!pin.bars_1m.empty()) pin.candle_age_s = (now_ms - pin.bars_1m.back().ts_ms - 60000) / 1000.0;
                                pr = vp::engine::predict(pin);
                                state.pred_cache[m.condition_id] = { pr, now_ms };
                                } else if (pcit != state.pred_cache.end()) {
                                    pr = pcit->second.out;   // reuse cached ensemble (cheap)
                                }
                                // else: no cache yet AND over budget → neutral output
                                // (prediction=0.5, confidence=0) → predictor/oracle gates
                                // won't fire for this market until it gets computed (a
                                // sweep or two later). Safe warmup, no bad entries.

                                // Recompute the price-sensitive edges against the LIVE
                                // executable ask + fee every sweep (cheap), even when the
                                // ensemble itself is a cached value — keeps entry pricing
                                // responsive. Edges must be fee-inclusive: min_predictor_edge
                                // is calibrated against the same execution_edge cost model
                                // the oracle/endgame paths use (a mid-based edge ignores
                                // spread and taker fee and overstates the trade).
                                {
                                    const auto up_edge = vp::engine::execution_edge(
                                        pr.prediction, pr.prediction, row.up_bid,
                                        row.up_ask, cfg.strategy.taker_fee_rate,
                                        cfg.strategy.fee_exponent);
                                    const auto dn_edge = vp::engine::execution_edge(
                                        1.0 - pr.prediction, 1.0 - pr.prediction,
                                        row.down_bid, row.down_ask,
                                        cfg.strategy.taker_fee_rate,
                                        cfg.strategy.fee_exponent);
                                    pr.edge_up = up_edge.executable
                                        ? up_edge.net
                                        : pr.prediction - row.up_price;
                                    pr.edge_down = dn_edge.executable
                                        ? dn_edge.net
                                        : (1.0 - pr.prediction) - row.down_price;
                                }
                                row.pred_up        = pr.prediction;
                                row.pred_fair_up   = pr.fair_value_up;
                                row.pred_edge_up   = pr.edge_up;
                                row.pred_edge_down = pr.edge_down;
                                row.pred_conf      = pr.confidence;
                                row.pred_rsi14     = pr.parts.rsi14;
                                row.pred_macd_hist = pr.parts.macd_hist;
                                row.pred_bb_z      = pr.parts.bb_z;
                                row.pred_tech_score= pr.parts.tech_score;
                                // Stash for the momentum-flip exit sweep (P2).
                                state.latest_pred[m.condition_id] =
                                    { pr.prediction, pr.confidence };
                            }
                            // ── Trend filter (config toggle, easy revert) ──
                            // If the last N settlements were mostly UP (or DOWN),
                            // penalize the edge on the losing side to avoid buying
                            // into a reversal. Only applies when trend_filter_enabled.
                            if (cfg.strategy.trend_filter_enabled) {
                                int trend_sum = 0;
                                for (int trend_val : state.settlement_trend)
                                    trend_sum += trend_val;
                                // Positive = UP trend, negative = DOWN trend
                                // Penalize the losing predictor edge
                                if (trend_sum > 0) {
                                    row.pred_edge_down -= std::abs(trend_sum) * cfg.strategy.trend_filter_strength;
                                    row.oracle_delta_pct -= std::abs(trend_sum) * cfg.strategy.trend_filter_strength * 0.01;
                                } else if (trend_sum < 0) {
                                    row.pred_edge_up -= std::abs(trend_sum) * cfg.strategy.trend_filter_strength;
                                    row.oracle_delta_pct += std::abs(trend_sum) * cfg.strategy.trend_filter_strength * 0.01;
                                }
                            }
                            // Per-window entry cap
                            const bool scalp_active = cfg.strategy.scalp_v3_enabled ||
                                                       cfg.strategy.scalp_v2_enabled;
                            const int scalp_entry_limit = cfg.strategy.scalp_v3_enabled
                                ? cfg.strategy.scalp_v3_max_entries_per_window
                                : cfg.strategy.scalp_v2_max_entries_per_window;
                            {
                                auto& reset_ts = state.entries_per_window_reset[m.condition_id];
                                auto& counter  = state.entries_per_window[m.condition_id];
                                if (m.window_close_ms != reset_ts) {
                                    counter  = 0;
                                    reset_ts = m.window_close_ms;
                                }
                            }

                            // Freshness and reference features must exist before EVERY
                            // strategy gate, including checkpoint votes.
                            vp::engine::populate_reference_forecasts(row,m,oracle_buf,cb_buf,candle_store,now_ms);
                            // ── WPRED: window-fraction checkpoint predictor ──
                            // Selective UP/DOWN prediction at configured fractions.
                            // Records only entries that pass the configured evidence
                            // and net-payout gates. Runs under state.mu:
                            // orders queue into wpred_trades, submitted after the
                            // lock releases (do_submit takes state.mu itself).
                            if (cfg.strategy.window_predictor_mode) {
                                const auto& fr = cfg.strategy.window_predictor_fractions;
                                const auto& am = cfg.strategy.window_predictor_amounts_usd;
                                bool asset_ok = cfg.strategy.window_predictor_assets.empty() ||
                                    std::find(cfg.strategy.window_predictor_assets.begin(),
                                              cfg.strategy.window_predictor_assets.end(),
                                              m.asset) !=
                                        cfg.strategy.window_predictor_assets.end();
                                bool horizon_ok = cfg.strategy.window_predictor_horizons.empty() ||
                                    std::find(cfg.strategy.window_predictor_horizons.begin(),
                                              cfg.strategy.window_predictor_horizons.end(),
                                              m.horizon_seconds) !=
                                        cfg.strategy.window_predictor_horizons.end();
                                const bool market_ok = cfg.strategy.window_predictor_pairs.empty()
                                    ? (asset_ok && horizon_ok)
                                    : vp::engine::checkpoint_pair_allowed(
                                          cfg.strategy.window_predictor_pairs,
                                          m.asset, m.horizon_seconds);
                                if (market_ok && !fr.empty() &&
                                    m.window_open_ms > 0 &&
                                    m.window_close_ms > m.window_open_ms) {
                                    for (size_t fi = 0; fi < fr.size(); ++fi) {
                                        double f = fr[fi];
                                        if (!(f > 0.0) || !(f < 1.0)) continue;
                                        std::string latch_key = m.condition_id + "|" +
                                            std::to_string(
                                                (int64_t)std::llround(f * 10000.0));
                                        auto lit = wpred_latch.find(latch_key);
                                        if (lit != wpred_latch.end() &&
                                            lit->second == m.window_close_ms)
                                            continue;                 // already consumed
                                        int64_t cp_ms = m.window_open_ms +
                                            (int64_t)std::llround(
                                                f * (double)(m.window_close_ms -
                                                             m.window_open_ms));
                                        if (now_ms < cp_ms) continue; // not due yet
                                        // Latch BEFORE any guard: one attempt, and a
                                        // missed checkpoint never fires late (covers
                                        // restarts and mid-window config changes).
                                        wpred_latch[latch_key] = m.window_close_ms;
                                        if (m.window_close_ms - now_ms <
                                            (int64_t)cfg.strategy
                                                .window_predictor_min_secs_left * 1000)
                                            continue;
                                        if (cfg.strategy.window_predictor_skip_late &&
                                            now_ms - cp_ms >
                                                cfg.strategy.window_predictor_late_grace_ms)
                                            continue;
                                        // An old fast-feed direction cannot cast a fresh vote.
                                        if ((cfg.strategy.window_predictor_vote_mode ||
                                             cfg.strategy.window_predictor_require_unanimous) &&
                                            !row.cb_data_ready) {
                                            spdlog::info("wpred skip stale Coinbase: {} {}s f={:.2f}",
                                                         m.asset, m.horizon_seconds, f);
                                            continue;
                                        }
                                        // Choose the side from correlated votes:
                                        // market/model, Chainlink window direction,
                                        // and Coinbase window direction.
                                        auto decision = vp::engine::checkpoint_decision(
                                            row.pred_up,
                                            row.oracle_delta_pct,
                                            row.cb_delta_pct,
                                            cfg.strategy.window_predictor_vote_min_delta_pct,
                                            cfg.strategy.window_predictor_size_max_mult);
                                        if (!cfg.strategy.window_predictor_vote_mode) {
                                            decision.buy_up = row.pred_up >= 0.5;
                                            decision.size_multiplier = 1.0;
                                        }
                                        if (cfg.strategy.window_predictor_require_unanimous &&
                                            !vp::engine::checkpoint_votes_unanimous(decision)) {
                                            spdlog::info(
                                                "wpred skip consensus: {} {}s f={:.2f} votes={}-{}-{}",
                                                m.asset, m.horizon_seconds, f,
                                                decision.model_vote, decision.oracle_vote,
                                                decision.coinbase_vote);
                                            continue;
                                        }
                                        const double min_oracle = std::max(
                                            0.0, cfg.strategy
                                                     .window_predictor_min_abs_oracle_delta_pct);
                                        if (!std::isfinite(row.oracle_delta_pct) ||
                                            std::abs(row.oracle_delta_pct) < min_oracle) {
                                            spdlog::info(
                                                "wpred skip oracle: {} {}s f={:.2f} delta={:+.4f}% min={:.4f}%",
                                                m.asset, m.horizon_seconds, f,
                                                row.oracle_delta_pct, min_oracle);
                                            continue;
                                        }
                                        vp::ShareSide side = decision.buy_up
                                            ? vp::ShareSide::UP : vp::ShareSide::DOWN;
                                        const double ask = (side == vp::ShareSide::UP)
                                            ? row.up_ask : row.down_ask;
                                        if (!std::isfinite(ask) || ask <= 0.0) continue;
                                        // Sanity band: don't record checkpoints against
                                        // degenerate pricing (near-certain outcomes).
                                        if (ask < cfg.strategy.window_predictor_price_min ||
                                            ask > cfg.strategy.window_predictor_price_max) {
                                            spdlog::info("wpred skip band: {} f={:.2f} ask={:.3f}",
                                                         m.asset, f, ask);
                                            continue;
                                        }
                                        const double fair_side = decision.buy_up
                                            ? row.pred_up : (1.0 - row.pred_up);
                                        const double net_edge =
                                            vp::engine::checkpoint_net_edge(
                                                fair_side, ask,
                                                cfg.strategy.taker_fee_rate,
                                                cfg.strategy.fee_exponent);
                                        const double min_net_edge = std::max(
                                            0.0, cfg.strategy
                                                     .window_predictor_min_net_edge);
                                        if (!std::isfinite(net_edge) ||
                                            net_edge < min_net_edge) {
                                            spdlog::info(
                                                "wpred skip edge: {} {}s f={:.2f} fair={:.3f} ask={:.3f} net={:+.3f} min={:.3f}",
                                                m.asset, m.horizon_seconds, f,
                                                fair_side, ask, net_edge,
                                                min_net_edge);
                                            continue;
                                        }
                                        const double win_return_pct =
                                            vp::engine::checkpoint_win_return_pct(
                                                ask, cfg.strategy.taker_fee_rate,
                                                cfg.strategy.fee_exponent);
                                        const double min_win_return = std::max(
                                            0.0, cfg.strategy
                                                     .window_predictor_min_win_return_pct);
                                        if (!std::isfinite(win_return_pct) ||
                                            win_return_pct < min_win_return) {
                                            spdlog::info(
                                                "wpred skip return: {} {}s f={:.2f} ask={:.3f} net_win={:.1f}% min={:.1f}%",
                                                m.asset, m.horizon_seconds, f, ask,
                                                win_return_pct, min_win_return);
                                            continue;
                                        }
                                        // Calibration gate: once this (pair,
                                        // fraction, probability bucket) has
                                        // enough settled samples, require its
                                        // realized win rate to be within margin
                                        // of the mean predicted side
                                        // probability. The unanimous-checkpoint
                                        // cohort ran ~7 points below its entry
                                        // price out-of-sample; this stops
                                        // trading buckets the model no longer
                                        // calibrates for.
                                        const std::string cal_key =
                                            m.asset + ":" +
                                            std::to_string(m.horizon_seconds) +
                                            ":" + fmt::format("{:.2f}", f) + ":" +
                                            fmt::format("{:.2f}",
                                                        std::floor(fair_side * 20.0) / 20.0);
                                        {
                                            const auto cv = wpred_cal.gate(
                                                cal_key,
                                                cfg.strategy.window_predictor_calibration_min_samples,
                                                cfg.strategy.window_predictor_calibration_margin);
                                            if (!cv.allow) {
                                                spdlog::info(
                                                    "wpred skip calibration: {} {}s f={:.2f} bucket n={} realized={:.3f} predicted={:.3f}",
                                                    m.asset, m.horizon_seconds, f,
                                                    cv.n, cv.realized, cv.predicted);
                                                continue;
                                            }
                                        }
                                        // Stale-book guard (same rationale as the rule
                                        // path): pricing off a dead book corrupts fills.
                                        {
                                            const std::string& tok =
                                                (side == vp::ShareSide::UP)
                                                    ? m.yes_token_id : m.no_token_id;
                                            auto bit = state.last_book_update_ms.find(tok);
                                            int64_t book_age =
                                                (bit == state.last_book_update_ms.end())
                                                    ? INT64_MAX : (now_ms - bit->second);
                                            if (book_age > (int64_t)cfg.strategy.max_book_age_ms)
                                                continue;
                                        }
                                        double base_amt = (fi < am.size())
                                            ? am[fi]
                                            : (!am.empty() ? am.back() : 5.0);
                                        double amt = base_amt * decision.size_multiplier;
                                        if (!(amt > 0.0)) continue;
                                        vp::OrderRequest req;
                                        req.condition_id   = m.condition_id;
                                        req.asset          = m.asset;
                                        req.horizon_s      = m.horizon_seconds;
                                        req.token_id       = (side == vp::ShareSide::UP)
                                                            ? m.yes_token_id : m.no_token_id;
                                        req.side           = side;
                                        req.dir            = vp::OrderDir::Buy;
                                        // buy_mode "shares": amt is a share count —
                                        // convert to its USD equivalent for the executor.
                                        req.size_usd =
                                            (cfg.strategy.window_predictor_buy_mode == "shares")
                                                ? amt * ask : amt;
                                        // Edge, return and band were approved at this ask.
                                        // Spending extra ticks can erase the entire 2c edge
                                        // or exceed the checkpoint's configured price cap.
                                        req.limit_price = ask;
                                        req.source          = cfg.strategy.window_predictor_source;
                                        req.window_close_ms = m.window_close_ms;
                                        req.neg_risk        = m.neg_risk;
                                        req.tick_size       = m.tick_size;
                                        double dbi = 0;
                                        {
                                            auto& bk = (side == vp::ShareSide::UP)
                                                       ? state.book_up : state.book_down;
                                            if (auto bit = bk.find(m.condition_id);
                                                bit != bk.end() && bit->second)
                                                dbi = bit->second->imbalance_topN(
                                                    cfg.strategy.window_predictor_depth_topn);
                                        }
                                        double pmf = pm_flow.imbalance_secs(m.condition_id, 10);
                                        // Forward-looking leads for the labeled
                                        // dataset: CB-vs-Binance divergence and the
                                        // reference asset's 5m drift (both now feed
                                        // the ensemble; logged so calibration can
                                        // weight them later).
                                        double cb_lead = 0;
                                        {
                                            double cbp = state.cb_asset_price(m.asset);
                                            if (cbp > 0 && row.asset_price > 0)
                                                cb_lead = 100.0 * (cbp - row.asset_price)
                                                            / row.asset_price;
                                        }
                                        double xmom = 0;
                                        {
                                            const char* other =
                                                (m.asset == "BTC") ? "ETH" : "BTC";
                                            auto oc = candle_store.copy_recent_closes(
                                                other, vp::engine::Timeframe::OneMinute, 6);
                                            if (oc.size() >= 6 && oc[oc.size() - 6] > 0)
                                                xmom = 100.0
                                                    * (oc.back() - oc[oc.size() - 6])
                                                      / oc[oc.size() - 6];
                                        }
                                        WpredTrade wt;
                                        wt.req   = std::move(req);
                                        wt.frac  = f;
                                        // The signal confidence field represents the
                                        // predicted probability of the selected side.
                                        // Vote consensus is separate telemetry; unanimous
                                        // votes do not imply a 100% outcome probability.
                                        wt.conf  = fair_side;
                                        wt.ask   = ask;
                                        wt.cal_key = cal_key;
                                        wt.pos_key = m.condition_id + "|" +
                                            (side == vp::ShareSide::UP ? "UP" : "DOWN");
                                        wt.cal_p   = fair_side;
                                        wt.reason = fmt::format(
                                            "WPRED-2 f={:.3f} pred={:.3f} vote={}-{}-{} "
                                            "cons={:.2f} size={:.2f}x ask={:.3f} edge={:+.3f} winret={:.1f}% od={:+.3f} "
                                            "cbd={:+.3f} dbi={:+.2f} pmf={:+.2f} cb={:+.3f} xm={:+.3f}",
                                            f, row.pred_up,
                                            decision.model_vote, decision.oracle_vote,
                                            decision.coinbase_vote, decision.consensus,
                                            decision.size_multiplier, ask, net_edge,
                                            win_return_pct,
                                            row.oracle_delta_pct, row.cb_delta_pct,
                                            dbi, pmf, cb_lead, xmom);
                                        wt.model_version = "WPRED-2";
                                        wt.payload = fmt::format(
                                            R"({{"fraction":{:.4f},"checkpoint_ms":{},)"
                                            R"("pred":{:.4f},"fair_up":{:.4f},"conf":{:.4f},)"
                                            R"("ask":{:.3f},"mid":{:.3f},"side":"{}",)"
                                            R"("oracle_delta_pct":{:.4f},"dbi":{:.4f},)"
                                            R"("pmf":{:.4f},"tech":{:.4f},"horizon_s":{},)"
                                            R"("cb_delta_pct":{:.4f},"cb_lead":{:.4f},"xmom":{:.4f},)"
                                            R"("model_vote":{},"oracle_vote":{},"coinbase_vote":{},)"
                                            R"("up_votes":{},"down_votes":{},"consensus":{:.4f},)"
                                            R"("base_amount":{:.4f},"size_multiplier":{:.4f},)"
                                            R"("net_edge":{:.4f},"net_win_return_pct":{:.4f},)"
                                            R"("mode":"{}"}})",
                                            f, cp_ms, row.pred_up, row.pred_fair_up,
                                            row.pred_conf, ask,
                                            (side == vp::ShareSide::UP) ? row.up_price
                                                                        : row.down_price,
                                            (side == vp::ShareSide::UP) ? "UP" : "DOWN",
                                            row.oracle_delta_pct, dbi, pmf,
                                            row.pred_tech_score, m.horizon_seconds,
                                            row.cb_delta_pct, cb_lead, xmom,
                                            decision.model_vote, decision.oracle_vote,
                                            decision.coinbase_vote, decision.up_votes,
                                            decision.down_votes, decision.consensus,
                                            base_amt, decision.size_multiplier,
                                            net_edge, win_return_pct,
                                            cfg.strategy.window_predictor_buy_mode);
                                        wpred_trades.push_back(std::move(wt));
                                    }
                                }
                            }

                            // ── Run rule engine FIRST (predictor, oracle-lag, endgame) ──
                            // These are your real alpha strategies. ScalpManager is the
                            // fallback — only fires if nothing else qualifies.
                            vp::rules::EvaluationDiagnostics rule_diag;
                            auto sig = vp::rules::evaluate(row, cfg.strategy, now_ms, &rule_diag);
                            bool from_scalp = false;

                            // Fix #1: block entry if the OPPOSITE side of this market is
                            // already open.  Buying both UP and DOWN on the same condition_id
                            // guarantees a split loss (one side wins, the other loses, and we
                            // pay two entry prices).  This is distinct from the same-side cap
                            // (kMaxSameSide) which only guards against global directional stacking.
                            if (sig) {
                                const vp::ShareSide want  = sig->side;
                                const vp::ShareSide other = (want == vp::ShareSide::UP)
                                                            ? vp::ShareSide::DOWN : vp::ShareSide::UP;
                                for (const auto& p : open_pos) {
                                    if (p.condition_id == m.condition_id &&
                                        p.side          == other &&
                                        p.shares        >  cfg.strategy.dust_position_shares) {
                                        sig = std::nullopt;
                                        rule_diag = {"opposite_position", "opposite side already open"};
                                        break;
                                    }
                                }
                            }

                            // ── ScalpManager entry (fallback only) ──
                            // Only checked when the rule engine passed (no signals from
                            // predictor/oracle-lag/endgame). This prevents micro-scalp noise
                            // from consuming entry slots and blocking real alpha.
                            if (!sig && scalp_active) {
                                bool scalp_asset_blocked = false;
                                if (cfg.strategy.scalp_v3_enabled) {
                                    scalp_asset_blocked = std::find(
                                        cfg.strategy.scalp_v3_blocked_assets.begin(),
                                        cfg.strategy.scalp_v3_blocked_assets.end(),
                                        m.asset) != cfg.strategy.scalp_v3_blocked_assets.end();
                                }
                                if (scalp_asset_blocked) {
                                    continue;
                                }
                                auto& counter = state.entries_per_window[m.condition_id];
                                double up_mid = row.up_price;
                                double down_mid = row.down_price;
                                double up_vel = tick_buffer.velocity_us(m.condition_id, "UP");
                                double down_vel = tick_buffer.velocity_us(m.condition_id, "DOWN");
                                int secs_left = (m.window_close_ms - now_ms) / 1000;
                                if (auto scalp_sig = scalp_manager.check_entry(
                                        m, up_mid, down_mid, up_vel, down_vel,
                                        now_ms * 1000LL, secs_left, counter)) {
                                    // Oracle-agreement gate: book velocity alone is noise
                                    // (settlement replay 2026-06-09: oracle-flat scalps =
                                    // 53% coin flip; oracle-confirmed ones won). The scalp
                                    // side must be backed by a real oracle move.
                                    bool oracle_agrees = true;
                                    if (cfg.strategy.scalp_v3_enabled &&
                                        cfg.strategy.scalp_v3_require_oracle_agree) {
                                        const double d = row.oracle_delta_pct;
                                        const double cb = cfg.strategy.coinbase_lead_mode
                                            ? row.cb_delta_pct : 0.0;
                                        const double thr = cfg.strategy.min_oracle_delta_pct;
                                        // Coinbase counts as confirmation too — it IS the
                                        // oracle's next print (1.5-3s lead), as long as the
                                        // oracle doesn't contradict the direction.
                                        oracle_agrees = (scalp_sig->side == vp::ShareSide::UP)
                                            ? (d >= +thr || (cb >= +thr && d >= 0))
                                            : (d <= -thr || (cb <= -thr && d <= 0));
                                    }
                                    if (oracle_agrees) {
                                        sig = vp::SignalOut{
                                            scalp_sig->side,
                                            vp::OrderDir::Buy,
                                            scalp_sig->confidence,
                                            scalp_sig->rec_limit_price,
                                            scalp_sig->rec_size_usd,
                                            scalp_sig->reason,
                                            scalp_sig->side == vp::ShareSide::UP ? '+' : 'v'};
                                        from_scalp = true;
                                    }
                                }
                            }

                            max_delta_pct = std::max(max_delta_pct, std::abs(row.oracle_delta_pct));
                            // ── Gate pipeline (Phase 1.3): one gate per reason,
                            // stable machine-readable codes, cheapest-first, short-circuit.
                            if (!sig) {
                                const std::string code = std::string("rule_") + rule_diag.code;
                                ++skip_codes[code];
                                if (code.find("price_band") != std::string::npos)
                                    ++diag_skip_price;
                                auto& last_log = rule_veto_last_log[code];
                                if (now_ms - last_log >= 30'000) {
                                    last_log = now_ms;
                                    spdlog::info("rule veto {} {} {} t={}s: {}",
                                        m.asset, m.condition_id.substr(0, 10), code,
                                        (m.window_close_ms-now_ms)/1000, rule_diag.detail);
                                }
                                continue;
                            }

                            // ── Phase 5.6: persistence sighting bookkeeping ──
                            // MUST run on every sweep sighting (before any gate
                            // can short-circuit). Signal gap > 1.5s resets the
                            // first-seen stamp (flicker = disappear/reappear).
                            {
                                const std::string pk = m.condition_id +
                                    (sig->side == vp::ShareSide::UP ? "|U" : "|D");
                                auto& sp = state.signal_persist[pk];
                                if (sp.second == 0 || now_ms - sp.second > 1500)
                                    sp.first = now_ms;
                                sp.second = now_ms;
                            }
                            auto& counter = state.entries_per_window[m.condition_id];
                            // Per-path entry limit: a scalp entry must not consume the
                            // window slots of the rule engine (and vice versa). Before
                            // this fix, enabling scalp v3 capped ALL paths at the scalp
                            // limit, so one noise scalp blocked the real oracle-lag entry.
                            const int entry_limit = from_scalp
                                ? scalp_entry_limit
                                : cfg.strategy.max_entries_per_window;
                            vp::gates::GatePipeline gp;
                            gp.add(0, "entry_limit", [&]() -> vp::gates::GateResult {
                                if (counter < entry_limit) return {};
                                return {.passed = false,
                                        .code = vp::gates::code::kEntryLimit,
                                        .detail = fmt::format("entries={}/{}", counter, entry_limit)};
                            });
                            gp.add(0, "conf_floor", [&]() -> vp::gates::GateResult {
                                if (sig->confidence >= cfg.strategy.autopilot_conf_floor) return {};
                                return {.passed = false,
                                        .code = vp::gates::code::kConfidenceFloor,
                                        .detail = fmt::format("conf={:.2f} < floor={:.2f}",
                                            sig->confidence, cfg.strategy.autopilot_conf_floor)};
                            });
                            gp.add(0, "phase_edge", [&]() -> vp::gates::GateResult {
                                // ── Phase 5.6: phase-scaled edge floor ──
                                // Early windows demand a stronger signal than late
                                // ones (time-decay makes late moves more reliable,
                                // early moves are noise-prone).
                                if (from_scalp) return {};   // scalps use velocity, not edge
                                const int64_t horizon_s =
                                    (m.horizon_seconds > 0) ? m.horizon_seconds : 300;
                                const double frac_left =
                                    (double)(m.window_close_ms - now_ms) / 1000.0 / horizon_s;
                                const double mult =
                                    (frac_left > 0.50) ? cfg.strategy.entry_early_edge_mult
                                  : (frac_left > 0.25) ? cfg.strategy.entry_mid_edge_mult
                                                       : cfg.strategy.entry_late_edge_mult;
                                if (mult <= 0.0) return {};   // disabled
                                // Edge metric mirrors the signal_cap convention.
                                double edge = 0.0;
                                if (sig->reason.rfind("ORACLE", 0) == 0)
                                    edge = std::abs(sig->trigger_delta_pct);
                                else if (sig->reason.rfind("PREDICTOR", 0) == 0)
                                    edge = 100.0 * std::abs(
                                        (sig->side == vp::ShareSide::UP)
                                            ? row.pred_edge_up : row.pred_edge_down);
                                else return {};   // other sources: no edge metric
                                const double need =
                                    vp::rules::oracle_trigger_threshold(cfg.strategy,
                                        sig->reason.rfind("ORACLE+CB-LEAD", 0) == 0) * mult;
                                if (edge >= need) return {};
                                return {.passed = false,
                                        .code = "phase_edge_floor",
                                        .detail = fmt::format(
                                            "edge={:.4f} < thr*{:.1f}={:.4f} ({}% left)",
                                            edge, mult, need, (int)(frac_left * 100))};
                            });
                            gp.add(0, "persist", [&]() -> vp::gates::GateResult {
                                // ── Phase 5.6: anti-flicker ──
                                // Signal must have persisted entry_persist_ms before
                                // firing. Bookkeeping above ran unconditionally, so
                                // a fresh first_seen < now-persist_ms means flicker.
                                if (!cfg.strategy.entry_persist_gate) return {};
                                const std::string pk = m.condition_id +
                                    (sig->side == vp::ShareSide::UP ? "|U" : "|D");
                                auto it = state.signal_persist.find(pk);
                                if (it == state.signal_persist.end()) return {}; // no data → pass
                                const int64_t held = now_ms - it->second.first;
                                if (held >= std::max(0, cfg.strategy.entry_persist_ms)) return {};
                                return {.passed = false,
                                        .code = "signal_flicker",
                                        .detail = fmt::format("held={}ms < {}ms",
                                            held, cfg.strategy.entry_persist_ms)};
                            });
                            gp.add(0, "sweep_dedup", [&]() -> vp::gates::GateResult {
                                // NOTE: insert() is this gate's side effect — it must
                                // run exactly once per market per sweep (same semantics
                                // as the legacy `traded.insert(...).second` conjunct).
                                if (traded_this_sweep.insert(m.condition_id).second) return {};
                                return {.passed = false,
                                        .code = vp::gates::code::kSweepDedup,
                                        .detail = "already fired this sweep"};
                            });
                            gp.add(0, "second_entry", [&]() -> vp::gates::GateResult {
                                // Fix #3: second (or later) entry in the same oracle
                                // window requires a meaningfully stronger signal.
                                // Marginal-delta double-downs amplify losses without
                                // proportionally improving expected value.
                                if (from_scalp || counter <= 0 ||
                                    std::abs(row.oracle_delta_pct) >=
                                        cfg.strategy.min_oracle_delta_pct * 1.5) return {};
                                return {.passed = false,
                                        .code = vp::gates::code::kSecondEntryWeak,
                                        .detail = fmt::format("|delta|={:.4f}% needs {:.4f}%",
                                            std::abs(row.oracle_delta_pct),
                                            cfg.strategy.min_oracle_delta_pct * 1.5)};
                            });

                            const double live_ask = (sig->side == vp::ShareSide::UP)
                                ? row.up_ask
                                : row.down_ask;
                            double signal_cap = cfg.strategy.hard_share_price_ceil;
                            if (sig->reason.rfind("ORACLE", 0) == 0) {
                                signal_cap = std::min(signal_cap, cfg.strategy.oracle_max_token_price);
                            } else if (sig->reason.rfind("PREDICTOR", 0) == 0) {
                                signal_cap = std::min(signal_cap, cfg.strategy.predictor_max_token_price);
                            } else if (sig->reason.rfind("ENDGAME", 0) == 0) {
                                signal_cap = std::min(signal_cap, cfg.strategy.endgame_max_price);
                            } else if (from_scalp) {
                                signal_cap = std::min(signal_cap,
                                    cfg.strategy.scalp_v3_enabled
                                        ? cfg.strategy.scalp_v3_max_price
                                        : cfg.strategy.scalp_v2_max_price);
                            }
                            gp.add(0, "price_band", [&]() -> vp::gates::GateResult {
                                if (live_ask <= 0)
                                    return {.passed = false,
                                            .code = vp::gates::code::kPriceNoBook,
                                            .detail = fmt::format("ask<=0 on {}",
                                                sig->side == vp::ShareSide::UP ? "UP" : "DOWN")};
                                if (live_ask > signal_cap)
                                    return {.passed = false,
                                            .code = vp::gates::code::kPriceAboveCap,
                                            .detail = fmt::format("ask={:.3f} > cap={:.3f}",
                                                live_ask, signal_cap)};
                                return {};
                            });

                            vp::OrderRequest req;
                            req.condition_id   = m.condition_id;
                            req.asset          = m.asset;
                            req.horizon_s      = m.horizon_seconds;
                            req.token_id       = (sig->side == vp::ShareSide::UP)
                                                  ? m.yes_token_id : m.no_token_id;
                            req.side           = sig->side;
                            req.dir            = vp::OrderDir::Buy;
                            double desired_size = sig->rec_size_usd;
                            if (!from_scalp &&
                                cfg.strategy.equity_sizing_enabled &&
                                cfg.strategy.equity_order_pct > 0.0) {
                                double equity_for_sizing =
                                    state.wallet_baseline_usd + state.net_pnl_usd;
                                if (!std::isfinite(equity_for_sizing) ||
                                    equity_for_sizing <= 0.0) {
                                    equity_for_sizing = cfg.wallet.starting_balance_usd;
                                }
                                if (!std::isfinite(equity_for_sizing) ||
                                    equity_for_sizing <= 0.0) {
                                    equity_for_sizing = 100.0;
                                }
                                desired_size =
                                    equity_for_sizing * cfg.strategy.equity_order_pct;
                            }
                            const double size_floor = from_scalp
                                ? std::max(0.0, cfg.risk.min_order_usd)
                                : std::max(0.0, cfg.strategy.min_size_usd);
                            const double size_ceil = from_scalp
                                ? std::max(size_floor, desired_size)
                                : std::max(size_floor, cfg.strategy.max_size_usd);
                            req.size_usd = std::clamp(desired_size,
                                                      size_floor,
                                                      size_ceil);
                            req.size_usd = std::min(req.size_usd,
                                                    cfg.risk.max_order_usd);
                            gp.add(0, "size", [&]() -> vp::gates::GateResult {
                                if (std::isfinite(req.size_usd) && req.size_usd > 0.0) return {};
                                return {.passed = false,
                                        .code = vp::gates::code::kSizeInvalid,
                                        .detail = fmt::format("size={:.2f}", req.size_usd)};
                            });
                            // Cross by a couple ticks, but never beyond the signal's
                            // configured price band. The executor re-reads best_ask
                            // at submit time and rejects if the ask has moved past this.
                            {
                                const double ts = m.tick_size > 0 ? m.tick_size : 0.01;
                                const double cross_ticks =
                                    std::max(0.0, cfg.strategy.entry_cross_ticks);
                                req.limit_price = std::min(signal_cap, live_ask + cross_ticks * ts);
                                // The model approved the observed ask after costs.
                                // Crossing two extra ticks can consume its entire
                                // margin before the order reaches the venue.
                                if (!from_scalp)
                                    req.limit_price = std::min(req.limit_price,sig->rec_limit_price);
                            }
                            req.source         = from_scalp
                                ? (cfg.strategy.scalp_v3_enabled ? "SCALP_V3_ENTRY" : "SCALP_V2_ENTRY")
                                : "RULE";
                            req.window_close_ms= m.window_close_ms;
                            req.neg_risk       = m.neg_risk;
                            req.tick_size      = m.tick_size;

                            // ── L1 gates: cached-state book checks ──
                            gp.add(1, "book_stale", [&]() -> vp::gates::GateResult {
                                // Stale-book guard: never fire on a book we haven't
                                // seen update recently. Pricing off a dead book is the
                                // root cause of "no orders found to match" /
                                // "Invalid order payload" (the price the bot sees no
                                // longer exists on the live market).
                                auto bit = state.last_book_update_ms.find(req.token_id);
                                int64_t book_age = (bit == state.last_book_update_ms.end())
                                    ? INT64_MAX : (now_ms - bit->second);
                                int max_book_age_ms = cfg.strategy.max_book_age_ms;
                                if (from_scalp && cfg.strategy.scalp_v3_enabled &&
                                    cfg.strategy.scalp_v3_max_book_age_ms > 0) {
                                    max_book_age_ms = std::min(max_book_age_ms,
                                        cfg.strategy.scalp_v3_max_book_age_ms);
                                }
                                if (book_age <= max_book_age_ms) return {};
                                return {.passed = false,
                                        .code = vp::gates::code::kBookStale,
                                        .detail = fmt::format("age={}ms > {}ms",
                                            book_age, max_book_age_ms)};
                            });
                            gp.add(1, "spread", [&]() -> vp::gates::GateResult {
                                // Spread of the side we're buying (ask we pay vs bid we'd
                                // be marked at). A wide spread guarantees an instant
                                // bid-marked "loss" == the spread, which trips the
                                // stop-loss on entry.
                                double spr_pct = 999.0;
                                {
                                    auto& bk = (sig->side == vp::ShareSide::UP)
                                               ? state.book_up : state.book_down;
                                    if (auto it = bk.find(m.condition_id);
                                        it != bk.end() && it->second) {
                                        double b = it->second->best_bid().price;
                                        double a = it->second->best_ask().price;
                                        if (a > 0 && b > 0) spr_pct = 100.0 * (a - b) / a;
                                    }
                                }
                                double max_spread_pct = cfg.strategy.max_entry_spread_pct;
                                if (from_scalp && cfg.strategy.scalp_v3_enabled &&
                                    cfg.strategy.scalp_v3_max_spread_pct > 0) {
                                    max_spread_pct = (max_spread_pct > 0)
                                        ? std::min(max_spread_pct, cfg.strategy.scalp_v3_max_spread_pct)
                                        : cfg.strategy.scalp_v3_max_spread_pct;
                                }
                                if (max_spread_pct > 0 && spr_pct > max_spread_pct)
                                    return {.passed = false,
                                            .code = vp::gates::code::kSpreadWide,
                                            .detail = fmt::format("spread={:.1f}% > {:.1f}%",
                                                spr_pct, max_spread_pct)};
                                return {};
                            });

                            auto gr = gp.run();
                            if (!gr.passed) {
                                const char* c = gr.code ? gr.code : "unknown";
                                ++skip_codes[c];
                                // Legacy counter continuity for the heartbeat format.
                                if (std::string_view(c) == std::string_view(vp::gates::code::kBookStale))
                                    ++diag_skip_stale;
                                else if (std::string_view(c) == std::string_view(vp::gates::code::kSpreadWide))
                                    ++diag_skip_spread;
                                else
                                    ++diag_skip_price;
                                spdlog::debug("autopilot gate {} tier={} code={} token={}… {}",
                                              gr.gate, gr.tier, c,
                                              req.token_id.substr(0, 12), gr.detail);
                                continue;
                            }
                            vp::SignalOut sized_sig = *sig;
                            sized_sig.rec_size_usd = req.size_usd;
                            // Rank scarce risk slots by fee-adjusted expected
                            // value instead of market discovery order.
                            const double rank_score =
                                vp::engine::checkpoint_net_edge(
                                    sized_sig.confidence, live_ask,
                                    cfg.strategy.taker_fee_rate,
                                    cfg.strategy.fee_exponent);
                            deferred_trades.push_back(
                                {req, sized_sig, from_scalp, rank_score});
                        }
                    } // mu released here
                    // ── WPRED checkpoint submissions ────────────────────────
                    // Deliberately bypasses the rule-engine caps below: checkpoints
                    // are deterministic dataset trades and must never be dropped by
                    // same-side caps, stop cooldowns, or per-sweep limits.
                    for (const auto& wt : wpred_trades) {
                        vp::SignalOut wsig;
                        wsig.side = wt.req.side;
                        wsig.dir  = vp::OrderDir::Buy;
                        wsig.confidence     = wt.conf;
                        wsig.rec_limit_price = wt.req.limit_price;
                        wsig.rec_size_usd   = wt.req.size_usd;
                        wsig.reason         = wt.reason;
                        wsig.glyph = (wt.req.side == vp::ShareSide::UP) ? '+' : 'v';
                        std::string rec = (wt.req.side == vp::ShareSide::UP)
                            ? "BUY_UP" : "BUY_DOWN";
                        std::string cid_copy = wt.req.condition_id;
                        vp::SignalOut sig_copy = wsig;
                        std::string mv_copy = wt.model_version;
                        std::string pl_copy = wt.payload;
                        auto res = do_submit(wt.req);
                        persist_q.push([pg = pg.get(), cid = std::move(cid_copy),
                                        sig = std::move(sig_copy), rec,
                                        coid = res.client_order_id,
                                        mv = std::move(mv_copy),
                                        pl = std::move(pl_copy)]() noexcept {
                            pg->insert_signal(cid, sig.side, sig.dir, sig.confidence,
                                              rec, sig.rec_size_usd,
                                              sig.rec_limit_price, sig.reason, mv, pl, coid);
                        });
                        if (res.accepted && res.fill_shares > 0)
                            wpred_cal.open(wt.cal_key, wt.pos_key, wt.cal_p);
                        if (res.accepted)
                            spdlog::info("wpred fired: {} f={:.2f} {} ${:.2f} @ {:.3f} "
                                         "fill={:.3f} [{}]",
                                         wt.req.asset, wt.frac, vp::to_string(wt.req.side),
                                         wt.req.size_usd, wt.ask, res.fill_price,
                                         wt.reason);
                        else
                            spdlog::warn("wpred rejected: {} f={:.2f} {} ask={:.3f} {}",
                                         wt.req.asset, wt.frac,
                                         vp::to_string(wt.req.side), wt.ask,
                                         res.reject_reason);
                    }
                    int64_t lk_held_ms = (vp::Clock::wall_ns() / 1'000'000LL) - lk_t0;
                    if (lk_held_ms > diag_max_lock_ms) diag_max_lock_ms = lk_held_ms;
                    spdlog::debug("autopilot sweep: max_delta={:+.4f}% threshold={:.4f}%",
                                  max_delta_pct, cfg.strategy.min_oracle_delta_pct);
                    ++diag_sweeps_total;
                    if (!deferred_trades.empty()) ++diag_sweeps_with_signal;
                    // Heartbeat every 30s so empty sweeps are visible in the log.
                    if (now_ms - last_autopilot_diag_ms >
                            std::max(1, cfg.risk.autopilot_heartbeat_secs) * 1000LL) {
                        last_autopilot_diag_ms = now_ms;
                        // ── Phase 1.3: per-code breakdown (machine-readable) ──
                        std::string gate_str;
                        for (const auto& [c, n] : skip_codes)
                            gate_str += fmt::format("{}={} ", c, n);
                        spdlog::info("autopilot heartbeat: sweeps={} fired={} max_delta={:+.4f}% "
                                     "(thr {:.4f}%) markets={} open_pos={} max_lock_hold={}ms "
                                     "skip[price={} stale={} spread={}] rows={} skip_open={} skip_ref={} skip_twap={} gates=[{}]",
                                     diag_sweeps_total, diag_sweeps_with_signal,
                                     max_delta_pct, cfg.strategy.min_oracle_delta_pct,
                                     (int)state.markets.size(), (int)open_pos.size(),
                                     diag_max_lock_ms,
                                     diag_skip_price, diag_skip_stale, diag_skip_spread,
                                     sweep_rows, sweep_skip_open, sweep_skip_ref,
                                     sweep_skip_twap, gate_str);
                        diag_sweeps_total = 0;
                        diag_sweeps_with_signal = 0;
                        diag_max_lock_ms = 0;
                        diag_skip_price  = 0;
                        diag_skip_stale  = 0;
                        diag_skip_spread = 0;
                        skip_codes.clear();
                    }
                    // Submit ALL qualifying trades (not just the first one found)
                    // but cap the burst to avoid over-concentrating in one sweep.
                    int max_per_sweep = std::max(1, cfg.strategy.autopilot_max_per_sweep);
                    int submitted = 0;
                    std::stable_sort(
                        deferred_trades.begin(), deferred_trades.end(),
                        [](const DeferredTrade& a, const DeferredTrade& b) {
                            return a.rank_score > b.rank_score;
                        });
                    // ── Correlation cap + trend guard ───────────────────────────
                    // Added after the 2026-06-06 −21% loss: the bot stacked FIVE
                    // same-side (UP) buys across BTC/ETH/SOL/XRP into a DOWN-trending
                    // market (crypto is correlated → one move hit the whole book).
                    //  (A) cap concurrent positions per side; (B) refuse to enter a
                    //      side that fights the recent oracle trend.
                    const int kMaxSameSide = std::max(1, cfg.strategy.max_same_side_positions);
                    // Dollar form of the same cap: small clips must not stack
                    // past the exposure the count cap was sized for (five $6
                    // positions pass a 2-position count but triple the risk).
                    const double kMaxSameSideUsd =
                        double(kMaxSameSide) * std::max(0.0, cfg.strategy.max_size_usd);
                    const double kTrendBlockPct = std::max(0.0, cfg.strategy.trend_block_pct);
                    const double kTrendLookbackSec = std::max(1.0, cfg.strategy.trend_lookback_secs);
                    int open_up = 0, open_down = 0;
                    double notional_up = 0.0, notional_down = 0.0;
                    for (const auto& p : positions.snapshot()) {
                        if (p.shares <= cfg.strategy.dust_position_shares) continue;
                        (p.side == vp::ShareSide::UP ? open_up : open_down)++;
                        (p.side == vp::ShareSide::UP ? notional_up : notional_down) +=
                            p.shares * p.avg_price;
                    }
                    for (auto& dt : deferred_trades) {
                        auto& dt_req = dt.req;
                        auto& dt_sig = dt.sig;
                        if (submitted >= max_per_sweep) break;
                        // (A) Same-side correlation cap (counts existing + this batch).
                        int& side_ct = (dt_req.side == vp::ShareSide::UP) ? open_up : open_down;
                        if (side_ct >= kMaxSameSide) {
                            spdlog::info("autopilot skip: same-side cap — already {} {} positions ({} {})",
                                         side_ct, vp::to_string(dt_req.side), dt_req.asset, vp::to_string(dt_req.side));
                            continue;
                        }
                        // (A3) Dollar form of the same cap.
                        double& side_nt = (dt_req.side == vp::ShareSide::UP) ? notional_up : notional_down;
                        if (kMaxSameSideUsd > 0 &&
                            side_nt + dt_req.size_usd > kMaxSameSideUsd + 1e-6) {
                            spdlog::info("autopilot skip: same-side $ cap — ${:.2f} deployed + ${:.2f} > ${:.2f} ({} {})",
                                         side_nt, dt_req.size_usd, kMaxSameSideUsd,
                                         dt_req.asset, vp::to_string(dt_req.side));
                            continue;
                        }
                        // (A2) Stop-loss cooldown: block re-entry on a market+side that just stopped.
                        {
                            auto ck = dt_req.condition_id + "|" + vp::to_string(dt_req.side);
                            auto it = stop_cooldown_until.find(ck);
                            if (it != stop_cooldown_until.end() && now_ms < it->second) {
                                spdlog::debug("autopilot skip: stop-loss cooldown active — {} {} ({}s left)",
                                              dt_req.asset, vp::to_string(dt_req.side),
                                              (it->second - now_ms) / 1000);
                                continue;
                            }
                        }
                        // (B) Trend guard: don't buy a side counter to the 3-min oracle trend.
                        {
                            double now_s  = now_ms / 1000.0;
                            double p_now  = oracle_buf.latest(dt_req.asset);
                            double p_then = oracle_buf.lookup_at_or_before(dt_req.asset, now_s - kTrendLookbackSec);
                            if (p_now > 0 && p_then > 0) {
                                double trend_pct = (p_now - p_then) / p_then * 100.0;
                                bool counter = (dt_req.side == vp::ShareSide::UP   && trend_pct < -kTrendBlockPct) ||
                                               (dt_req.side == vp::ShareSide::DOWN && trend_pct >  kTrendBlockPct);
                                if (counter) {
                                    spdlog::info("autopilot skip: counter-trend — {} entry vs {:+.2f}% 3m oracle ({})",
                                                 vp::to_string(dt_req.side), trend_pct, dt_req.asset);
                                    continue;
                                }
                            }
                        }
                        // Persist signal to DB (best-effort)
                        std::string rec = (dt_sig.glyph == '+') ? "BUY_UP"
                                        : (dt_sig.glyph == 'v') ? "BUY_DOWN"
                                        : "BUY";
                        std::string cid_copy = dt_req.condition_id;
                        vp::SignalOut sig_copy = dt_sig;
                        ++submitted;
                        auto res = do_submit(dt_req);
                        persist_q.push([pg=pg.get(), cid=std::move(cid_copy),
                                        sig=std::move(sig_copy), rec,
                                        coid=res.client_order_id]() noexcept {
                            pg->insert_signal(cid, sig.side, sig.dir,
                                              sig.confidence, rec,
                                              sig.rec_size_usd, sig.rec_limit_price,
                                              sig.reason, "", "", coid);
                        });
                        if (res.accepted) {
                            ++side_ct;   // accepted entry counts toward this batch's side cap
                            {
                                std::lock_guard<std::mutex> lk(state.mu);
                                auto& reset_ts = state.entries_per_window_reset[dt_req.condition_id];
                                auto& counter = state.entries_per_window[dt_req.condition_id];
                                if (dt_req.window_close_ms != reset_ts) {
                                    counter = 0;
                                    reset_ts = dt_req.window_close_ms;
                                }
                                ++counter;
                            }
                            if (dt.from_scalp) {
                                const double entry_price = res.fill_price > 0
                                    ? res.fill_price
                                    : dt_req.limit_price;
                                scalp_manager.on_entry(dt_req.condition_id, dt_req.side,
                                                       entry_price, now_ms * 1000LL);
                            }
                        }
                    }
                }   // throttle gate
                }   // startup delay else
            }   // autopilot enabled guard

            // ----------------------------------------------------------------
            // Oracle-Direction Reversal Exit
            // Exit a winning position when the oracle flips direction against us.
            // Uses oracle delta (reliable, not tick velocity which fires on spread).
            // Only exits when bid > avg_price — stop_loss handles the loss case.
            // ----------------------------------------------------------------
            if (cfg.strategy.oracle_reversal_exit && state.autopilot.load()) {
                auto pos_snap = positions.snapshot();
                for (const auto& p : pos_snap) {
                    if (p.shares < cfg.strategy.min_position_shares || p.avg_price <= 0) continue;
                    // Wait minimum hold time before checking reversal
                    int64_t hold_ms = now_ms - p.opened_ms;
                    if (hold_ms < (int64_t)cfg.strategy.oracle_reversal_min_hold_secs * 1000)
                        continue;
                    // Skip positions managed by ScalpManager
                    if ((cfg.strategy.scalp_v2_enabled || cfg.strategy.scalp_v3_enabled) &&
                        scalp_manager.is_tracking(p.condition_id, p.side))
                        continue;
                    // Compute current oracle delta for this market
                    double oracle_delta = 0;
                    std::string rev_asset;
                    {
                        std::lock_guard<std::mutex> lk(state.mu);
                        auto mit = std::find_if(state.markets.begin(), state.markets.end(),
                            [&](const vp::PmMarket& m) { return m.condition_id == p.condition_id; });
                        if (mit != state.markets.end()) {
                            rev_asset = mit->asset;
                            double window_open_ts_s = (double)mit->window_open_ms / 1000.0;
                            double oracle_now     = oracle_buf.latest(mit->reference_key());
                            double oracle_at_open = oracle_buf.lookup_at_or_before(mit->reference_key(), window_open_ts_s);
                            if (oracle_at_open <= 0)
                                oracle_at_open = 0; // never manufacture an opening reference
                            if (oracle_at_open > 0 && oracle_now > 0)
                                oracle_delta = 100.0 * (oracle_now - oracle_at_open) / oracle_at_open;
                        }
                    }
                    if (rev_asset.empty()) continue;
                    // Check if oracle has reversed relative to our position direction
                    double rev_thresh = cfg.strategy.min_oracle_delta_pct;
                    bool reversed = (p.side == vp::ShareSide::UP   && oracle_delta <= -rev_thresh) ||
                                    (p.side == vp::ShareSide::DOWN  && oracle_delta >=  rev_thresh);
                    if (!reversed) continue;
                    // Only take profit — stop_loss_mode handles the losing side
                    double bid = 0;
                    {
                        std::lock_guard<std::mutex> lk(state.mu);
                        auto& books = (p.side == vp::ShareSide::UP) ? state.book_up : state.book_down;
                        if (auto it = books.find(p.condition_id); it != books.end() && it->second)
                            bid = it->second->best_bid().price;
                    }
                    if (bid <= 0 || bid <= p.avg_price) continue;
                    spdlog::info("ORACLE-REVERSAL {} {} {:.0f}sh avg={:.3f} bid={:.3f} "
                                 "oracle_delta={:+.3f}% (thresh={:.3f}%)",
                                 rev_asset, vp::to_string(p.side), p.shares,
                                 p.avg_price, bid, oracle_delta, rev_thresh);
                    vp::OrderRequest req;
                    req.condition_id    = p.condition_id;
                    req.asset           = p.asset;
                    req.horizon_s       = p.horizon_s;
                    req.token_id        = p.token_id;
                    req.side            = p.side;
                    req.dir             = vp::OrderDir::Sell;
                    req.size_shares     = 0;
                    req.source          = "ORACLE_REVERSAL";
                    req.window_close_ms = p.window_close_ms;
                    {
                        std::lock_guard<std::mutex> lk(state.mu);
                        auto mit = std::find_if(state.markets.begin(), state.markets.end(),
                            [&](const vp::PmMarket& m) { return m.condition_id == p.condition_id; });
                        if (mit != state.markets.end()) {
                            req.neg_risk  = mit->neg_risk;
                            req.tick_size = mit->tick_size;
                        }
                    }
                    do_submit(req);
                }
            }

            // ----------------------------------------------------------------
            // Oracle Panic Exit (loss-side complement to ORACLE_REVERSAL)
            // When the oracle has moved DECISIVELY against an open position, bail
            // at market NOW — before the losing side's book gaps through the
            // stop_loss threshold (the -64%/-84% gap-through losses). The oracle
            // reprices ahead of the orderbook, so reacting to it catches a bid
            // before it evaporates. Fires on the LOSS side (bid <= avg); the
            // profit side stays with ORACLE_REVERSAL. The threshold is larger than
            // the entry delta so only a real adverse move triggers it, not noise.
            // ----------------------------------------------------------------
            if (cfg.strategy.oracle_panic_exit && state.autopilot.load()) {
                auto pos_snap = positions.snapshot();
                for (const auto& p : pos_snap) {
                    if (p.shares < cfg.strategy.min_position_shares || p.avg_price <= 0) continue;
                    if (now_ms - p.opened_ms <
                        (int64_t)cfg.strategy.oracle_panic_min_hold_secs * 1000LL) continue;
                    if ((cfg.strategy.scalp_v2_enabled || cfg.strategy.scalp_v3_enabled) &&
                        scalp_manager.is_tracking(p.condition_id, p.side)) continue;
                    // Decisive adverse oracle move for this market?
                    double oracle_delta = 0;
                    std::string pa_asset;
                    {
                        std::lock_guard<std::mutex> lk(state.mu);
                        auto mit = std::find_if(state.markets.begin(), state.markets.end(),
                            [&](const vp::PmMarket& m) { return m.condition_id == p.condition_id; });
                        if (mit != state.markets.end()) {
                            pa_asset = mit->asset;
                            double window_open_ts_s = (double)mit->window_open_ms / 1000.0;
                            double oracle_now     = oracle_buf.latest(mit->reference_key());
                            double oracle_at_open = oracle_buf.lookup_at_or_before(mit->reference_key(), window_open_ts_s);
                            if (oracle_at_open <= 0)
                                oracle_at_open = 0; // never manufacture an opening reference
                            if (oracle_at_open > 0 && oracle_now > 0)
                                oracle_delta = 100.0 * (oracle_now - oracle_at_open) / oracle_at_open;
                        }
                    }
                    if (pa_asset.empty()) continue;
                    double pa_thresh = cfg.strategy.oracle_panic_delta_pct;
                    bool adverse = (p.side == vp::ShareSide::UP   && oracle_delta <= -pa_thresh) ||
                                   (p.side == vp::ShareSide::DOWN  && oracle_delta >=  pa_thresh);
                    if (!adverse) continue;
                    // Panic-exit only the LOSS side; bid > avg belongs to ORACLE_REVERSAL.
                    double bid = 0;
                    {
                        std::lock_guard<std::mutex> lk(state.mu);
                        auto& books = (p.side == vp::ShareSide::UP) ? state.book_up : state.book_down;
                        if (auto it = books.find(p.condition_id); it != books.end() && it->second)
                            bid = it->second->best_bid().price;
                    }
                    // No bid = nothing to sell into (closed/empty book — typical in
                    // the gap between window-close and settlement). Firing here just
                    // produces an endless "no bids on book" reject spin; let the
                    // settlement loop resolve the position instead.
                    if (bid <= 0) continue;
                    if (bid > p.avg_price) continue;   // in profit → ORACLE_REVERSAL owns it
                    double loss_pct = (bid > 0) ? 100.0 * (bid - p.avg_price) / p.avg_price : -100.0;
                    spdlog::info("ORACLE-PANIC {} {} {:.0f}sh avg={:.3f} bid={:.3f} "
                                 "oracle_delta={:+.3f}% (thresh={:.3f}%) pl={:+.1f}%",
                                 pa_asset, vp::to_string(p.side), p.shares,
                                 p.avg_price, bid, oracle_delta, pa_thresh, loss_pct);
                    vp::OrderRequest req;
                    req.condition_id    = p.condition_id;
                    req.asset           = p.asset;
                    req.horizon_s       = p.horizon_s;
                    req.token_id        = p.token_id;
                    req.side            = p.side;
                    req.dir             = vp::OrderDir::Sell;
                    req.size_shares     = 0;  // sell all
                    req.source          = "ORACLE_PANIC";
                    req.window_close_ms = p.window_close_ms;
                    {
                        std::lock_guard<std::mutex> lk(state.mu);
                        auto mit = std::find_if(state.markets.begin(), state.markets.end(),
                            [&](const vp::PmMarket& m) { return m.condition_id == p.condition_id; });
                        if (mit != state.markets.end()) {
                            req.neg_risk  = mit->neg_risk;
                            req.tick_size = mit->tick_size;
                        }
                    }
                    do_submit(req);
                }
            }

            // ----------------------------------------------------------------
            // Momentum-Flip Exit + Reverse (P2)
            // The instant the ensemble predictor turns against an open position
            // — in profit OR loss — fire-sale it immediately and (optionally) buy
            // the opposite side if enough time remains. This is the "watch closely,
            // momentum changed, bail now and flip" behaviour. The predictor folds
            // in tick momentum, trade flow, oracle velocity and BS fair value, so a
            // confident cross past 0.5±margin is a real regime change, not noise.
            // stop_loss_pct is a separate configured backstop.
            // ----------------------------------------------------------------
            if (cfg.strategy.momentum_flip_exit && state.autopilot.load()) {
                const double fm = cfg.strategy.momentum_flip_margin;
                const double fc = cfg.strategy.momentum_flip_min_conf;
                auto pos_snap = positions.snapshot();
                for (const auto& p : pos_snap) {
                    if (p.shares < cfg.strategy.min_position_shares || p.avg_price <= 0) continue;
                    // Small hold guard: avoid flip-flopping on entry-time wobble.
                    if (now_ms - p.opened_ms <
                            std::max(0, cfg.strategy.momentum_flip_min_hold_ms)) continue;
                    // Skip positions managed by ScalpManager (it has its own exits).
                    if ((cfg.strategy.scalp_v2_enabled || cfg.strategy.scalp_v3_enabled) &&
                        scalp_manager.is_tracking(p.condition_id, p.side))
                        continue;
                    // Latest predictor read for this market.
                    double pred_up = 0.5, pred_conf = 0.0;
                    {
                        std::lock_guard<std::mutex> lk(state.mu);
                        auto it = state.latest_pred.find(p.condition_id);
                        if (it == state.latest_pred.end()) continue;
                        pred_up   = it->second.first;
                        pred_conf = it->second.second;
                    }
                    if (pred_conf < fc) continue;
                    // Has the model flipped to favour the OTHER side?
                    bool flipped = (p.side == vp::ShareSide::UP   && pred_up <= 0.5 - fm) ||
                                   (p.side == vp::ShareSide::DOWN  && pred_up >= 0.5 + fm);
                    if (!flipped) continue;

                    double bid = 0;
                    {
                        std::lock_guard<std::mutex> lk(state.mu);
                        auto& books = (p.side == vp::ShareSide::UP) ? state.book_up : state.book_down;
                        if (auto it = books.find(p.condition_id); it != books.end() && it->second)
                            bid = it->second->best_bid().price;
                    }
                    double pl_pct = (bid > 0) ? 100.0 * (bid - p.avg_price) / p.avg_price : 0.0;
                    int64_t secs_left_mf = (p.window_close_ms - now_ms) / 1000;
                    spdlog::info("MOMENTUM-FLIP {} {} {:.0f}sh avg={:.3f} bid={:.3f} "
                                 "pl={:+.1f}% pred_up={:.3f} conf={:.2f} t={}s",
                                 p.asset, vp::to_string(p.side), p.shares,
                                 p.avg_price, bid, pl_pct, pred_up, pred_conf, secs_left_mf);

                    // 1) Fire-sale the position now (profit or loss).
                    vp::OrderRequest req;
                    req.condition_id    = p.condition_id;
                    req.asset           = p.asset;
                    req.horizon_s       = p.horizon_s;
                    req.token_id        = p.token_id;
                    req.side            = p.side;
                    req.dir             = vp::OrderDir::Sell;
                    req.size_shares     = 0;  // sell all
                    req.source          = "MOMENTUM_FLIP";
                    req.window_close_ms = p.window_close_ms;
                    {
                        std::lock_guard<std::mutex> lk(state.mu);
                        auto mit = std::find_if(state.markets.begin(), state.markets.end(),
                            [&](const vp::PmMarket& m) { return m.condition_id == p.condition_id; });
                        if (mit != state.markets.end()) {
                            req.neg_risk  = mit->neg_risk;
                            req.tick_size = mit->tick_size;
                        }
                    }
                    do_submit(req);

                    // 2) Reverse into the opposite side if enough time remains.
                    if (cfg.strategy.momentum_flip_reverse &&
                        secs_left_mf >= cfg.strategy.momentum_flip_min_secs) {
                        vp::ShareSide opp = (p.side == vp::ShareSide::UP)
                                             ? vp::ShareSide::DOWN : vp::ShareSide::UP;
                        double opp_ask = 0; std::string opp_token;
                        bool opp_neg_risk = true; double opp_tick = 0.01;
                        {
                            std::lock_guard<std::mutex> lk(state.mu);
                            auto& opp_books = (opp == vp::ShareSide::UP) ? state.book_up : state.book_down;
                            if (auto it = opp_books.find(p.condition_id);
                                it != opp_books.end() && it->second)
                                opp_ask = it->second->best_ask().price;
                            auto mit = std::find_if(state.markets.begin(), state.markets.end(),
                                [&](const vp::PmMarket& m) { return m.condition_id == p.condition_id; });
                            if (mit != state.markets.end()) {
                                opp_token    = (opp == vp::ShareSide::UP)
                                               ? mit->yes_token_id : mit->no_token_id;
                                opp_neg_risk = mit->neg_risk;
                                opp_tick     = mit->tick_size;
                            }
                        }
                        // Only reverse into the same mid-window band we'd enter fresh,
                        // off a FRESH book, and only if the anti-churn guard allows it
                        // (cooldown + per-window cap). reverse_allowed() has a side effect
                        // so it must stay last in the && chain (only records on a real fire).
                        if (opp_ask >= cfg.strategy.min_entry_share_price &&
                            opp_ask <= cfg.strategy.hard_share_price_ceil &&
                            !opp_token.empty() &&
                            book_fresh(opp_token, now_ms) &&
                            reverse_allowed(p.condition_id, p.window_close_ms, now_ms)) {
                            vp::OrderRequest rev;
                            rev.condition_id    = p.condition_id;
                            rev.asset           = p.asset;
                            rev.horizon_s       = p.horizon_s;
                            rev.token_id        = opp_token;
                            rev.side            = opp;
                            rev.dir             = vp::OrderDir::Buy;
                            rev.size_usd        = cfg.strategy.per_click_size_usd;
                            rev.limit_price     = std::min(cfg.strategy.hard_share_price_ceil,
                                                           opp_ask * (1.0 + cfg.strategy.reverse_slippage_pct) +
                                                           cfg.strategy.reverse_slippage_extra);
                            rev.source          = "MOMENTUM_FLIP_REVERSE";
                            rev.window_close_ms = p.window_close_ms;
                            rev.neg_risk        = opp_neg_risk;
                            rev.tick_size       = opp_tick;
                            rev.fee_rate        = cfg.strategy.taker_fee_rate;
                            rev.fee_exponent    = cfg.strategy.fee_exponent;
                            spdlog::info("MOMENTUM-FLIP-REVERSE {} {} opp_ask={:.3f} t={}s",
                                         p.asset, vp::to_string(opp), opp_ask, secs_left_mf);
                            do_submit(rev);
                        }
                    }
                }
            }

            // ----------------------------------------------------------------
            // Conviction score for an open position: how many independent
            // signals still favor our side? (0-3)
            //   1. window-anchored ORACLE delta   (the settlement truth source)
            //   2. window-anchored COINBASE delta (the oracle's next print)
            //   3. cached predictor ensemble      (5y candles + microstructure)
            // Forensics 2026-06-10: 67% of blind price stops would have WON at
            // settlement. The exit sweeps below use this score, not price alone.
            // ----------------------------------------------------------------
            auto conviction_votes = [&](const std::string& cond_id,
                                        vp::ShareSide side,
                                        const std::string& asset) -> int {
                double wopen_s = 0;
                {
                    std::lock_guard<std::mutex> lk(state.mu);
                    auto mit = std::find_if(state.markets.begin(), state.markets.end(),
                        [&](const vp::PmMarket& m){ return m.condition_id == cond_id; });
                    if (mit != state.markets.end() && mit->window_open_ms > 0)
                        wopen_s = (double)mit->window_open_ms / 1000.0;
                }
                const double dir = (side == vp::ShareSide::UP) ? +1.0 : -1.0;
                const double thr = cfg.strategy.conviction_min_delta_pct;
                int votes = 0;
                auto delta_vote = [&](vp::core::OracleBuffer& buf) {
                    double now_px  = buf.latest(asset);
                    double open_px = wopen_s > 0 ? buf.lookup_at_or_before(asset, wopen_s) : 0.0;
                    if (open_px <= 0) open_px = buf.earliest(asset);
                    if (open_px > 0 && now_px > 0 &&
                        100.0 * (now_px - open_px) / open_px * dir >= thr)
                        ++votes;
                };
                delta_vote(oracle_buf);
                delta_vote(cb_buf);
                {
                    std::lock_guard<std::mutex> lk(state.mu);
                    auto it = state.pred_cache.find(cond_id);
                    if (it != state.pred_cache.end() &&
                        now_ms - it->second.ts_ms < 30000 &&
                        it->second.out.confidence >= 0.50) {
                        double pu = it->second.out.prediction;
                        if ((dir > 0 && pu >= 0.55) || (dir < 0 && pu <= 0.45))
                            ++votes;
                    }
                }
                return votes;
            };
            // Throttle for SMART-HOLD / RIDE-TO-SETTLE log lines (the sweeps
            // run every ~200ms; one line per position per 15s is plenty).
            static std::unordered_map<std::string, int64_t> smart_log_ms;

            // ----------------------------------------------------------------
            // ScalpManager exit evaluation (momentum reversal, trailing stop,
            // tight profit target). Runs for all scalp-managed positions at
            // full TUI refresh rate (every 200ms). Exits are submitted immediately.
            // ----------------------------------------------------------------
            if ((cfg.strategy.scalp_v2_enabled || cfg.strategy.scalp_v3_enabled) &&
                state.autopilot.load()) {
                auto pos_snap = positions.snapshot();
                auto get_bid = [&state](const std::string& cid, vp::ShareSide side) -> double {
                    std::lock_guard<std::mutex> lk(state.mu);
                    auto& books = (side == vp::ShareSide::UP) ? state.book_up : state.book_down;
                    auto it = books.find(cid);
                    return (it != books.end() && it->second) ? it->second->best_bid().price : 0.0;
                };
                auto exits = scalp_manager.check_exits(pos_snap, get_bid, now_ms * 1000LL);
                for (const auto& exit : exits) {
                    // SMART EXIT: a scalp trailing stop on a position the signals
                    // still favor is the same noise-sell as a blind stop-loss.
                    if (cfg.strategy.smart_exit_mode && exit.reason == "TRAILING_STOP") {
                        std::string sc_asset;
                        {
                            std::lock_guard<std::mutex> lk(state.mu);
                            for (const auto& m : state.markets)
                                if (m.condition_id == exit.condition_id) { sc_asset = m.asset; break; }
                        }
                        if (!sc_asset.empty()) {
                            int votes = conviction_votes(exit.condition_id, exit.side, sc_asset);
                            if (votes > cfg.strategy.smart_stop_max_votes) {
                                auto& last = smart_log_ms["sc|" + exit.condition_id +
                                                          vp::to_string(exit.side)];
                                if (now_ms - last > 15000) {
                                    last = now_ms;
                                    spdlog::info("SMART-HOLD scalp {} {} votes={} — trailing stop suppressed",
                                                 sc_asset, vp::to_string(exit.side), votes);
                                }
                                continue;
                            }
                        }
                    }
                    spdlog::info("scalp exit: {} {} reason={}", exit.condition_id,
                                 exit.side == vp::ShareSide::UP ? "UP" : "DN", exit.reason);
                    vp::OrderRequest req;
                    req.condition_id    = exit.condition_id;
                    req.side            = exit.side;
                    req.dir             = vp::OrderDir::Sell;
                    req.size_shares     = 0;  // sell all
                    req.source          = "SCALP_EXIT_" + exit.reason;
                    // Look up market metadata from shared state
                    {
                        std::lock_guard<std::mutex> lk(state.mu);
                        for (const auto& m : state.markets) {
                            if (m.condition_id == exit.condition_id) {
                                req.asset           = m.asset;
                                req.horizon_s       = m.horizon_seconds;
                                req.token_id        = (exit.side == vp::ShareSide::UP)
                                                       ? m.yes_token_id : m.no_token_id;
                                req.window_close_ms = m.window_close_ms;
                                req.neg_risk        = m.neg_risk;
                                req.tick_size       = m.tick_size;
                                break;
                            }
                        }
                    }
                    if (!req.asset.empty()) {
                        auto res = do_submit(req);
                        if (res.accepted) {
                            scalp_manager.on_exit(exit.condition_id, exit.side,
                                                  now_ms * 1000LL);
                        }
                    }
                }
            }

            // ----------------------------------------------------------------
            // Profit-take sweep — sell open positions the instant they hit
            // profit_take_min_pct (default +20%). No momentum gate: take the
            // money. Re-entry is naturally allowed because deployed_in_market
            // counts only open shares, so we can buy → sell → buy again
            // multiple times within the same window.
            // ----------------------------------------------------------------
            if (cfg.strategy.profit_take_mode && state.autopilot.load()) {
                auto pos_snap = positions.snapshot();
                for (const auto& p : pos_snap) {
                    if (p.shares < cfg.strategy.min_position_shares || p.avg_price <= 0) continue;
                    // Skip positions managed by ScalpManager
                    if ((cfg.strategy.scalp_v2_enabled || cfg.strategy.scalp_v3_enabled) &&
                        scalp_manager.is_tracking(p.condition_id, p.side))
                        continue;
                    // Current bid for our side = price we'd realize on a sell.
                    double bid = 0;
                    {
                        std::lock_guard<std::mutex> lk(state.mu);
                        auto& books = (p.side == vp::ShareSide::UP)
                                      ? state.book_up : state.book_down;
                        if (auto it = books.find(p.condition_id);
                            it != books.end() && it->second)
                            bid = it->second->best_bid().price;
                    }
                    if (bid <= 0) continue;
                    double pnl_pct = 100.0 * (bid - p.avg_price) / p.avg_price;
                    bool hit_pct   = (pnl_pct >= cfg.strategy.profit_take_min_pct);
                    bool hit_price = (bid >= cfg.strategy.profit_take_price_thresh);
                    if (!hit_pct && !hit_price) continue;

                    // SMART EXIT: a winner near settlement with >=2 conviction
                    // votes rides to $1 (exit fee -> 0 there) instead of paying
                    // the taker fee to bank early. The bid>=thresh trigger still
                    // banks unconditionally (small remaining upside).
                    if (cfg.strategy.smart_exit_mode && hit_pct && !hit_price) {
                        int64_t secs_left = (p.window_close_ms - now_ms) / 1000;
                        if (secs_left > 0 &&
                            secs_left <= cfg.strategy.ride_to_settle_secs) {
                            int votes = conviction_votes(p.condition_id, p.side, p.asset);
                            if (votes >= 2) {
                                auto& last = smart_log_ms["pt|" + p.condition_id +
                                                          vp::to_string(p.side)];
                                if (now_ms - last > 15000) {
                                    last = now_ms;
                                    spdlog::info("RIDE-TO-SETTLE {} {} votes={} pnl={:+.1f}% t={}s — skipping profit-take, riding to $1",
                                                 p.asset, vp::to_string(p.side),
                                                 votes, pnl_pct, secs_left);
                                }
                                continue;
                            }
                        }
                    }

                    spdlog::info("PROFIT-TAKE {} {} {:.0f}sh avg={:.3f} bid={:.3f} "
                                 "pnl={:+.1f}% trigger={}",
                                 p.asset, vp::to_string(p.side), p.shares,
                                 p.avg_price, bid, pnl_pct,
                                 hit_price ? "price>=0.90" : "pct>=35%");
                    vp::OrderRequest req;
                    req.condition_id    = p.condition_id;
                    req.asset           = p.asset;
                    req.horizon_s       = p.horizon_s;
                    req.token_id        = p.token_id;
                    req.side            = p.side;
                    req.dir             = vp::OrderDir::Sell;
                    req.size_shares     = 0;  // sell all
                    req.source          = "PROFIT_TAKE";
                    req.window_close_ms = p.window_close_ms;
                    {
                        std::lock_guard<std::mutex> lk(state.mu);
                        auto mit = std::find_if(state.markets.begin(), state.markets.end(),
                            [&](const vp::PmMarket& m){ return m.condition_id == p.condition_id; });
                        if (mit != state.markets.end()) {
                            req.neg_risk  = mit->neg_risk;
                            req.tick_size = mit->tick_size;
                        }
                    }
                    do_submit(req);
                }
            }

            // ----------------------------------------------------------------
            // Stop-loss sweep — auto-sell at an explicitly configured bid price,
            // or fall back to a percentage drawdown from average cost. Runs
            // regardless of autopilot state (risk management).
            // ----------------------------------------------------------------
            if (cfg.strategy.stop_loss_mode) {
                auto pos_snap = positions.snapshot();
                for (const auto& p : pos_snap) {
                    if (p.shares < cfg.strategy.min_position_shares || p.avg_price <= 0) continue;
                    // Min-hold: a freshly-opened position's bid-marked P&L is mostly the
                    // spread, not a real adverse move. Don't stop-loss (and reverse) it
                    // until it's had time to prove out — breaks the instant-stop churn.
                    if (cfg.strategy.stop_loss_min_hold_secs > 0 &&
                        now_ms - p.opened_ms <
                            (int64_t)cfg.strategy.stop_loss_min_hold_secs * 1000LL)
                        continue;
                    // Positions managed by ScalpManager are exempt from the
                    // normal stop-loss threshold (the scalp trailing stop is
                    // their loss exit) but NOT from the hard_stop_pct
                    // catastrophe floor — a scalp position whose trailing stop
                    // is disabled previously had NO dollar stop at all here
                    // and could only ride to settlement.
                    const bool scalp_managed =
                        (cfg.strategy.scalp_v2_enabled || cfg.strategy.scalp_v3_enabled) &&
                        scalp_manager.is_tracking(p.condition_id, p.side);
                    // Book-freshness guard: never realize a stop-loss off a stale book.
                    // On restart/reconnect a not-yet-refreshed book can show a fake low
                    // bid; min_hold only protects freshly-OPENED positions, not RESTORED
                    // ones, so a carried position could be dumped at a stale price the
                    // instant the engine comes back. Wait for a live quote before selling.
                    if (!book_fresh(p.token_id, now_ms)) continue;
                    // Current bid for our side = price we'd realize on a sell.
                    double bid = 0;
                    {
                        std::lock_guard<std::mutex> lk(state.mu);
                        auto& books = (p.side == vp::ShareSide::UP)
                                      ? state.book_up : state.book_down;
                        if (auto it = books.find(p.condition_id);
                            it != books.end() && it->second)
                            bid = it->second->best_bid().price;
                    }
                    if (bid <= 0) continue;
                    const bool absolute_price_stop =
                        std::isfinite(cfg.strategy.stop_loss_price) &&
                        cfg.strategy.stop_loss_price > 0.0 &&
                        cfg.strategy.stop_loss_price < 1.0;
                    double threshold = scalp_managed
                        ? p.avg_price * (1.0 - cfg.strategy.hard_stop_pct / 100.0)
                        : (absolute_price_stop
                              ? cfg.strategy.stop_loss_price
                              : p.avg_price * (1.0 - cfg.strategy.stop_loss_pct / 100.0));
                    if (bid > threshold) continue;

                    // SMART EXIT: the bid is below the stop threshold, but is the
                    // position actually WRONG, or is the book just dipping? If any
                    // conviction signal (oracle / Coinbase / predictor) still favors
                    // our side, HOLD — 67% of blind stops would have settled WIN.
                    // The hard_stop_pct catastrophe floor fires regardless of votes.
                    if (cfg.strategy.smart_exit_mode) {
                        bool catastrophic = bid <= p.avg_price *
                            (1.0 - cfg.strategy.hard_stop_pct / 100.0);
                        if (!catastrophic) {
                            int votes = conviction_votes(p.condition_id, p.side, p.asset);
                            if (votes > cfg.strategy.smart_stop_max_votes) {
                                auto& last = smart_log_ms["sl|" + p.condition_id +
                                                          vp::to_string(p.side)];
                                if (now_ms - last > 15000) {
                                    last = now_ms;
                                    spdlog::info("SMART-HOLD {} {} votes={} dip={:.1f}% — signals still favor us, not stopping",
                                                 p.asset, vp::to_string(p.side), votes,
                                                 100.0 * (bid - p.avg_price) / p.avg_price);
                                }
                                continue;
                            }
                        }
                    }

                    double loss_pct = 100.0 * (bid - p.avg_price) / p.avg_price;
                    spdlog::info("STOP-LOSS {} {} {:.0f}sh avg={:.3f} bid={:.3f} "
                                 "threshold={:.3f} trigger={} loss={:.1f}%",
                                 p.asset, vp::to_string(p.side), p.shares,
                                 p.avg_price, bid, threshold,
                                 absolute_price_stop ? "absolute-price" : "drawdown-pct",
                                 loss_pct);
                    vp::OrderRequest req;
                    req.condition_id    = p.condition_id;
                    req.asset           = p.asset;
                    req.horizon_s       = p.horizon_s;
                    req.token_id        = p.token_id;
                    req.side            = p.side;
                    req.dir             = vp::OrderDir::Sell;
                    req.size_shares     = 0;  // sell all
                    req.source          = "STOP_LOSS";
                    req.window_close_ms = p.window_close_ms;
                    {
                        std::lock_guard<std::mutex> lk(state.mu);
                        auto mit = std::find_if(state.markets.begin(), state.markets.end(),
                            [&](const vp::PmMarket& m) { return m.condition_id == p.condition_id; });
                        if (mit != state.markets.end()) {
                            req.neg_risk  = mit->neg_risk;
                            req.tick_size = mit->tick_size;
                        }
                    }
                    do_submit(req);
                    // Record stop-loss cooldown — block re-entry on this (market, side) for 10 min.
                    stop_cooldown_until[p.condition_id + "|" + vp::to_string(p.side)] =
                        now_ms + std::max(0, cfg.strategy.stop_loss_cooldown_secs) * 1000LL;
                    spdlog::info("stop-loss cooldown set: {} {} — no re-entry for {}s",
                                 p.asset, vp::to_string(p.side),
                                 cfg.strategy.stop_loss_cooldown_secs);

                    // ── Stop-loss reverse: immediately buy the opposite side ──────
                    if (cfg.strategy.stop_loss_reverse) {
                        int64_t secs_left_sl = (p.window_close_ms - now_ms) / 1000;
                        if (secs_left_sl > cfg.strategy.reverse_min_secs_left) {
                            vp::ShareSide opp = (p.side == vp::ShareSide::UP)
                                                 ? vp::ShareSide::DOWN : vp::ShareSide::UP;
                            double opp_ask = 0;
                            std::string opp_token;
                            bool opp_neg_risk = true;
                            double opp_tick   = 0.01;
                            {
                                std::lock_guard<std::mutex> lk(state.mu);
                                auto& opp_books = (opp == vp::ShareSide::UP)
                                                  ? state.book_up : state.book_down;
                                if (auto it = opp_books.find(p.condition_id);
                                    it != opp_books.end() && it->second)
                                    opp_ask = it->second->best_ask().price;
                                auto mit = std::find_if(state.markets.begin(), state.markets.end(),
                                    [&](const vp::PmMarket& m) { return m.condition_id == p.condition_id; });
                                if (mit != state.markets.end()) {
                                    opp_token    = (opp == vp::ShareSide::UP)
                                                   ? mit->yes_token_id : mit->no_token_id;
                                    opp_neg_risk = mit->neg_risk;
                                    opp_tick     = mit->tick_size;
                                }
                            }
                            // Require a fresh opposite-side book + pass the anti-churn
                            // guard (cooldown + per-window cap). reverse_allowed() records
                            // on fire, so it stays last in the && chain.
                            if (opp_ask > 0 && opp_ask < cfg.strategy.hard_share_price_ceil
                                && !opp_token.empty()
                                && book_fresh(opp_token, now_ms)
                                && reverse_allowed(p.condition_id, p.window_close_ms, now_ms)) {
                                vp::OrderRequest rev;
                                rev.condition_id    = p.condition_id;
                                rev.asset           = p.asset;
                                rev.horizon_s       = p.horizon_s;
                                rev.token_id        = opp_token;
                                rev.side            = opp;
                                rev.dir             = vp::OrderDir::Buy;
                                rev.size_usd        = cfg.strategy.per_click_size_usd;
                                rev.limit_price     = std::min(cfg.strategy.hard_share_price_ceil,
                                                               opp_ask * (1.0 + cfg.strategy.reverse_slippage_pct) +
                                                               cfg.strategy.reverse_slippage_extra);
                                rev.source          = "STOP_LOSS_REVERSE";
                                rev.window_close_ms = p.window_close_ms;
                                rev.neg_risk        = opp_neg_risk;
                                rev.tick_size       = opp_tick;
                                rev.fee_rate        = cfg.strategy.taker_fee_rate;
                                rev.fee_exponent    = cfg.strategy.fee_exponent;
                                spdlog::info("STOP-LOSS-REVERSE {} {} opp_ask={:.3f} t={}s",
                                             p.asset, vp::to_string(opp), opp_ask, secs_left_sl);
                                do_submit(rev);
                            }
                        }
                    }
                }
            }

            // ----------------------------------------------------------------
            // Timed-exit sweep — auto-sell LOSING open positions N seconds
            // before window close. Runs regardless of autopilot state (risk mgmt).
            // ----------------------------------------------------------------
            if (cfg.strategy.timed_exit_secs > 0) {
                const int64_t flatten_before_ms =
                    (int64_t)cfg.strategy.timed_exit_secs * 1000LL;
                auto pos_snap = positions.snapshot();
                for (const auto& p : pos_snap) {
                    if (p.shares < cfg.strategy.min_position_shares || p.window_close_ms <= 0) continue;
                    int64_t ms_to_close = p.window_close_ms - now_ms;
                    if (ms_to_close > flatten_before_ms || ms_to_close < 0) continue;

                    // Only cut losing positions; let winners ride.
                    double bid = 0;
                    {
                        std::lock_guard<std::mutex> lk(state.mu);
                        auto& books = (p.side == vp::ShareSide::UP)
                                      ? state.book_up : state.book_down;
                        if (auto it = books.find(p.condition_id);
                            it != books.end() && it->second)
                            bid = it->second->best_bid().price;
                    }
                    if (bid <= 0 || bid >= p.avg_price) continue;  // skip if winning or no bids
                    if (bid < cfg.strategy.timed_exit_min_bid) continue;

                    // Throttle: attempt at most every 2 seconds per position.
                    std::string te_key =
                        p.condition_id + (p.side == vp::ShareSide::UP ? "|U" : "|D");
                    auto& last_ms = timed_exit_last_ms[te_key];
                    if (now_ms - last_ms < std::max(0, cfg.strategy.timed_exit_throttle_ms)) continue;
                    last_ms = now_ms;

                    spdlog::info("TIMED-EXIT {} {} {:.0f}sh avg={:.3f} bid={:.3f} "
                                 "secs_to_close={:.1f} (cutting loss)",
                                 p.asset, vp::to_string(p.side), p.shares,
                                 p.avg_price, bid, ms_to_close / 1000.0);
                    vp::OrderRequest req;
                    req.condition_id    = p.condition_id;
                    req.asset           = p.asset;
                    req.horizon_s       = p.horizon_s;
                    req.token_id        = p.token_id;
                    req.side            = p.side;
                    req.dir             = vp::OrderDir::Sell;
                    req.size_shares     = 0;  // sell all
                    req.source          = "TIMED_EXIT";
                    req.window_close_ms = p.window_close_ms;
                    {
                        std::lock_guard<std::mutex> lk(state.mu);
                        auto mit = std::find_if(
                            state.markets.begin(), state.markets.end(),
                            [&](const vp::PmMarket& m) {
                                return m.condition_id == p.condition_id;
                            });
                        if (mit != state.markets.end()) {
                            req.neg_risk  = mit->neg_risk;
                            req.tick_size = mit->tick_size;
                        }
                    }
                    do_submit(req);
                }
            }
            // Live pUSD balance refresh — update wallet_baseline every 30s.
            // Only override TOML baseline if CLOB returns non-zero pUSD;
            // a fresh wallet with no CLOB deposits returns 0, but the user's
            // actual USDC.e sits on-chain, not inside the CLOB.
            if (live_exec && (now_ms - last_balance_refresh_ms) >=
                    std::max(1, cfg.runtime.wallet_balance_refresh_ms)) {
                last_balance_refresh_ms = now_ms;
                double bal = live_exec->fetch_balance();
                if (bal > 0.0) {
                    std::lock_guard<std::mutex> lk(state.mu);
                    state.wallet_baseline_usd = bal;
                }
                apply_runtime_limits("balance-refresh");
            }

            // CLOB session heartbeat every 5s (live only) — keeps the session
            // alive so the exchange doesn't cancel orders for inactivity.
            // Must echo the heartbeat_id returned by the previous response.
            if (live_exec && (now_ms - last_heartbeat_ms) >=
                    std::max(1, cfg.runtime.live_heartbeat_ms)) {
                last_heartbeat_ms = now_ms;
                std::string new_id = live_exec->post_heartbeat(heartbeat_id);
                if (!new_id.empty()) heartbeat_id = new_id;
            }

            // Drive resting-order state from the engine cadence. The executors
            // internally enforce their poll/reprice timers, so this is cheap on
            // sweeps where no maker action is due.
            std::size_t maker_changes = live_exec
                ? live_exec->maintain_open_orders(book_lookup, now_ms,
                                                  [&] { return !uncertain_order_halt.load(); })
                : paper->maintain_open_orders([&](const std::string& cid,vp::ShareSide side) -> const vp::OrderBook* {
                    if(cfg.strategy.recycler_mode!="off") {
                        std::string token;
                        { std::lock_guard<std::mutex> lk(state.mu);
                          for(const auto& m:state.markets) if(m.condition_id==cid)
                              token=side==vp::ShareSide::UP?m.yes_token_id:m.no_token_id; }
                        if(!book_fresh(token,now_ms))return nullptr;
                    }
                    return book_lookup(cid,side);
                }, now_ms);
            if (maker_changes > 0) {
                std::lock_guard<std::mutex> lk(state.mu);
                state.positions_snap = positions.snapshot();
                state.deployed_usd = positions.total_deployed_usd();
                state.net_pnl_usd = positions.net_pnl_usd();
            }

            // Dedicated PAPER strategy controller, after maker fill reconciliation.
            // It continues managing held inventory while entry autopilot is paused.
            if (!live_exec && (cfg.strategy.recycler_mode!="off" || recycler.engaged())) {
                std::vector<vp::RecyclerBook> books;
                {
                    std::lock_guard<std::mutex> lk(state.mu);
                    for(const auto& m:state.markets) {
                        const bool recycler_horizon_supported=
                            m.horizon_seconds==300 || m.horizon_seconds==900 ||
                            (cfg.strategy.recycler_maniac_mode &&
                             (m.horizon_seconds==3600 || m.horizon_seconds==14400 ||
                              m.horizon_seconds==86400));
                        if(!recycler_horizon_supported ||
                           m.window_open_ms>now_ms)continue;
                        for(auto side:{vp::ShareSide::UP,vp::ShareSide::DOWN}) {
                            auto& map=side==vp::ShareSide::UP?state.book_up:state.book_down;
                            auto it=map.find(m.condition_id);if(it==map.end() || !it->second)continue;
                            vp::RecyclerBook b;auto& req=b.market;
                            req.condition_id=m.condition_id;req.asset=m.asset;req.horizon_s=m.horizon_seconds;
                            req.token_id=side==vp::ShareSide::UP?m.yes_token_id:m.no_token_id;
                            req.side=side;req.window_close_ms=m.window_close_ms;
                            req.tick_size=m.tick_size;req.neg_risk=m.neg_risk;
                            b.window_open_ms=m.window_open_ms;
                            b.settlement_window_s=m.settlement_window_s;
                            b.bid=it->second->best_bid().price;b.ask=it->second->best_ask().price;
                            b.generation=it->second->generation();
                            auto stamp=state.last_book_update_ms.find(req.token_id);
                            b.updated_ms=stamp==state.last_book_update_ms.end()?0:stamp->second;
                            const auto ref_key=m.reference_key();
                            b.reference_key=ref_key;
                            // Match the Prediction Board: use the official
                            // unaveraged Chainlink spot against the market's
                            // settlement-specific opening strike.
                            const auto spot_key=m.reference_price_key();
                            b.reference_price=oracle_buf.latest(spot_key);
                            const double spot_ts=oracle_buf.latest_timestamp(spot_key);
                            const double ref_ts=oracle_buf.latest_timestamp(ref_key);
                            b.reference_updated_ms=(spot_ts>0 && ref_ts>0)
                                ? static_cast<int64_t>(std::min(spot_ts,ref_ts)*1000.0) : 0;
                            b.reference_strike=oracle_buf.lookup_at_or_before(
                                ref_key,m.window_open_ms/1000.0);
                            books.push_back(b);
                        }
                    }
                }
                for(auto& b:books) {
                    if(b.reference_strike>0 || b.reference_key.empty()) continue;
                    auto& cached=recycler_strikes[b.market.condition_id];
                    if(cached.first>0) {
                        b.reference_strike=cached.first;
                        continue;
                    }
                    if(now_ms-cached.second<1000) continue;
                    cached.second=now_ms;
                    cached.first=pg->oracle_price_at_or_before(b.reference_key,b.window_open_ms);
                    if(cached.first>0) b.reference_strike=cached.first;
                }
                // Run the exact shared current-window reference-v3 forecast
                // used by Prediction Board once per market, then attach it to
                // both UP and DOWN books for side ranking.
                std::map<std::string,vp::engine::Forecast> board_forecasts;
                std::map<std::string,double> forecast_sigma_per_sec;
                std::map<std::string,std::pair<vp::engine::VolatilityEstimate,double>> vol_by_asset;
                for(auto& b:books) {
                    const auto& cid=b.market.condition_id;
                    auto forecast_it=board_forecasts.find(cid);
                    if(forecast_it==board_forecasts.end()) {
                        auto vol_it=vol_by_asset.find(b.market.asset);
                        if(vol_it==vol_by_asset.end()) {
                            const auto closes=candle_store.copy_recent_closes(
                                b.market.asset,vp::engine::Timeframe::OneMinute,1441);
                            const auto last_bar=candle_store.copy_recent_bars(
                                b.market.asset,vp::engine::Timeframe::OneMinute,1);
                            const auto vol=vp::engine::estimate_volatility(closes);
                            const double candle_age=last_bar.empty() ? -1.0 :
                                (now_ms-last_bar.back().ts_ms-60000)/1000.0;
                            vol_it=vol_by_asset.emplace(b.market.asset,
                                std::make_pair(vol,candle_age)).first;
                        }
                        const double reference_age=b.reference_updated_ms>0
                            ? (now_ms-b.reference_updated_ms)/1000.0 : -1.0;
                        const vp::engine::ForecastInput input{
                            b.reference_price,b.reference_strike,
                            std::max(0.0,(b.market.window_close_ms-now_ms)/1000.0),
                            vol_it->second.first.sigma_minute,reference_age,
                            vol_it->second.second,vol_it->second.first.samples,
                            b.settlement_window_s,0.0};
                        const auto forecast=vp::engine::forecast(input);
                        forecast_it=board_forecasts.emplace(cid,forecast).first;
                        forecast_sigma_per_sec[cid]=vol_it->second.first.sigma_minute/std::sqrt(60.0);
                    }
                    b.board_forecast=forecast_it->second;
                    b.forecast_sigma_per_sec=forecast_sigma_per_sec[cid];
                }
                const bool entries=state.autopilot.load() && !state.kill_switch.load() &&
                    !uncertain_order_halt.load() && !state.daily_kill_fired.load() && now_ms>=state.pause_until_ms &&
                    now_ms-state.startup_ts_ms>=cfg.strategy.autopilot_startup_delay_secs*1000LL;
                auto actions=recycler.step(cfg.strategy,now_ms,books,positions.snapshot(),
                    paper->open_orders(),positions.net_pnl_usd(),entries,
                    cfg.risk.clob_min_shares_fallback,state.runtime_max_deployed_usd);
                for(auto& a:actions) {
                    if(!a.cancel_id.empty()) {
                        if(!a.reason.empty())
                            spdlog::info("RECYCLER cancel order={} reason={}",a.cancel_id,a.reason);
                        paper->cancel_order(a.cancel_id);
                    }
                    else do_submit(a.order);
                }
                if(!actions.empty() || now_ms-last_recycler_log_ms>=15000) {
                    last_recycler_log_ms=now_ms;
                    spdlog::info("RECYCLER mode={} state={} cycles={} net={:.4f} quotes={} PAPER_SIMULATED",
                        cfg.strategy.recycler_mode,recycler.status,recycler.completed_cycles,
                        recycler.session_net,paper->open_order_count());
                }
            }

            // Legacy path retained for compatibility; config parsing forces this knob to zero.
            if (cfg.risk.daily_loss_kill_pct > 0.0
                && !state.daily_kill_fired.load()
                && (now_ms - last_kill_check_ms) >=
                    std::max(1, cfg.risk.daily_kill_check_interval_ms)) {
                last_kill_check_ms = now_ms;
                // Startup grace: don't arm the kill until restored-position cleanup
                // (settles/exits) has had time to finish, then (re)baseline so those
                // inherited losses are excluded from this session's loss measurement.
                if (now_ms - state.startup_ts_ms
                        < (int64_t)cfg.risk.daily_kill_grace_secs * 1000LL) {
                    goto kill_check_done;
                }
                if (!kill_baseline_set) {
                    session_start_net_pnl = positions.net_pnl_usd();
                    kill_baseline_set = true;
                    spdlog::info("daily-kill armed: baseline net_pnl=${:.4f} after {}s grace",
                                 session_start_net_pnl.load(), cfg.risk.daily_kill_grace_secs);
                }
                double net_pnl       = positions.net_pnl_usd();
                double session_loss  = net_pnl - session_start_net_pnl.load();
                double kill_thresh   = state.runtime_max_deployed_usd
                                     * cfg.risk.daily_loss_kill_pct / 100.0;
                if (session_loss < -kill_thresh) {
                    state.daily_kill_fired.store(true);
                    spdlog::warn("DAILY LOSS KILL: session loss ${:.4f} exceeds {:.1f}% threshold ${:.4f}  "
                                 "— autopilot OFF, all positions flattened",
                                 -session_loss, cfg.risk.daily_loss_kill_pct, kill_thresh);
                    state.kill_switch.store(true);
                    state.autopilot.store(false);
                    // Flatten every open position
                    auto all_pos = positions.snapshot();
                    for (const auto& pos : all_pos) {
                        if (pos.shares <= cfg.strategy.dust_position_shares) continue;
                        vp::OrderRequest req;
                        req.condition_id    = pos.condition_id;
                        req.asset           = pos.asset;
                        req.horizon_s       = pos.horizon_s;
                        req.token_id        = pos.token_id;
                        req.side            = pos.side;
                        req.dir             = vp::OrderDir::Sell;
                        req.size_shares     = 0;  // sell all held shares
                        req.source          = "DAILY_KILL";
                        req.window_close_ms = pos.window_close_ms;
                        do_submit(req);
                    }
                }
            }
            kill_check_done:;  // grace-period skip target for the daily-kill block above

            // Auto-settle positions whose market window has closed — BOTH modes.
            // Polymarket UP/DOWN markets resolve on the SAME Chainlink oracle we
            // track, paying $1/$0 via on-chain CTF redemption — which is NOT a CLOB
            // fill, so live mode never received any settlement event. The old code
            // gated this to paper, so live positions lingered as phantoms forever:
            // realized PnL omitted every held-to-resolution outcome (the bulk of the
            // edge) and the exit loops kept firing ORACLE_PANIC sells into the now-
            // empty books ("no bids on book" × thousands). Settling here in live too
            // clears the tracker and books the real win/loss. (The actual cash lands
            // via Polymarket's automatic on-chain redemption; the 30s balance poll +
            // the equity snapshot below re-anchor displayed cash to the real wallet.)
            {
                const std::string settle_mode = state.mode_label;
                auto pos_snap = positions.snapshot();
                for (const auto& p : pos_snap) {
                    if (p.shares <= cfg.strategy.dust_position_shares) continue;
                    if (p.window_close_ms <= 0) continue;     // unknown close time
                    // Configurable grace period after market close. The oracle
                    // usually has the close-price tick within a few seconds,
                    // but waiting a little avoids settling before the final
                    // close price is visible.
                    if (now_ms < p.window_close_ms +
                            std::max(0, cfg.strategy.settlement_grace_ms)) continue;

                    std::optional<bool> venue_up;
                    { // Paper and live both settle only against the venue result.
                        auto& check=venue_resolutions[p.condition_id];
                        if (!check.pending.valid()) {
                            if (now_ms < check.retry_after_ms) continue;
                            check.pending=std::async(std::launch::async,
                                [clob=cfg.polymarket.clob_rest_url,gamma=cfg.polymarket.gamma_rest_url,
                                 cid=p.condition_id] {
                                    return vp::PolymarketRest(clob,gamma).resolved_up_outcome(cid);
                                });
                            continue;
                        }
                        if (check.pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                            continue;
                        venue_up=check.pending.get();
                        check.retry_after_ms=now_ms+30'000;
                        if (!venue_up.has_value()) continue;
                    }

                    // Look up the market's asset and window_open_ms for oracle-based settlement.
                    // Fallback to position metadata if the market has already dropped from the
                    // live catalog (common on restart after a window closed).
                    std::string settle_asset;
                    int64_t window_open_ms = 0;
                    {
                        std::lock_guard<std::mutex> lk(state.mu);
                        for (const auto& m : state.markets) {
                            if (m.condition_id == p.condition_id) {
                                settle_asset   = m.reference_key();
                                window_open_ms = m.window_open_ms;
                                break;
                            }
                        }
                    }
                    // Persisted market metadata identifies the reference after
                    // catalog pruning/restart. Unknown references defer settlement.
                    if (settle_asset.empty()) {
                        settle_asset = pg->market_reference_key(p.condition_id);
                        window_open_ms = p.horizon_s > 0
                            ? p.window_close_ms - (int64_t)p.horizon_s * 1000LL : 0;
                    }
                    if (settle_asset.empty() && !venue_up.has_value()) continue;

                    // Oracle price at window open (the "strike") and at window close.
                    // UP wins if oracle_at_close >= oracle_at_open (price went up).
                    double oracle_at_open  = oracle_buf.lookup_at_or_before(
                        settle_asset, (double)window_open_ms / 1000.0);
                    if (oracle_at_open <= 0)
                        oracle_at_open = pg->oracle_price_at_or_before(settle_asset, window_open_ms);

                    double oracle_at_close = oracle_buf.lookup_at_or_before(
                        settle_asset, (double)p.window_close_ms / 1000.0);
                    if (oracle_at_close <= 0)
                        oracle_at_close = pg->oracle_price_at_or_before(settle_asset, p.window_close_ms);
                    // ── Phase 4.6: paper-settlement DEFER ──────────────────
                    // If we have no oracle tick at/just-before window close,
                    // defer and retry next sweep instead of falling back to
                    // oracle_buf.latest() — that "latest" may already be the
                    // NEXT window's price (RTDS keeps ticking after close),
                    // which would settle the position against the WRONG close.
                    // The DB fallback above (vp.oracle_ticks historical rows)
                    // is a real record and is fine; anything else waits.
                    if (!venue_up.has_value() && (oracle_at_open <= 0 || oracle_at_close <= 0)) {
                        const int64_t age_ms = now_ms - p.window_close_ms;
                        spdlog::debug("PAPER SETTLE defer {} — missing oracle "
                                      "open={:.2f} close={:.2f} (age={}ms, retrying)",
                            p.condition_id.substr(0,12), oracle_at_open,
                            oracle_at_close, age_ms);
                        // Loud warn (rate-limited per position) after 10 min.
                        if (age_ms > 10 * 60 * 1000LL) {
                            auto& last = settle_defer_warned[p.condition_id];
                            if (now_ms - last > 60'000) {
                                last = now_ms;
                                spdlog::warn("PAPER SETTLE still deferred {} — no "
                                             "oracle close for {}min (RTDS gap? "
                                             "DB empty? position stuck open)",
                                    p.condition_id.substr(0,12), age_ms / 60000);
                            }
                        }
                        continue;
                    }
                    bool up_wins   = venue_up.value_or(oracle_at_close >= oracle_at_open);
                    if (venue_up.has_value()) venue_resolutions.erase(p.condition_id);
                    bool is_winner = (p.side == vp::ShareSide::UP) ? up_wins : !up_wins;
                    double settle_price = is_winner ? 1.00 : 0.00; // CTF resolves losing tokens at exactly $0

                    spdlog::info("{} SETTLE {} {} {:.4f}sh @ ${:.2f} ({}) oracle: {:.2f}→{:.2f}",
                        settle_mode, vp::to_string(p.side), p.condition_id.substr(0,12),
                        p.shares, settle_price, is_winner ? "WIN" : "LOSS",
                        oracle_at_open, oracle_at_close);

                    const double settled_shares = p.shares;
                    positions.apply_fill(
                        p.condition_id, p.asset, p.horizon_s, p.token_id,
                        p.side, vp::OrderDir::Sell,
                        settle_price, settled_shares, p.window_close_ms, 0.0);

                    // Persist settlement to Postgres as synthetic order+fill
                    {
                        std::string settle_coid = vp::core::make_uuid();
                        SessionOrderRecord settle_rec;
                        settle_rec.client_order_id = settle_coid;
                        settle_rec.source = "settle";
                        settle_rec.condition_id = p.condition_id;
                        settle_rec.asset = p.asset;
                        settle_rec.horizon_s = p.horizon_s;
                        settle_rec.token_id = p.token_id;
                        settle_rec.side = p.side;
                        settle_rec.dir = vp::OrderDir::Sell;
                        settle_rec.limit_price = settle_price;
                        settle_rec.fill_price = settle_price;
                        settle_rec.fill_shares = settled_shares;
                        settle_rec.size_usd = settle_price * settled_shares;
                        settle_rec.fee_usd = 0.0;
                        record_session_fill(std::move(settle_rec));
                        persist_q.push_wait([pg=pg.get(), settle_coid, settle_mode,
                                        cid=p.condition_id, tok=p.token_id,
                                        side=p.side, settle_price, settled_shares]() noexcept {
                            pg->insert_order(settle_coid, "", "taker", settle_mode, cid, tok,
                                             side, vp::OrderDir::Sell,
                                             settle_price, settled_shares,
                                             settle_price * settled_shares,
                                             "filled", "settle", "",settle_price,settled_shares,0.0);
                        });
                    }

                    {
                        std::lock_guard<std::mutex> lk(state.mu);
                        state.positions_snap = positions.snapshot();
                        state.deployed_usd   = positions.total_deployed_usd();
                        state.net_pnl_usd    = positions.net_pnl_usd();
                        auto st = positions.trade_stats();
                        state.trades_won     = st.wins;
                        state.trades_lost    = st.losses;

                        // Push a persistent "what just happened" record so the
                        // user can see win/loss long after the position vanishes.
                        vp::SharedState::RecentResult rr;
                        rr.ts_ms        = p.window_close_ms;
                        rr.asset        = p.asset;
                        rr.side         = p.side;
                        rr.horizon_s    = p.horizon_s;
                        rr.shares       = settled_shares;
                        rr.entry_price  = p.avg_price;
                        rr.settle_price = settle_price;
                        rr.pnl_usd      = (settle_price - p.avg_price) * settled_shares;
                        rr.win          = is_winner;
                        state.recent_results.insert(state.recent_results.begin(), rr);
                        if (state.recent_results.size() > 12)
                            state.recent_results.resize(12);

                        // Feed the WPRED calibration ledger (no-op for
                        // positions that did not come from a checkpoint).
                        wpred_cal.settle(p.condition_id + "|" +
                                             (p.side == vp::ShareSide::UP ? "UP"
                                                                          : "DOWN"),
                                         is_winner);

                        if (rr.win) {
                            state.consecutive_losses = 0;
                        } else {
                            ++state.consecutive_losses;
                            if (cfg.strategy.consecutive_loss_pause > 0 &&
                                state.consecutive_losses >=
                                    cfg.strategy.consecutive_loss_pause) {
                                state.pause_until_ms = std::max(
                                    state.pause_until_ms,
                                    now_ms + (int64_t)std::max(
                                        0, cfg.strategy.consecutive_pause_secs) * int64_t{1000});
                                spdlog::warn(
                                    "autopilot paused after {} consecutive loss(es) until {}",
                                    state.consecutive_losses, state.pause_until_ms);
                            }
                        }

                        // ── Trend filter: track which side wins ──
                        // +1 = UP settled at $1, -1 = DOWN settled at $1
                        state.settlement_trend.push_front(up_wins ? +1 : -1);
                        if ((int)state.settlement_trend.size() > cfg.strategy.trend_filter_windows)
                            state.settlement_trend.pop_back();

                        // Log window settled event
                        if (cfg.data_collection.window_events_enabled) {
                            std::string settle_payload = fmt::format(
                                R"({{"outcome":"{}","oracle_open":{},"oracle_close":{},)"
                                R"("entry_price":{},"settle_price":{},"pnl_usd":{},"win":{},"side":"{}"}})",
                                up_wins ? "UP" : "DOWN", oracle_at_open, oracle_at_close,
                                p.avg_price, settle_price, rr.pnl_usd,
                                is_winner ? "true" : "false", vp::to_string(p.side));
                            persist_q.push([pg=pg.get(), cid=p.condition_id,
                                            p=std::move(settle_payload)]() noexcept {
                                pg->insert_window_event(cid, "settled", p);
                            });
                        }
                    }
                    apply_runtime_limits("settlement");
                }
            }

            // Persist positions snapshot every 30s
            if (now_ms - last_pos_persist_ms >
                    std::max(1, cfg.runtime.position_snapshot_interval_ms)) {
                last_pos_persist_ms = now_ms;
                double deployed = positions.total_deployed_usd();
                double gross_realized = positions.realized_pnl_usd();
                double fees     = positions.total_fees_usd();
                double net_pnl  = gross_realized - fees;
                auto pos_snap = positions.snapshot();
                // Build JSON array of open positions
                std::string js = "[";
                for (const auto& p : pos_snap) {
                    if (p.shares <= cfg.strategy.dust_position_shares) continue;
                    if (js.size() > 1) js += ",";
                    js += fmt::format(
                        R"({{"cid":"{}","side":"{}","shares":{:.6f},"avg":{:.4f}}})",
                        p.condition_id, vp::to_string(p.side), p.shares, p.avg_price);
                }
                js += "]";
                // Update shared state net P&L for TUI top bar + read the cash anchor.
                // cash_usd is GROUND TRUTH: in live it's the real on-chain free pUSD
                // (wallet_baseline_usd, refreshed every 30s from /balance-allowance);
                // in paper it's the modeled baseline + net_pnl − cost-of-open.
                double cash_usd;
                {
                    std::lock_guard<std::mutex> lk(state.mu);
                    state.net_pnl_usd = net_pnl;
                    cash_usd = (state.mode_label == "LIVE")
                        ? state.wallet_baseline_usd
                        : (state.wallet_baseline_usd + net_pnl - deployed);
                }
                double equity_usd = cash_usd + deployed;   // free cash + open-position cost basis
                persist_q.push([pg=pg.get(), cash_usd, equity_usd, deployed, realized=net_pnl, js,
                                 mode=state.mode_label]() noexcept {
                    // realized_pnl is stored net-of-fees. (unrealized=0: real
                    // mark-to-market unrealized is handled by cockpit from live books.)
                    pg->snapshot_positions(mode, cash_usd, equity_usd, realized, 0.0, deployed, js);
                });
                // vp.strategy_runs bookkeeping on the same 30s cadence: one
                // row per (instance, session) with counts and net-of-fee P&L,
                // so the DB — not log tailing — stays the attribution source.
                {
                    const auto stats = positions.trade_stats();
                    const long fills_now = session_fill_count.load();
                    const double run_pnl = positions.net_pnl_usd();
                    const double run_fees = positions.total_fees_usd();
                    persist_q.push([pg = pg.get(), inst = instance_label,
                                    sess = session_id, fills_now,
                                    settles = stats.wins + stats.losses,
                                    wins = stats.wins, losses = stats.losses,
                                    pnl = run_pnl, fees = run_fees]() noexcept {
                        pg->upsert_strategy_run(inst, sess, int(fills_now),
                                                settles, wins, losses, pnl,
                                                fees, false);
                    });
                }
            }

            // Periodic market re-discovery: pre-subscribe newly listed 5m/15m
            // windows before they open. Runs every 30s; the REST call is short
            // enough at this cadence and prevents next-window book gaps.
            if (now_ms - last_rest_discover_ms >=
                    std::max(1, cfg.polymarket.poll_seconds) * 1000LL) {
                last_rest_discover_ms = now_ms;
                std::vector<vp::PmMarket> new_markets;
                try {
                    new_markets = rest.discover_updown_markets();
                } catch (const std::exception& e) {
                    // A silent discovery failure during a venue incident looks
                    // exactly like "the fleet quietly stopped trading" — make
                    // it loud (Sep 06: 46 silent minutes during the gamma/API
                    // outage while everything else kept logging).
                    spdlog::warn("market discovery failed: {} — retrying next cycle", e.what());
                }
                // Bound candidates to the same nearest-windows-per-series policy
                // as startup so re-discovery can only ever ADD windows that are
                // inside the tradable universe — never refill the subscription set
                // with dozens of far-out windows (the disconnect root cause).
                new_markets = vp::select_active_universe(std::move(new_markets),
                                                         cfg.polymarket,
                                                         cfg.strategy.blocked_assets, now_ms);
                std::vector<vp::PmMarket> added;
                std::vector<std::string>   new_tokens;
                {
                    std::lock_guard<std::mutex> lk(state.mu);
                    std::unordered_set<std::string> known;
                    known.reserve(state.markets.size());
                    for (const auto& m : state.markets) known.insert(m.condition_id);
                    // Do not unsubscribe just-closed windows here. This loop runs
                    // every 30s, exactly when the observed book drop happened.
                    // The dedicated prune path above removes closed/no-position
                    // markets after its grace period, while this refresh loop only
                    // pre-subscribes newly discovered upcoming markets.
                    for (auto& m : new_markets) {
                        if (known.count(m.condition_id)) continue;
                        bool blocked = false;
                        for (const auto& b : cfg.strategy.blocked_assets)
                            if (m.asset == b) { blocked = true; break; }
                        if (blocked) continue;  // skip blocked assets entirely - don't add to state.markets either
                        new_tokens.push_back(m.yes_token_id);
                        new_tokens.push_back(m.no_token_id);
                        state.book_up[m.condition_id]   = std::make_shared<vp::OrderBook>();
                        state.book_down[m.condition_id] = std::make_shared<vp::OrderBook>();
                        state.token_to_cond[m.yes_token_id] = m.condition_id;
                        state.token_to_cond[m.no_token_id]  = m.condition_id;
                        state.token_to_side[m.yes_token_id] = vp::ShareSide::UP;
                        state.token_to_side[m.no_token_id]  = vp::ShareSide::DOWN;
                        added.push_back(m);
                        state.markets.push_back(std::move(m));
                    }
                }
                if (!added.empty()) {
                    if (pm_md) pm_md->subscribe(new_tokens);
                    spdlog::info("market refresh: +{} new markets, subscribed {} tokens",
                                 added.size(), new_tokens.size());
                    for (const auto& m : added) {
                        persist_q.push([pg=pg.get(), m]() noexcept {
                            pg->upsert_market(m.condition_id, m.asset, m.horizon_seconds,
                                              m.target_price, m.window_open_ms, m.window_close_ms,
                                              m.yes_token_id, m.no_token_id, m.question, m.slug, m.settlement_window_s);
                        });
                        if (cfg.data_collection.window_events_enabled && m.window_open_ms > 0) {
                            persist_q.push([pg=pg.get(), cid=m.condition_id]() noexcept {
                                pg->insert_window_event(cid, "opened", "");
                            });
                        }
                    }
                } else {
                    spdlog::debug("market refresh: no new markets");
                }
            }

            // Disk space monitoring - cadence and recovery threshold are in TOML.
            static int64_t last_disk_check_ms = 0;
            if (now_ms - last_disk_check_ms >
                    std::max(1, cfg.data_collection.disk_check_interval_ms)) {
                last_disk_check_ms = now_ms;
                double free_mb = get_free_space_mb();
                bool was_full = state.disk_full.load();
                if (free_mb >= 0 && free_mb < (double)cfg.data_collection.disk_space_kill_mb) {
                    if (!was_full) {
                        state.disk_full.store(true);
                        spdlog::critical("DISK FULL: {} MB free - DISABLING data collection to preserve execution",
                                         (int)free_mb);
                        // Disable non-critical data writes
                        cfg.data_collection.oracle_ticks_enabled   = false;
                        cfg.data_collection.binance_ticks_enabled  = false;
                        cfg.data_collection.coinbase_ticks_enabled = false;
                        cfg.data_collection.window_events_enabled  = false;
                        cfg.data_collection.features_enabled      = false;
                    }
                } else if (was_full &&
                           free_mb >= (double)cfg.data_collection.disk_space_kill_mb *
                                      std::max(1.0, cfg.data_collection.disk_recovery_multiplier)) {
                    state.disk_full.store(false);
                    spdlog::info("DISK RECOVERY: {} MB free - re-enabling data collection", (int)free_mb);
                    cfg.data_collection.oracle_ticks_enabled   = true;
                    cfg.data_collection.binance_ticks_enabled  = true;
                    cfg.data_collection.coinbase_ticks_enabled = true;
                    cfg.data_collection.window_events_enabled  = true;
                    cfg.data_collection.features_enabled      = true;
                }
            }

            // Orderbook aggregated health check: log summary every 30s.
            // Also reports WS connection status for cross-referencing.
            if (now_ms - state.last_ob_health_log_ms >
                    std::max(1, cfg.polymarket.book_health_log_interval_ms)) {
                state.last_ob_health_log_ms = now_ms;
                int stale_count = 0, total = 0, never = 0;
                int64_t oldest_ms = 0;
                // Real market-data WS state (was hardcoded false → the health log
                // always lied "ws=disconnected" regardless of the actual socket).
                bool ws_connected = pm_md && pm_md->connected();
                {
                    std::lock_guard<std::mutex> lk(state.mu);
                    for (const auto& kv : state.last_book_update_ms) {
                        ++total;
                        int64_t age = now_ms - kv.second;
                        if (age > std::max(1, cfg.polymarket.book_health_stale_ms)) {
                            ++stale_count;
                            if (age > oldest_ms) oldest_ms = age;
                        }
                    }
                    for (const auto& m : state.markets) {
                        if (state.last_book_update_ms.count(m.yes_token_id) == 0) ++never;
                        if (state.last_book_update_ms.count(m.no_token_id) == 0) ++never;
                    }
                }
                if (stale_count > 0 || never > 0) {
                    spdlog::warn("orderbook health: {}/{} stale ({} never updated, oldest={}ms, ws={})",
                                 stale_count, total, never, oldest_ms,
                                 ws_connected ? "connected" : "disconnected");
                } else if (total > 0) {
                    spdlog::info("orderbook health: {}/{} fresh", total, total);
                }
            }

            // REST book seeding: refresh stale books via CLOB /books.
            // Keep this aligned with the entry freshness guard; otherwise the
            // bot can reject every signal for stale books while waiting minutes
            // for the old 120s recovery path to fire.
            static int64_t last_rest_seed_ms = 0;
            static int64_t last_rest_seed_log_ms = 0;
            static int rest_seed_failures = 0;
            const int64_t rest_seed_base_ms =
                std::max(1, cfg.polymarket.rest_book_seed_interval_ms);
            const int rest_seed_shift = std::min(rest_seed_failures, 4);
            const int64_t rest_seed_retry_ms = std::min<int64_t>(
                5 * 60 * 1000LL, rest_seed_base_ms * (1LL << rest_seed_shift));
            if (now_ms - last_rest_seed_ms >=
                    rest_seed_retry_ms) {
                last_rest_seed_ms = now_ms;
                std::vector<std::string> seed_tokens;
                const int64_t seed_age_ms = std::max<int64_t>(
                    std::max(1, cfg.polymarket.rest_book_seed_min_age_ms),
                    cfg.strategy.max_book_age_ms);
                int stale_seed_count = 0;
                int never_seed_count = 0;
                int desync_seed_count = 0;
                {
                    std::lock_guard<std::mutex> lk(state.mu);
                    std::unordered_set<std::string> queued;
                    auto add_seed = [&](const std::string& tok, int kind) {
                        if (queued.insert(tok).second) {
                            seed_tokens.push_back(tok);
                            if      (kind == 1) ++never_seed_count;
                            else if (kind == 2) ++desync_seed_count;
                            else                ++stale_seed_count;
                        }
                    };
                    // Targeted: books whose pushed BBO disagreed with our ladder.
                    for (const auto& tok : state.bbo_desync_tokens)
                        add_seed(tok, 2);
                    state.bbo_desync_tokens.clear();
                    for (const auto& kv : state.last_book_update_ms) {
                        if (now_ms - kv.second > seed_age_ms)
                            add_seed(kv.first, 0);
                    }
                    // Also seed tokens that have never received ANY update
                    for (const auto& m : state.markets) {
                        if (state.last_book_update_ms.count(m.yes_token_id) == 0)
                            add_seed(m.yes_token_id, 1);
                        if (state.last_book_update_ms.count(m.no_token_id) == 0)
                            add_seed(m.no_token_id, 1);
                    }
                }
                if (!seed_tokens.empty()) {
                    auto rest_books = rest.fetch_orderbooks(seed_tokens);
                    if (rest_books.empty()) {
                        ++rest_seed_failures;
                        if (rest_seed_failures == 1 || rest_seed_failures % 4 == 0) {
                            const int next_shift = std::min(rest_seed_failures, 4);
                            const int64_t next_retry_ms = std::min<int64_t>(
                                5 * 60 * 1000LL,
                                rest_seed_base_ms * (1LL << next_shift));
                            spdlog::warn("rest book seed returned no books; backing off to {}ms (failures={})",
                                         next_retry_ms, rest_seed_failures);
                        }
                    } else {
                        rest_seed_failures = 0;
                    }
                    int refreshed = 0;
                    for (const auto& bu : rest_books) {
                        std::lock_guard<std::mutex> lk(state.mu);
                        auto cit = state.token_to_cond.find(bu.token_id);
                        if (cit == state.token_to_cond.end()) continue;
                        auto sit = state.token_to_side.find(bu.token_id);
                        if (sit == state.token_to_side.end()) continue;
                        auto bit = (sit->second == vp::ShareSide::UP)
                            ? state.book_up.find(cit->second)
                            : state.book_down.find(cit->second);
                        if (bit == state.book_up.end() && sit->second == vp::ShareSide::UP) continue;
                        if (bit == state.book_down.end() && sit->second == vp::ShareSide::DOWN) continue;
                        auto b = (sit->second == vp::ShareSide::UP)
                            ? state.book_up[cit->second]
                            : state.book_down[cit->second];
                        if (b) {
                            b->apply_snapshot(bu.bids, bu.asks);
                            state.last_book_update_ms[bu.token_id] = now_ms;
                            ++refreshed;
                        }
                    }
                    if (refreshed > 0 &&
                        now_ms - last_rest_seed_log_ms >
                            std::max(1, cfg.polymarket.rest_book_seed_log_interval_ms)) {
                        last_rest_seed_log_ms = now_ms;
                        spdlog::info("rest book seed: refreshed {}/{} tokens via /books ({} stale +{} never-updated +{} bbo-desync)",
                                     refreshed, seed_tokens.size(),
                                     stale_seed_count, never_seed_count,
                                     desync_seed_count);
                    }
                }
            }

            std::this_thread::sleep_for(milliseconds(200));
        }
    });

    // ── AI Trader's Brief loop ─────────────────────────────────────
    // Every kBriefPeriodMs ask the LLM for a short situational
    // commentary on the current session. Output goes to
    // (AI Trader's Brief background loop temporarily disabled — was
    // causing shutdown SIGSEGV. The panel still renders and shows a
    // static placeholder so users can verify the new build is live.
    // Re-enable after stabilizing LlmAdvisor lifetime.)
    state.trader_brief = "[ AI brief loop disabled in this build "
                         "while we debug a shutdown crash. The watchlist "
                         "now shows COIN-OP / COIN-NOW (oracle dollar prices), "
                         "and the wallet strip shows WINDOW COIN OPEN PRICE. "
                         "If you see this box, the new TUI changes are live. ]";
    state.trader_brief_ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    auto write_session_report = [&](const std::string& reason) {
        try {
            const int64_t end_ms = vp::Clock::wall_ns() / 1'000'000LL;
            std::vector<SessionOrderRecord> records;
            {
                std::lock_guard<std::mutex> lk(session_records_mu);
                records = session_records;
            }
            const auto open_positions = positions.snapshot();
            const auto stats = positions.trade_stats();
            const double realized = positions.realized_pnl_usd();
            const double fees = positions.total_fees_usd();
            const double net = positions.net_pnl_usd();
            const double deployed = positions.total_deployed_usd();
            double starting_balance = 0.0;
            {
                std::lock_guard<std::mutex> lk(state.mu);
                starting_balance = state.wallet_baseline_usd;
            }

            int accepted = 0;
            int rejected = 0;
            double buy_notional = 0.0;
            double sell_notional = 0.0;
            double recorded_fees = 0.0;
            std::map<std::string, int> source_counts;
            for (const auto& r : records) {
                source_counts[r.source.empty() ? "unknown" : r.source]++;
                if (r.accepted) {
                    ++accepted;
                    const double notional = r.fill_price * r.fill_shares;
                    if (r.dir == vp::OrderDir::Buy) buy_notional += notional;
                    else                            sell_notional += notional;
                    recorded_fees += r.fee_usd;
                } else {
                    ++rejected;
                }
            }

            std::filesystem::create_directories("session_reports");
            const std::string mode = state.mode_label;
            const std::string safe_mode = (mode == "LIVE") ? "live" : "paper";
            const std::string report_path = fmt::format(
                "session_reports/polypulse_{}{}_{}_{}.md",
                instance_label.empty() ? "" : (instance_label + "_"),
                wall_ts_compact(session_start_ms), safe_mode, session_id.substr(0, 8));
            std::ofstream out(report_path);
            if (!out) {
                spdlog::warn("session report: cannot write {}", report_path);
                return;
            }

            out << "# PolySparQ++ Session Report\n\n";
            out << "## Session\n\n";
            out << fmt::format("- Session ID: `{}`\n", session_id);
            out << fmt::format("- Instance: `{}`\n",
                               instance_label.empty() ? "default" : instance_label);
            out << fmt::format("- Mode: `{}`\n", mode);
            out << fmt::format("- Config: `{}`\n", cfg_path);
            out << fmt::format("- Started: {}\n", wall_ts_pretty(session_start_ms));
            out << fmt::format("- Ended: {}\n", wall_ts_pretty(end_ms));
            out << fmt::format("- Duration: {:.1f} minutes\n",
                               (end_ms - session_start_ms) / 60000.0);
            out << fmt::format("- Exit reason: `{}`\n\n", reason);

            out << "## PnL\n\n";
            out << fmt::format("- Starting balance: ${:.2f}\n", starting_balance);
            out << fmt::format("- Ending realized-balance estimate: ${:.2f}\n",
                               starting_balance + net);
            out << fmt::format("- Realized PnL before fees: ${:.4f}\n", realized);
            out << fmt::format("- Fees: ${:.4f}\n", fees);
            out << fmt::format("- Net realized PnL: ${:.4f}\n", net);
            out << fmt::format("- Open deployed at exit: ${:.4f}\n", deployed);
            out << fmt::format("- Closed trade record: {}W / {}L\n\n",
                               stats.wins, stats.losses);

            out << "## Activity\n\n";
            out << fmt::format("- Session records: {}\n", records.size());
            out << fmt::format("- Accepted fills: {}\n", accepted);
            out << fmt::format("- Rejected orders: {}\n", rejected);
            out << fmt::format("- Buy notional: ${:.4f}\n", buy_notional);
            out << fmt::format("- Sell notional: ${:.4f}\n", sell_notional);
            out << fmt::format("- Recorded fees: ${:.4f}\n", recorded_fees);
            if (!source_counts.empty()) {
                out << "- Sources:";
                for (const auto& kv : source_counts)
                    out << fmt::format(" {}={}", kv.first, kv.second);
                out << "\n";
            }
            out << "\n";

            out << "## Strategy Snapshot\n\n";
            out << fmt::format("- Autopilot on start: `{}`\n",
                               cfg.strategy.autopilot_on_start ? "true" : "false");
            out << fmt::format("- Entry price range: {:.3f} to {:.3f}\n",
                               cfg.strategy.min_entry_share_price,
                               cfg.strategy.max_entry_share_price);
            out << fmt::format("- Oracle lag mode: `{}`, min delta: {:.4f}%\n",
                               cfg.strategy.oracle_lag_mode ? "true" : "false",
                               cfg.strategy.min_oracle_delta_pct);
            out << fmt::format("- Order size: ${:.2f} to ${:.2f}, per-window cap: ${:.2f}\n",
                               cfg.strategy.min_size_usd,
                               cfg.strategy.max_size_usd,
                               cfg.strategy.max_per_window_usd);
            out << fmt::format("- Equity sizing: `{}`, order pct: {:.4f}\n",
                               cfg.strategy.equity_sizing_enabled ? "true" : "false",
                               cfg.strategy.equity_order_pct);
            out << fmt::format("- Scalp v3: `{}`\n",
                               cfg.strategy.scalp_v3_enabled ? "true" : "false");
            out << fmt::format("- Profit take: `{}`, stop loss: `{}`\n\n",
                               cfg.strategy.profit_take_mode ? "true" : "false",
                               cfg.strategy.stop_loss_mode ? "true" : "false");

            out << "## Open Positions At Exit\n\n";
            if (open_positions.empty()) {
                out << "None.\n\n";
            } else {
                out << "| Asset | Side | Condition | Shares | Avg | Deployed | Closes |\n";
                out << "|---|---:|---|---:|---:|---:|---:|\n";
                for (const auto& p : open_positions) {
                    out << fmt::format("| {} | {} | `{}` | {:.4f} | {:.4f} | ${:.4f} | {} |\n",
                                       md_cell(p.asset), vp::to_string(p.side),
                                       md_cell(p.condition_id.substr(0, 12)),
                                       p.shares, p.avg_price, p.shares * p.avg_price,
                                       p.window_close_ms > 0 ? wall_ts_pretty(p.window_close_ms) : "-");
                }
                out << "\n";
            }

            out << "## Trades And Orders\n\n";
            if (records.empty()) {
                out << "No orders recorded in this session.\n";
            } else {
                out << "| Time | Source | Status | Dir | Side | Asset | Condition | Shares | Price | Notional | Fee | Reason |\n";
                out << "|---|---|---|---|---:|---|---|---:|---:|---:|---:|---|\n";
                for (const auto& r : records) {
                    const double notional = r.accepted
                        ? r.fill_price * r.fill_shares
                        : r.size_usd;
                    out << fmt::format("| {} | {} | {} | {} | {} | {} | `{}` | {:.4f} | {:.4f} | ${:.4f} | ${:.4f} | {} |\n",
                                       wall_ts_pretty(r.ts_ms),
                                       md_cell(r.source),
                                       r.accepted ? "filled" : "rejected",
                                       vp::to_string(r.dir),
                                       vp::to_string(r.side),
                                       md_cell(r.asset),
                                       md_cell(r.condition_id.substr(0, 12)),
                                       r.fill_shares,
                                       r.fill_price > 0 ? r.fill_price : r.limit_price,
                                       notional,
                                       r.fee_usd,
                                       md_cell(r.reject_reason));
                }
            }
            out.flush();
            spdlog::info("session report written: {}", report_path);
        } catch (const std::exception& e) {
            spdlog::warn("session report failed: {}", e.what());
        } catch (...) {
            spdlog::warn("session report failed: unknown error");
        }
    };

    tui.run();   // blocks until 'q' or signal

    g_stop.store(true);
    // Explicitly withdraw our managed quotes; heartbeat expiry remains a second
    // line of defense if the process is killed before reaching this path.
    if (live_exec) live_exec->cancel_open_orders();
    else           paper->cancel_open_orders();
    g_tui.store(nullptr);
    spdlog::info("shutdown: tui exited, restoring terminal");
    tui.shutdown();
    write_session_report("shutdown");

    // Close the vp.strategy_runs row for this session (stamps ended_at).
    if (pg) {
        const auto stats = positions.trade_stats();
        pg->upsert_strategy_run(instance_label, session_id,
                                int(session_fill_count.load()),
                                stats.wins + stats.losses, stats.wins,
                                stats.losses, positions.net_pnl_usd(),
                                positions.total_fees_usd(), true);
    }

    // Hard safety for genuinely wedged shutdowns. The previous detached 3-second
    // timer routinely killed healthy teardown while 5s/8s keepalive sleepers were
    // still joining. Keepalive waits are now interruptible, and this watchdog is
    // cancelled on a clean shutdown.
    std::mutex watchdog_mu;
    std::condition_variable watchdog_cv;
    bool shutdown_complete = false;
    std::thread watchdog([&] {
        std::unique_lock<std::mutex> lk(watchdog_mu);
        if (watchdog_cv.wait_for(lk, std::chrono::seconds(15),
                                 [&] { return shutdown_complete; })) return;
        spdlog::critical("shutdown watchdog: teardown exceeded 15s; forcing _exit(1)");
        std::fflush(nullptr);
        std::_Exit(1);
    });
    // NOTE: intentionally NOT detached — joined after teardown below (join on a
    // detached thread throws std::system_error Invalid argument).

    persist_q.shutdown();
    if (bootstrap_th.joinable()) bootstrap_th.join();
    refresh_th.join();
    persist_th.join();
    if (coinbase_th.joinable()) coinbase_th.join();
    if (binance)  binance->stop();
    if (coinbase_ws) coinbase_ws->stop();
    if (pm_md)    pm_md->stop();
    if (user_ws)  user_ws->stop();
    if (rtds)     rtds->stop();

    {
        std::lock_guard<std::mutex> lk(watchdog_mu);
        shutdown_complete = true;
    }
    watchdog_cv.notify_all();
    watchdog.join();
    spdlog::info("clean shutdown");
    // libpqxx 7.10's DSO finalizer double-frees during exit() — every
    // SIGTERM shutdown aborted with "double free or corruption (!prev)"
    // inside __cxa_finalize AFTER the teardown above had already completed.
    // Teardown is done and queues are flushed, so skip C++ static
    // destruction entirely.
    spdlog::shutdown();
    std::fflush(nullptr);
    std::_Exit(0);
}
