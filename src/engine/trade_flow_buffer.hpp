#pragma once
// TradeFlowBuffer — rolling ring buffer of Binance trade flow direction.
//
// Tracks buy vs sell aggressor volume from individual Binance trades,
// computing a normalised buy/sell imbalance for the predictor.
// Thread-safe: all public methods hold an internal mutex.
//
// The caller (main.cpp Binance callback) feeds every trade via on_trade().
// The predictor query method (buy_sell_imbalance) reads from any window.

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace vp::engine {

class TradeFlowBuffer {
public:
    static constexpr int kDefaultMaxRecordsPerSymbol = 50000;

    explicit TradeFlowBuffer(int max_records_per_symbol =
                             kDefaultMaxRecordsPerSymbol);
    void set_max_records_per_symbol(int max_records);

    // Feed a trade tick. is_buy = true means the aggressor was a buyer.
    void on_trade(const std::string& symbol, double qty, bool is_buy, int64_t ts_ms);

    // Buy/sell volume imbalance over the last window_minutes.
    // Returns -1.0 (all sell) to +1.0 (all buy). 0.0 = balanced or no data.
    double buy_sell_imbalance(const std::string& symbol, int window_minutes = 5) const;

    // Buy/sell imbalance over the last window_seconds (sub-minute resolution).
    // Convenience wrapper around the same buffer for shorter lookbacks.
    double buy_sell_imbalance_secs(const std::string& symbol, int window_seconds = 10) const;

    // Number of buffered trade events for this symbol.
    int size(const std::string& symbol) const;

private:
    struct TradeRecord {
        int64_t ts_ms;
        double  qty;
        bool    is_buy;
    };
    mutable std::mutex mu_;
    // Keyed by symbol (e.g. "BTCUSDT", "ETHUSDT").
    std::unordered_map<std::string, std::deque<TradeRecord>> records_;
    int max_records_per_symbol_ = kDefaultMaxRecordsPerSymbol;
};

} // namespace vp::engine
