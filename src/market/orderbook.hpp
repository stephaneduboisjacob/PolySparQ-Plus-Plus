#pragma once
// Polymarket order book — flat arrays for cache-friendly top-N rendering.
// Shallow books, frequent updates: arrays beat trees.
#include "core/types.hpp"
#include <vector>
#include <cstdint>
#include <atomic>
#include <shared_mutex>

namespace vp {

class OrderBook {
public:
    void apply_snapshot(std::vector<PriceLevel> bids,
                        std::vector<PriceLevel> asks) noexcept;
    void apply_delta_bid(double price, double size) noexcept;
    void apply_delta_ask(double price, double size) noexcept;

    PriceLevel best_bid() const noexcept;
    PriceLevel best_ask() const noexcept;
    double     midprice() const noexcept;
    double     microprice() const noexcept;
    double     imbalance_top1() const noexcept;
    double     imbalance_topN(int n) const noexcept;
    double     spread() const noexcept;

    // Thread-safe read accessors — copy the vectors under shared lock.
    std::vector<PriceLevel> copy_bids() const noexcept;
    std::vector<PriceLevel> copy_asks() const noexcept;

    std::uint64_t generation() const noexcept { return gen_.load(std::memory_order_acquire); }

private:
    std::vector<PriceLevel> bids_;     // descending
    std::vector<PriceLevel> asks_;     // ascending
    mutable std::shared_mutex mu_;
    std::atomic<std::uint64_t> gen_{0};
};

} // namespace vp

