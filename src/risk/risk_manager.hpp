#pragma once
// 12-Check Risk Manager — pure function port of Homerun risk_manager.py (235 lines).
// Centralizes ALL risk checks currently scattered across submit(), main.cpp, positions.cpp.
// Each check returns RiskCheck{key, passed, detail, score}; failures aggregate into
// one reason string. Called from the gate pipeline (L1_CACHED tier).

#include "core/types.hpp"
#include <string>
#include <vector>
#include <unordered_map>

namespace vp::risk {

struct RiskCheck {
    std::string key;          // stable identifier, e.g. "global_daily_loss_realized"
    bool        passed = true;
    std::string detail;       // human-readable: "realized -$18.50 exceeds -$15.00 limit"
    double      score  = 0.0; // 0.0 = safe, 1.0 = at limit, >1.0 = breach
};

struct RiskConfig {
    // ── Daily loss limits (realized AND realized+unrealized) ─────────────
    // Fixed loss-budget basis, shared with the executor kill switch.
    double global_daily_loss_pct      = 15.0;
    double trader_daily_loss_pct      = 15.0;   // same, but per-trader (same as global here)

    // ── Consecutive loss halt ────────────────────────────────────────────
    int    consecutive_loss_halt      = 10;     // halt after N consecutive losing trades
    int    consecutive_loss_cooldown_s = 600;   // cooldown seconds after halt

    // ── Order/Notional caps ──────────────────────────────────────────────
    int    max_orders_per_cycle       = 10;     // max entries per market window
    double max_notional_per_order_usd = 5.0;    // max $ per single order
    double max_gross_exposure_usd     = 50.0;   // max Σ|position| across all markets
    double max_deployed_usd           = 20.0;   // max Σ(shares * avg_price) = deployed

    // ── Open orders/positions limits ─────────────────────────────────────
    int    max_open_orders            = 20;     // max pending orders at CLOB
    int    max_open_positions         = 10;     // max markets with open position
    int    max_positions_per_market   = 2;      // max UP+DOWN per condition_id

    // ── Per-market exposure ──────────────────────────────────────────────
    double max_exposure_per_market_usd = 25.0;  // max deployed in single market
};

struct RiskInput {
    // Wallet / equity
    double wallet_baseline_usd = 100.0;        // starting bankroll
    double net_pnl_usd       = 0.0;            // realized - fees (includes yield)
    double unrealized_pnl_usd = 0.0;           // mark-to-market P&L on open positions
    double loss_period_start_net_pnl_usd = 0.0;
    // Zero uses the starting wallet baseline. Main supplies its deployed cap,
    // matching the existing session kill; never use shrinking current equity.
    double daily_loss_basis_usd = 0.0;

    // Position state
    double deployed_usd            = 0.0;      // Σ open.shares * open.avg_price
    int    open_positions_count    = 0;        // markets with position > dust
    int    open_orders_count       = 0;        // pending at CLOB
    int    positions_in_this_market = 0;       // UP+DOWN for the market we're entering

    // Per-market exposure
    double exposure_this_market_usd = 0.0;     // deployed in the target market

    // Order being checked
    double order_notional_usd       = 0.0;     // shares * limit_price for this order
    int    orders_this_cycle        = 0;       // entries already in this window

    // Trade history
    int    consecutive_losses       = 0;       // current streak
    int64_t last_loss_ts_ms         = 0;       // timestamp of last losing trade
    int64_t loss_pause_until_ms     = 0;       // scheduled streak pause; 0 = indefinite halt
    int64_t now_ms                  = 0;       // current wall-clock ms

    // Risk config (from StrategyHot + RuntimeConfig)
    RiskConfig cfg;
};

// Run all 12 checks. Returns vector of results (all checks run; caller decides).
// Checks are ordered: catastrophic kills first, then soft limits.
std::vector<RiskCheck> run_all_checks(const RiskInput& in);

// Aggregate failures into a single reason string for logging/rejection.
std::string aggregate_failures(const std::vector<RiskCheck>& checks);

} // namespace vp::risk
