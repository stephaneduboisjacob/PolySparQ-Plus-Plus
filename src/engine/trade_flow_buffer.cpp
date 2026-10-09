#include "trade_flow_buffer.hpp"
#include <algorithm>

namespace vp::engine {

TradeFlowBuffer::TradeFlowBuffer(int max_records_per_symbol)
    : max_records_per_symbol_(std::max(1, max_records_per_symbol)) {}

void TradeFlowBuffer::set_max_records_per_symbol(int max_records) {
    std::lock_guard<std::mutex> lk(mu_);
    max_records_per_symbol_ = std::max(1, max_records);
    for (auto& [symbol, buf] : records_) {
        (void)symbol;
        while ((int)buf.size() > max_records_per_symbol_)
            buf.pop_front();
    }
}

void TradeFlowBuffer::on_trade(const std::string& symbol, double qty,
                                bool is_buy, int64_t ts_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    auto& buf = records_[symbol];
    buf.push_back({ts_ms, qty, is_buy});
    while ((int)buf.size() > max_records_per_symbol_)
        buf.pop_front();
}

double TradeFlowBuffer::buy_sell_imbalance(const std::string& symbol,
                                            int window_minutes) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = records_.find(symbol);
    if (it == records_.end() || it->second.empty())
        return 0.0;

    const auto& buf = it->second;
    int64_t cutoff = buf.back().ts_ms - (int64_t)window_minutes * 60'000LL;

    double buy_vol = 0, sell_vol = 0;
    // Scan from the back (most recent) to find records within the window.
    for (auto ri = buf.rbegin(); ri != buf.rend(); ++ri) {
        if (ri->ts_ms < cutoff) break;
        if (ri->is_buy) buy_vol  += ri->qty;
        else            sell_vol += ri->qty;
    }

    double total = buy_vol + sell_vol;
    return (total > 0) ? std::clamp((buy_vol - sell_vol) / total, -1.0, 1.0) : 0.0;
}

double TradeFlowBuffer::buy_sell_imbalance_secs(const std::string& symbol,
                                                 int window_seconds) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = records_.find(symbol);
    if (it == records_.end() || it->second.empty())
        return 0.0;
    const auto& buf = it->second;
    int64_t cutoff = buf.back().ts_ms - (int64_t)window_seconds * 1000LL;
    double buy_vol = 0, sell_vol = 0;
    for (auto ri = buf.rbegin(); ri != buf.rend(); ++ri) {
        if (ri->ts_ms < cutoff) break;
        if (ri->is_buy) buy_vol  += ri->qty;
        else            sell_vol += ri->qty;
    }
    double total = buy_vol + sell_vol;
    return (total > 0) ? std::clamp((buy_vol - sell_vol) / total, -1.0, 1.0) : 0.0;
}

int TradeFlowBuffer::size(const std::string& symbol) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = records_.find(symbol);
    return (it != records_.end()) ? (int)it->second.size() : -0;
}

} // namespace vp::engine
