#include "exec/positions.hpp"
#include "core/clock.hpp"
#include <algorithm>
namespace vp {
Position Positions::apply_fill(const std::string& cond,
                               const std::string& asset,
                               int horizon_s,
                               const std::string& token_id,
                               ShareSide side, OrderDir dir,
                               double price, double shares,
                               std::int64_t window_close_ms,
                               double fee_usd) {
    std::lock_guard<std::mutex> lk(mu_);
    auto k = mk_key(cond, side);
    auto& p = by_key_[k];
    if (p.condition_id.empty()) {
        p.condition_id   = cond;
        p.asset          = asset;
        p.horizon_s      = horizon_s;
        p.side           = side;
        p.token_id       = token_id;
        p.window_close_ms = window_close_ms;
        p.opened_ms      = Clock::wall_ns() / 1'000'000;
    }
    p.fees_usd += fee_usd;
    if (dir == OrderDir::Buy) {
        // VWAP update
        double new_total = p.shares + shares;
        if (new_total > 0)
            p.avg_price = (p.avg_price * p.shares + price * shares) / new_total;
        p.shares = new_total;
    } else { // Sell — realize PnL on the closed shares
        double closed = std::min(shares, p.shares);
        double pnl = (price - p.avg_price) * closed;
        p.realized_pnl_usd += pnl;
        realized_total_    += pnl;
        fees_total_        += fee_usd;
        p.shares -= closed;
        if (p.shares <= 0.01) {
            p.shares = 0;
            if (p.realized_pnl_usd - p.fees_usd > 1e-9) ++trades_won_;
            else if (p.realized_pnl_usd - p.fees_usd < -1e-9) ++trades_lost_;
            Position ret = p;   // copy before erase
            by_key_.erase(k);   // remove closed position — prevents phantom open_pos
            return ret;
        }
        return p;
    }
    fees_total_ += fee_usd;
    return p;
}
std::vector<Position> Positions::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Position> out;
    out.reserve(by_key_.size());
    for (auto& [_, p] : by_key_) if (p.shares > 0.01)
        out.push_back(p);
    return out;
}
double Positions::total_deployed_usd() const {
    std::lock_guard<std::mutex> lk(mu_);
    double s = 0;
    for (auto& [_, p] : by_key_) s += p.shares * p.avg_price;
    return s;
}
double Positions::deployed_in_market(const std::string& cond) const {
    std::lock_guard<std::mutex> lk(mu_);
    double s = 0;
    for (auto& [_, p] : by_key_)
        if (p.condition_id == cond) s += p.shares * p.avg_price;
    return s;
}
double Positions::realized_pnl_usd() const {
    std::lock_guard<std::mutex> lk(mu_);
    return realized_total_;
}
double Positions::total_fees_usd() const {
    std::lock_guard<std::mutex> lk(mu_);
    return fees_total_;
}
double Positions::net_pnl_usd() const {
    std::lock_guard<std::mutex> lk(mu_);
    return realized_total_ - fees_total_;
}
Positions::Stats Positions::trade_stats() const {
    std::lock_guard<std::mutex> lk(mu_);
    return { trades_won_, trades_lost_ };
}
double Positions::unrealized_pnl_usd(
    const std::unordered_map<std::string, std::pair<double,double>>& mids) const {
    std::lock_guard<std::mutex> lk(mu_);
    double u = 0;
    for (auto& [_, p] : by_key_) {
        if (p.shares <= 0) continue;
        auto it = mids.find(p.condition_id);
        if (it == mids.end()) continue;
        double mid = (p.side == ShareSide::UP) ? it->second.first : it->second.second;
        if (mid > 0) u += (mid - p.avg_price) * p.shares;
    }
    return u;
}
double Positions::reconcile_shares(const std::string& cond, ShareSide side,
                                   double actual_shares) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = by_key_.find(mk_key(cond, side));
    if (it == by_key_.end()) return 0.0;
    auto& p = it->second;
    double prior = p.shares;
    if (actual_shares < 0.01) {
        // Gone on-chain (sold elsewhere / settled). Erase entirely — no phantom.
        by_key_.erase(it);
        return prior;
    } else if (actual_shares < p.shares - 1e-6) {
        // We hold fewer than the tracker believed — trust the chain.
        p.shares = actual_shares;
    }
    return prior;
}
void Positions::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    by_key_.clear();
    realized_total_ = 0;
    fees_total_     = 0;
    trades_won_     = 0;
    trades_lost_    = 0;
}
} // namespace vp
