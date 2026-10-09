#pragma once
// Rule engine — evaluates market conditions and emits optional buy/sell signals.
//
// Called under the SharedState mutex; no DB calls.
//
// Strategy parameters come from StrategyHot (hot-reloadable from config).
// Risk cap enforcement is NOT done here — caps.hpp constants are checked
// again in the executor before any order is placed.
#include "core/types.hpp"
#include "config/config.hpp"
#include <optional>
#include <string>

namespace vp {

// Forward-declared to avoid circular headers
struct WatchlistRow;

struct SignalOut {
    ShareSide   side;
    OrderDir    dir = OrderDir::Buy;   // currently always Buy
    double      confidence = 0;        // 0..1
    double      rec_limit_price = 0;  // recommended limit (best_ask at signal time)
    double      rec_size_usd = 0;     // recommended size (per_click_size_usd from cfg)
    std::string reason;                // human-readable
    char        glyph = '-';          // '+' buy-up / 'v' buy-down / 'X' avoid / '-' wait
    double      trigger_delta_pct = 0; // actual oracle/CB move used by an ORACLE signal
};

namespace rules {

struct EvaluationDiagnostics {
    const char* code = "no_strategy";
    std::string detail;
};

// Keep downstream phase gates on the same source/threshold as the signal.
inline double oracle_trigger_threshold(const StrategyHot& cfg, bool coinbase_led) {
    return coinbase_led && cfg.coinbase_lead_min_delta_pct > 0
        ? cfg.coinbase_lead_min_delta_pct : cfg.min_oracle_delta_pct;
}

// Evaluate one market row against the active strategy config.
// Returns std::nullopt when no actionable signal ("WAIT" / "AVOID" are
// encoded in the returned glyph on the watchlist row itself).
std::optional<SignalOut> evaluate(const WatchlistRow& row,
                                  const StrategyHot&  cfg,
                                  int64_t             now_ms,
                                  EvaluationDiagnostics* diagnostics = nullptr);

} // namespace rules
} // namespace vp
