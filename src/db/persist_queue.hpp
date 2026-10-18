#pragma once
// Bounded MPSC persistence queue.
// Multiple feed callbacks push items (non-blocking, drops on overflow).
// One writer thread pops items and calls the corresponding Pg method.
// Thread-safe.
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>

namespace vp {

template<typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(size_t max = 2000) : max_(max) {}

    // Push item; returns false and discards if full.
    bool push(T item) {
        std::lock_guard<std::mutex> lk(mu_);
        if (shutdown_ || q_.size() >= max_) return false;
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
    bool                    shutdown_ = false;
};

} // namespace vp
