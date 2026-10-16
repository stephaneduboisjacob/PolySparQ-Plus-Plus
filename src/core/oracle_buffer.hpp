#pragma once
// OracleBuffer — thread-safe, time-indexed ring buffer of oracle prices per asset.
//
// Stores (unix_timestamp_s, price) pairs for each asset in a sorted deque.
// Supports exact-time lookups via binary search (lookup_at_or_before).
//
// Used by the oracle-lag strategy to compute:
//   oracle_delta_pct = (oracle_now - oracle_at_window_open) / oracle_at_window_open * 100
//
// Threading: all methods are thread-safe (per-instance mutex).
//   Lock ordering: state.mu → oracle_buffer.mu_ (never reversed).
//   Callers holding state.mu may safely call any method here.

#include <algorithm>
#include <cmath>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vp::core {

class OracleBuffer {
public:
    // max_depth: maximum number of (ts, price) samples to keep per asset.
    // At ~1 update/second from RTDS, 7200 = 2 hours of history.
    explicit OracleBuffer(std::size_t max_depth = 7200)
        : max_depth_(max_depth) {}

    // Add a price observation. If timestamp is less than the newest stored
    // timestamp for this asset the call is silently ignored (out-of-order).
    void add(const std::string& asset, double timestamp_s, double price) {
        std::lock_guard lk(mu_);
        if (!std::isfinite(price) || price <= 0 || !std::isfinite(timestamp_s)) return;
        auto& buf = data_[asset];
        if (!buf.empty() && timestamp_s < buf.back().first)
            return;
        buf.emplace_back(timestamp_s, price);
        while (buf.size() > max_depth_)
            buf.pop_front();
    }

    // Insert a timestamped observation recovered from durable history. Unlike
    // add(), this permits an older sample to be inserted into an already-live
    // stream, while preserving the timestamp order required by lookups.
    void add_historical(const std::string& asset, double timestamp_s, double price) {
        std::lock_guard lk(mu_);
        if (!std::isfinite(price) || price <= 0 || !std::isfinite(timestamp_s)) return;
        auto& buf = data_[asset];
        auto it = std::lower_bound(buf.begin(), buf.end(), timestamp_s,
            [](const std::pair<double, double>& sample, double ts) {
                return sample.first < ts;
            });
        if (it != buf.end() && it->first == timestamp_s) return;
        buf.insert(it, {timestamp_s, price});
        while (buf.size() > max_depth_)
            buf.pop_front();
    }

    // Return most recent price for asset, or 0.0 if no data.
    double latest(const std::string& asset) const {
        std::lock_guard lk(mu_);
        auto it = data_.find(asset);
        if (it == data_.end() || it->second.empty()) return 0.0;
        return it->second.back().second;
    }

    double latest_timestamp(const std::string& asset) const {
        std::lock_guard lk(mu_);
        auto it = data_.find(asset);
        return it == data_.end() || it->second.empty() ? 0.0 : it->second.back().first;
    }

    // Return the oracle price at or just before target_ts_s.
    // Returns 0.0 if the buffer has no data preceding target_ts_s.
    // max_age_s: optional freshness gate relative to target_ts_s. The default
    // is UNCAPPED — these are historical reads (window-open strike, trend
    // lookback, settlement), where the tick at-or-before the target is exactly
    // what is wanted regardless of the current wall-clock time. A cap rejects
    // a tick only when it is too far BEFORE the queried timestamp; querying
    // the same window opening later does not make its historical tick expire.
    // Callers that want freshness gating pass it explicitly.
    double lookup_at_or_before(const std::string& asset, double target_ts_s,
                               double max_age_s = -1.0) const {
        std::lock_guard lk(mu_);
        auto it = data_.find(asset);
        if (it == data_.end() || it->second.empty()) return 0.0;
        const auto& buf = it->second;
        // upper_bound: first element with timestamp > target_ts_s
        auto upper = std::upper_bound(buf.begin(), buf.end(), target_ts_s,
            [](double ts, const std::pair<double, double>& e) {
                return ts < e.first;
            });
        if (upper == buf.begin()) return 0.0; // all data is after target_ts_s
        --upper;
        if (max_age_s >= 0.0 && target_ts_s - upper->first > max_age_s)
            return 0.0;
        return upper->second;
    }

    // Return the earliest price for asset, or 0.0 if no data.
    double earliest(const std::string& asset) const {
        std::lock_guard lk(mu_);
        auto it = data_.find(asset);
        if (it == data_.end() || it->second.empty()) return 0.0;
        return it->second.front().second;
    }

    // Time-weighted mean over [start_ts_s, end_ts_s], treating each sample as
    // valid until the next observation. Returns 0 if the buffer does not cover
    // the full interval. Used to condition the final Chainlink TWAP estimate on
    // the portion of its averaging window that has already been observed.
    double time_weighted_average(const std::string& asset,
                                 double start_ts_s, double end_ts_s) const {
        if (!std::isfinite(start_ts_s) || !std::isfinite(end_ts_s) ||
            end_ts_s <= start_ts_s) return 0.0;
        std::lock_guard lk(mu_);
        auto it = data_.find(asset);
        if (it == data_.end() || it->second.size() < 2) return 0.0;
        const auto& buf = it->second;
        auto upper = std::upper_bound(buf.begin(), buf.end(), start_ts_s,
            [](double ts, const std::pair<double, double>& e) {
                return ts < e.first;
            });
        if (upper == buf.begin()) return 0.0;
        auto sample = std::prev(upper);
        if (!(sample->second > 0.0) || sample->first > start_ts_s) return 0.0;
        double cursor = start_ts_s;
        double price = sample->second;
        double area = 0.0;
        for (++sample; sample != buf.end() && sample->first < end_ts_s; ++sample) {
            if (sample->first > cursor)
                area += price * (sample->first - cursor);
            cursor = sample->first;
            price = sample->second;
            if (!(price > 0.0) || !std::isfinite(price)) return 0.0;
        }
        if (end_ts_s > cursor) area += price * (end_ts_s - cursor);
        const double mean = area / (end_ts_s - start_ts_s);
        return std::isfinite(mean) && mean > 0.0 ? mean : 0.0;
    }

    // Return number of samples stored for an asset (useful for diagnostics).
    std::size_t size(const std::string& asset) const {
        std::lock_guard lk(mu_);
        auto it = data_.find(asset);
        if (it == data_.end()) return 0;
        return it->second.size();
    }

    // Rate of change over the last lookback_secs (% per minute).
    // Returns 0.0 if insufficient data (need at least 2 samples spanning
    // at least 1 second).
    double velocity_roc_pct(const std::string& asset, int lookback_secs = 60) const {
        std::lock_guard lk(mu_);
        auto it = data_.find(asset);
        if (it == data_.end() || it->second.size() < 2) return 0.0;
        const auto& buf = it->second;
        double now_price = buf.back().second;
        double now_ts    = buf.back().first;
        double then_ts   = now_ts - lookback_secs;
        auto upper = std::upper_bound(buf.begin(), buf.end(), then_ts,
            [](double ts, const std::pair<double, double>& e) { return ts < e.first; });
        if (upper == buf.begin()) return 0.0;
        --upper;
        double then_price = upper->second;
        double elapsed_min = (now_ts - then_ts) / 60.0;
        if (elapsed_min <= 0 || then_price <= 0) return 0.0;
        return 100.0 * (now_price - then_price) / then_price / elapsed_min;
    }

    // Oracle acceleration = change in velocity over the last lookback_secs.
    // Computed as (current velocity - velocity lookback_secs ago).
    double acceleration_pct(const std::string& asset, int lookback_secs = 60) const {
        double v_now = velocity_roc_pct(asset, lookback_secs / 1);
        double v_then = velocity_roc_pct(asset, lookback_secs * 2);
        return v_now - v_then;
    }

    // ── Per-second realized volatility (Phase 5.1 digital FV) ──
    // Stdev of consecutive tick-to-tick log returns over the lookback
    // window, assuming ~1 tick/s cadence from RTDS. This is the σ in the
    // zero-drift GBM digital fair value. Returns 0.0 with <3 samples.
    double realized_vol_per_sec(const std::string& asset,
                                int lookback_secs = 60) const {
        std::lock_guard lk(mu_);
        auto it = data_.find(asset);
        if (it == data_.end() || it->second.size() < 3) return 0.0;
        const auto& buf = it->second;
        const double cutoff = buf.back().first - lookback_secs;
        std::vector<double> lr;
        double prev = 0.0;
        for (const auto& e : buf) {
            if (e.first < cutoff || e.second <= 0) continue;
            if (prev > 0.0) lr.push_back(std::log(e.second / prev));
            prev = e.second;
        }
        if (lr.size() < 2) return 0.0;
        double mean = 0.0;
        for (double r : lr) mean += r;
        mean /= (double)lr.size();
        double var = 0.0;
        for (double r : lr) { double d = r - mean; var += d * d; }
        var /= (double)(lr.size() - 1);
        return std::sqrt(var);
    }

    // ── Jump-regime z-score (Phase 5.1) ──
    // |z| of the most recent log return vs the trailing 60s distribution.
    // A |z| above ~3 means a jump regime (news / liquidation cascade) where
    // the GBM digital fair value is invalid → entries must be vetoed.
    // Returns 0.0 with insufficient data.
    double last_move_z(const std::string& asset, int lookback_secs = 60) const {
        std::lock_guard lk(mu_);
        auto it = data_.find(asset);
        if (it == data_.end() || it->second.size() < 4) return 0.0;
        const auto& buf = it->second;
        const double cutoff = buf.back().first - lookback_secs;
        std::vector<double> lr;
        double prev = 0.0;
        for (const auto& e : buf) {
            if (e.first < cutoff || e.second <= 0) continue;
            if (prev > 0.0) lr.push_back(std::log(e.second / prev));
            prev = e.second;
        }
        if (lr.size() < 3) return 0.0;
        double mean = 0.0;
        for (double r : lr) mean += r;
        mean /= (double)lr.size();
        double var = 0.0;
        for (double r : lr) { double d = r - mean; var += d * d; }
        var /= (double)(lr.size() - 1);
        if (var <= 0.0) return 0.0;
        return std::fabs(lr.back() - mean) / std::sqrt(var);
    }

private:
    mutable std::mutex mu_;
    std::size_t max_depth_;
    std::unordered_map<std::string, std::deque<std::pair<double, double>>> data_;
};

} // namespace vp::core
