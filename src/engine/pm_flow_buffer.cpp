#include "pm_flow_buffer.hpp"
#include <algorithm>

namespace vp::engine {

PmFlowBuffer::PmFlowBuffer(int max_records_per_market)
    : max_records_per_market_(std::max(1, max_records_per_market)) {}

void PmFlowBuffer::on_trade(const std::string& condition_id, double qty,
                            int sign, int64_t ts_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    auto& buf = records_[condition_id];
    buf.push_back({ts_ms, qty, sign});
    while ((int)buf.size() > max_records_per_market_)
        buf.pop_front();
}

double PmFlowBuffer::imbalance_secs(const std::string& condition_id,
                                    int window_seconds) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = records_.find(condition_id);
    if (it == records_.end() || it->second.empty())
        return 0.0;
    const auto& buf = it->second;
    int64_t cutoff = buf.back().ts_ms - (int64_t)window_seconds * 1000LL;
    double up_vol = 0, down_vol = 0;
    for (auto ri = buf.rbegin(); ri != buf.rend(); ++ri) {
        if (ri->ts_ms < cutoff) break;
        if (ri->sign > 0) up_vol   += ri->qty;
        else              down_vol += ri->qty;
    }
    double total = up_vol + down_vol;
    return (total > 0) ? std::clamp((up_vol - down_vol) / total, -1.0, 1.0) : 0.0;
}

int PmFlowBuffer::size(const std::string& condition_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = records_.find(condition_id);
    return (it != records_.end()) ? (int)it->second.size() : 0;
}

} // namespace vp::engine
