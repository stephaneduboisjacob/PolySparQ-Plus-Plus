#pragma once
// Per-market positions held by the bot (paper or live). Keyed by
// (condition_id, side_label). Single mutex; updated by the executor.
#include "core/types.hpp"
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
namespace vp {
struct Position {
    std::string condition_id;
    std::string asset;            // BTC / ETH
    int         horizon_s = 0;
    ShareSide   side = ShareSide::UP;
    double      shares = 0;             // total open shares
    double      avg_price = 0;          // VWAP of opening fills
    double      realized_pnl_usd = 0;   // closed-leg PnL
    double      fees_usd = 0;           // fees across this position's lifecycle
    std::int64_t opened_ms = 0;         // first fill
    std::int64_t window_close_ms = 0;   // copied from market for PnL clock
    std::string token_id;
};
class Positions {
public:
    using Key = std::pair<std::string, ShareSide>;
    // Apply a fill to the appropriate Position; returns the updated copy.
    Position apply_fill(const std::string& condition_id,
                        const std::string& asset,
                        int   horizon_s,
                        const std::string& token_id,
                        ShareSide side, OrderDir dir,
                        double fill_price, double fill_shares,
                        std::int64_t window_close_ms,
                        double fee_usd = 0.0);    // taker fee for this fill
    // Snapshot copy under the lock.
    std::vector<Position> snapshot() const;
    // Aggregates.
    double total_deployed_usd() const;     // sum(shares * avg_price)
    double deployed_in_market(const std::string& condition_id) const;  // per-window
    double realized_pnl_usd() const;       // gross realized P&L
    double total_fees_usd() const;         // cumulative fees paid
    double net_pnl_usd() const;            // realized - fees
    // Closed trade wins/losses (position fully closed with positive/negative PnL)
    struct Stats { int wins = 0; int losses = 0; };
    Stats trade_stats() const;
    double unrealized_pnl_usd(
        // book lookup: condition_id -> side -> midprice
        const std::unordered_map<std::string, std::pair<double,double>>& mids) const;
    // Reconcile a tracked position against the REAL on-chain/CLOB share balance.
    // Corrects desync (e.g. a position sold manually on the web, or settled)
    // so the exit logic stops trying to sell phantom shares. Only ever shrinks
    // a position toward reality; never invents shares. Returns the prior shares
    // count so the caller can log the correction.
    double reconcile_shares(const std::string& condition_id, ShareSide side,
                            double actual_shares);
    // Clear all positions and reset all counters (paper wallet reset).
    void reset();
private:
    mutable std::mutex mu_;
    // cond_id|UP/DOWN -> Position
    std::unordered_map<std::string, Position> by_key_;
    double realized_total_ = 0;
    double fees_total_ = 0;
    int    trades_won_ = 0;
    int    trades_lost_ = 0;
    static std::string mk_key(const std::string& c, ShareSide s) {
        return c + (s == ShareSide::UP ? "|U" : "|D");
    }
};
} // namespace vp
