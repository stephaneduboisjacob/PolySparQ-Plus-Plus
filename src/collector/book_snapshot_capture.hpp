#pragma once

#include "feeds/polymarket_md.hpp"
#include "market/orderbook.hpp"

#include <cmath>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vp {

struct CapturedBookSnapshot {
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;
    double best_bid = 0.0;
    double best_ask = 0.0;
};

// The market stream sends one complete `book` seed followed by partial
// `price_change` events. Rebuild that state before producing a persistence
// snapshot; persisting a delta as a book would fabricate a false BBO and empty
// depth. This accumulator is owned and updated by the collector's WS callback
// thread. The engine has a separate OrderBook instance for trading decisions.
class BookSnapshotAccumulator {
public:
    bool apply(const PolymarketMD::BookUpdate& update) {
        if (update.token_id.empty()) return false;

        if (update.is_snapshot) {
            auto bids = valid_snapshot_levels(update.bids);
            auto asks = valid_snapshot_levels(update.asks);
            auto [it, inserted] = books_.try_emplace(update.token_id);
            (void)inserted;
            it->second.apply_snapshot(std::move(bids), std::move(asks));
            seeded_.insert(update.token_id);
            return true;
        }

        if (!seeded_.contains(update.token_id)) return false;
        auto it = books_.find(update.token_id);
        if (it == books_.end()) return false;
        for (const auto& level : update.bids) {
            if (std::isfinite(level.size) && level.size >= 0.0)
                it->second.apply_delta_bid(level.price, level.size);
        }
        for (const auto& level : update.asks) {
            if (std::isfinite(level.size) && level.size >= 0.0)
                it->second.apply_delta_ask(level.price, level.size);
        }
        return true;
    }

    std::optional<CapturedBookSnapshot> snapshot(const std::string& token_id) const {
        if (!seeded_.contains(token_id)) return std::nullopt;
        const auto it = books_.find(token_id);
        if (it == books_.end()) return std::nullopt;

        CapturedBookSnapshot result;
        result.bids = it->second.copy_bids();
        result.asks = it->second.copy_asks();
        if (!result.bids.empty()) result.best_bid = result.bids.front().price;
        if (!result.asks.empty()) result.best_ask = result.asks.front().price;
        return result;
    }

private:
    static std::vector<PriceLevel> valid_snapshot_levels(
            const std::vector<PriceLevel>& levels) {
        std::vector<PriceLevel> valid;
        valid.reserve(levels.size());
        for (const auto& level : levels) {
            if (std::isfinite(level.price) && level.price > 0.0 &&
                level.price <= 1.0 && std::isfinite(level.size) &&
                level.size > 0.0) {
                valid.push_back(level);
            }
        }
        return valid;
    }

    std::unordered_map<std::string, OrderBook> books_;
    std::unordered_set<std::string> seeded_;
};

} // namespace vp
