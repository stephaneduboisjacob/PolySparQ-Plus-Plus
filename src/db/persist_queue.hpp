#pragma once
// Bounded MPSC persistence queue.
// Multiple feed callbacks push items (non-blocking, drops on overflow).
// One writer thread pops items and calls the corresponding Pg method.
// Thread-safe.
#include <array>
#include <cstddef>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <string_view>

namespace vp {

enum class PersistSource : std::size_t {
    Unclassified,
    MarketMetadata,
    WindowEvent,
    PolymarketBookSnapshot,
    PolymarketPublicTrade,
    OracleTick,
    BinanceTick,
    CoinbaseTick,
    FeatureSnapshot,
    ImpulseShadow,
    Signal,
    PositionSnapshot,
    StrategyRun,
    Count,
};

inline constexpr std::size_t kPersistSourceCount =
    static_cast<std::size_t>(PersistSource::Count);

constexpr std::string_view persist_source_name(PersistSource source) noexcept {
    switch (source) {
    case PersistSource::MarketMetadata: return "market_metadata";
    case PersistSource::WindowEvent: return "window_event";
    case PersistSource::PolymarketBookSnapshot: return "pm_book_snapshot";
    case PersistSource::PolymarketPublicTrade: return "pm_public_trade";
    case PersistSource::OracleTick: return "oracle_tick";
    case PersistSource::BinanceTick: return "binance_tick";
    case PersistSource::CoinbaseTick: return "coinbase_tick";
    case PersistSource::FeatureSnapshot: return "feature_snapshot";
    case PersistSource::ImpulseShadow: return "impulse_shadow";
    case PersistSource::Signal: return "signal";
    case PersistSource::PositionSnapshot: return "position_snapshot";
    case PersistSource::StrategyRun: return "strategy_run";
    case PersistSource::Unclassified:
    case PersistSource::Count:
        return "unclassified";
    }
    return "unclassified";
}

template<typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(size_t max = 2000) : max_(max) {}

    // Push item; returns false and discards if full.
    bool push(T item, PersistSource source = PersistSource::Unclassified) {
        std::lock_guard<std::mutex> lk(mu_);
        if (shutdown_ || q_.size() >= max_) {
            ++rejected_pushes_;
            const auto source_index = static_cast<std::size_t>(source);
            ++rejected_by_source_[source_index < rejected_by_source_.size()
                                      ? source_index
                                      : static_cast<std::size_t>(PersistSource::Unclassified)];
            return false;
        }
        q_.push(std::move(item));
        cv_.notify_one();
        return true;
    }

    // Accounting events cannot use the lossy market-data enqueue policy.
    // Backpressure only when the writer falls behind; preserve FIFO ordering.
    bool push_wait(T item) {
        std::unique_lock<std::mutex> lk(mu_);
        space_cv_.wait(lk,[this]{return shutdown_ || q_.size()<max_;});
        if (shutdown_) return false;
        q_.push(std::move(item));
        cv_.notify_one();
        return true;
    }

    // Pop one item; blocks up to timeout_ms. Returns false on timeout/shutdown.
    bool pop(T& out, int timeout_ms = 500) {
        std::unique_lock<std::mutex> lk(mu_);
        if (!cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                          [this]{ return !q_.empty() || shutdown_; }))
            return false;
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop();
        space_cv_.notify_one();
        return true;
    }

    void shutdown() {
        std::lock_guard<std::mutex> lk(mu_);
        shutdown_ = true;
        cv_.notify_all();
        space_cv_.notify_all();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lk(mu_);
        return q_.size();
    }

    // Number of non-blocking writes rejected because the queue was full or
    // shut down. This exposes market-data loss without making feed callbacks
    // block behind the database writer.
    size_t rejected_pushes() const {
        std::lock_guard<std::mutex> lk(mu_);
        return rejected_pushes_;
    }

    std::array<size_t, kPersistSourceCount> rejected_by_source() const {
        std::lock_guard<std::mutex> lk(mu_);
        return rejected_by_source_;
    }

    bool is_shutdown() const {
        std::lock_guard<std::mutex> lk(mu_);
        return shutdown_;
    }

private:
    mutable std::mutex      mu_;
    std::condition_variable cv_;
    std::condition_variable space_cv_;
    std::queue<T>           q_;
    size_t                  max_;
    size_t                  rejected_pushes_ = 0;
    std::array<size_t, kPersistSourceCount> rejected_by_source_{};
    bool                    shutdown_ = false;
};

} // namespace vp
