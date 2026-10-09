#pragma once
// TickBuffer — microsecond-precision price tick buffer for sub-second
// momentum detection.
//
// Stores Polymarket order-book mid-prices keyed by (condition_id, side).
// Provides linear-regression velocity, acceleration, peak tracking, and
// monotonicity checks — all at microsecond resolution (orders of magnitude
// faster than the 10s window used by the TUI).
//
// Thread-safe: all public methods hold an internal mutex.
//
// The caller feeds ticks from the Polymarket MD WebSocket BookUpdate
// callback AND from the TUI refresh loop (rebuild_snapshot), ensuring
// the buffer stays fresh even between WebSocket updates.

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vp::engine {

class TickBuffer {
public:
    TickBuffer() = default;
    TickBuffer(int64_t max_age_us, int max_entries);

    void set_limits(int64_t max_age_us, int max_entries);

    // Feed a Polymarket book mid-price tick.
    // cond_id = Polymarket condition_id, side = UP or DOWN share side.
    void on_price(const std::string& cond_id, const std::string& side,
                  double price, int64_t ts_us);

    // ── Query methods ──────────────────────────────────────────────

    // Velocity over the last window_us microseconds ($/s).
    // Uses linear regression slope across all ticks in the window to reject
    // noise spikes from individual ticks. Returns 0.0 if insufficient data.
    double velocity_us(const std::string& cond_id, const std::string& side,
                       int64_t window_us = 1'000'000) const;

    // Acceleration: change in velocity over the last window_us ($/s/s).
    // Computed as velocity(window_us/2) - velocity(window_us) normalised.
    double acceleration_us(const std::string& cond_id, const std::string& side,
                           int64_t window_us = 2'000'000) const;

    // Highest price seen for this side since since_us (for UP-side trailing stop).
    // For DOWN side, returns the lowest price (since falling price is "winning").
    double peak_since(const std::string& cond_id, const std::string& side,
                      int64_t since_us) const;

    // True when every tick in the window ticks in direction_up without reversal.
    // Used to detect momentum exhaustion before explicit velocity reversal.
    bool is_monotonic(const std::string& cond_id, const std::string& side,
                      bool direction_up, int64_t window_us = 2'000'000) const;

    // Total ticks stored for diagnostic logging.
    int total_ticks() const;

private:
    struct TickRecord {
        int64_t ts_us;
        double  price;
    };

    struct SideBuffer {
        std::deque<TickRecord> ticks;
    };

    struct CondBuffer {
        SideBuffer up;
        SideBuffer down;
    };

    // Prune entries older than max_age_us_ from the given deque.
    void prune_locked(std::deque<TickRecord>& ticks, int64_t now_us) const;

    mutable std::mutex mu_;
    std::unordered_map<std::string, CondBuffer> conds_;
    int64_t max_age_us_ = 30 * 1'000'000LL;
    int max_entries_ = 5000;
};

} // namespace vp::engine
