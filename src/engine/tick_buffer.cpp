#include "tick_buffer.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace vp::engine {

TickBuffer::TickBuffer(int64_t max_age_us, int max_entries) {
    set_limits(max_age_us, max_entries);
}

void TickBuffer::set_limits(int64_t max_age_us, int max_entries) {
    std::lock_guard<std::mutex> lk(mu_);
    max_age_us_ = std::max<int64_t>(1, max_age_us);
    max_entries_ = std::max(1, max_entries);
}

void TickBuffer::on_price(const std::string& cond_id, const std::string& side,
                          double price, int64_t ts_us) {
    if (price <= 0) return;
    std::lock_guard<std::mutex> lk(mu_);
    auto& buf = conds_[cond_id];
    auto& ticks = (side == "UP") ? buf.up.ticks : buf.down.ticks;
    ticks.push_back({ts_us, price});
    // Prune old entries
    prune_locked(ticks, ts_us);
    while ((int)ticks.size() > max_entries_) ticks.pop_front();
}

void TickBuffer::prune_locked(std::deque<TickRecord>& ticks, int64_t now_us) const {
    while (!ticks.empty() && ticks.front().ts_us < now_us - max_age_us_)
        ticks.pop_front();
}

double TickBuffer::velocity_us(const std::string& cond_id, const std::string& side,
                               int64_t window_us) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = conds_.find(cond_id);
    if (it == conds_.end()) return 0.0;
    const auto& ticks = (side == "UP") ? it->second.up.ticks : it->second.down.ticks;
    if (ticks.size() < 2) return 0.0;

    int64_t cutoff = ticks.back().ts_us - window_us;
    // Find the first tick within the window
    auto start = ticks.begin();
    while (start != ticks.end() && start->ts_us < cutoff) ++start;
    if (start == ticks.end() || ticks.back().ts_us - start->ts_us < window_us / 2)
        return 0.0;

    // Linear regression: slope = (n*sum(xy) - sum(x)*sum(y)) / (n*sum(xx) - sum(x)^2)
    // Use time offsets relative to the first tick's ts_us to avoid large-number precision loss.
    int64_t t0 = start->ts_us;
    int n = 0;
    double sum_x = 0, sum_y = 0, sum_xx = 0, sum_xy = 0;
    for (auto it = start; it != ticks.end(); ++it) {
        double dx = (it->ts_us - t0) / 1'000'000.0;  // seconds
        double dy = it->price;
        ++n;
        sum_x  += dx;
        sum_y  += dy;
        sum_xx += dx * dx;
        sum_xy += dx * dy;
    }
    if (n < 2) return 0.0;
    double denom = n * sum_xx - sum_x * sum_x;
    if (std::abs(denom) < 1e-12) return 0.0;
    return (n * sum_xy - sum_x * sum_y) / denom;  // $/s
}

double TickBuffer::acceleration_us(const std::string& cond_id, const std::string& side,
                                   int64_t window_us) const {
    // Acceleration = difference between short-window and long-window velocity.
    double v_short = velocity_us(cond_id, side, window_us / 2);
    double v_long  = velocity_us(cond_id, side, window_us);
    return v_short - v_long;  // positive = accelerating
}

double TickBuffer::peak_since(const std::string& cond_id, const std::string& side,
                              int64_t since_us) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = conds_.find(cond_id);
    if (it == conds_.end()) return 0.0;
    const auto& ticks = (side == "UP") ? it->second.up.ticks : it->second.down.ticks;
    if (ticks.empty()) return 0.0;

    double peak = (side == "UP") ? -1e9 : 1e9;
    for (const auto& t : ticks) {
        if (t.ts_us < since_us) continue;
        if (side == "UP") {
            if (t.price > peak) peak = t.price;
        } else {
            if (t.price < peak) peak = t.price;
        }
    }
    return (side == "UP") ? peak : (peak == 1e9 ? 0.0 : peak);
}

bool TickBuffer::is_monotonic(const std::string& cond_id, const std::string& side,
                              bool direction_up, int64_t window_us) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = conds_.find(cond_id);
    if (it == conds_.end()) return false;
    const auto& ticks = (side == "UP") ? it->second.up.ticks : it->second.down.ticks;
    if (ticks.size() < 3) return false;

    int64_t cutoff = ticks.back().ts_us - window_us;
    auto start = ticks.begin();
    while (start != ticks.end() && start->ts_us < cutoff) ++start;
    if (start == ticks.end()) return false;

    auto prev = start;
    for (auto cur = start + 1; cur != ticks.end(); ++cur) {
        if (direction_up && cur->price < prev->price) return false;
        if (!direction_up && cur->price > prev->price) return false;
        prev = cur;
    }
    return true;
}

int TickBuffer::total_ticks() const {
    std::lock_guard<std::mutex> lk(mu_);
    int count = 0;
    for (const auto& [cid, cb] : conds_) {
        count += (int)cb.up.ticks.size() + (int)cb.down.ticks.size();
    }
    return count;
}

} // namespace vp::engine
