#pragma once
// CandleStore — in-memory rolling OHLCV bars per asset.
//
// Bootstrapped from the 5-year history in vp.candles_1m at startup, then
// updated in real-time from Binance ticks. Provides slice() to get the last
// N closes/bars for indicator calculations.
//
// Threading: writers must hold the per-asset mutex (mu_); readers may take
// a const snapshot via copy_recent_closes() / copy_recent_bars() which is
// internally synchronised. Designed for single-writer-many-reader access:
// the Binance feed thread is the only writer.
#include "indicators.hpp"   // for Candle
#include <atomic>
#include <deque>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace vp { class Pg; }

namespace vp::engine {

// Supported timeframes (in minutes)
enum class Timeframe { OneMinute = 1, FiveMinutes = 5, FifteenMinutes = 15, SixtyMinutes = 60 };

inline std::string asset_to_symbol(const std::string& asset) {
    if (asset == "BTC") return "BTCUSDT";
    if (asset == "ETH") return "ETHUSDT";
    if (asset == "SOL") return "SOLUSDT";
    if (asset == "XRP") return "XRPUSDT";
    return asset;
}

inline std::string symbol_to_asset(const std::string& symbol) {
    if (symbol == "BTCUSDT") return "BTC";
    if (symbol == "ETHUSDT") return "ETH";
    if (symbol == "SOLUSDT") return "SOL";
    if (symbol == "XRPUSDT") return "XRP";
    return symbol;
}

class CandleStore {
public:
    // Live working set: seven days at minute resolution and one year hourly.
    // The full five-year archive stays in PostgreSQL for offline research.
    static constexpr int kDefaultMaxBarsPerAssetPerTimeframe = 10081;

    explicit CandleStore(int max_bars_per_asset_timeframe =
                         kDefaultMaxBarsPerAssetPerTimeframe);
    void set_max_bars_per_asset_timeframe(int max_bars);

    // Load historical candles for all supported timeframes from Postgres.
    // Symbol mapping: BTC → BTCUSDT, ETH → ETHUSDT, SOL → SOLUSDT, XRP → XRPUSDT.
    // Loads bounded 1m data and server-aggregated hourly context.
    void bootstrap_from_pg(Pg& pg);

    // Repair startup/reconnect gaps using the shared archive's complete bars.
    // Queries run outside the feed mutex; live in-progress bars are preserved.
    void refresh_recent_from_pg(Pg& pg, int64_t now_ms);
    void merge_closed_candles(const std::string& asset, std::span<const Candle> bars,
                              int64_t now_ms);
    uint64_t closed_revision() const { return closed_revision_.load(std::memory_order_relaxed); }

    // True once bootstrap_from_pg has loaded history. Lets the hot-reload path
    // lazily bootstrap if the predictor is enabled after a memory-saving skip.
    bool is_bootstrapped() const { return bootstrapped_.load(std::memory_order_relaxed); }

    // Append a real-time trade tick to the current 1-min bar. Rolls a new
    // bar when the minute boundary is crossed. ts_ms is the trade time.
    // buyer_is_maker=true => buyer was maker (seller aggressor = sell volume).
    void on_tick(const std::string& asset, double price, double qty,
                 bool buyer_is_maker, int64_t ts_ms);

    // Copy the last N close prices for a specific timeframe.
    std::vector<double> copy_recent_closes(const std::string& asset, Timeframe tf, int n) const;

    // Copy the last N OHLCV bars for a specific timeframe.
    std::vector<Candle> copy_recent_bars(const std::string& asset, Timeframe tf, int n) const;

    // Quick read accessors (under lock).
    int  size(const std::string& asset, Timeframe tf) const;
    bool has_asset(const std::string& asset) const;

private:
    struct TimeframeState {
        std::deque<Candle> bars;      // completed candles
        Candle             current{};  // bar in progress
        int64_t            cur_ts_ms = 0;  // timestamp of current bar (aligned to timeframe)
        int                completed_minutes = 0;
        bool               current_complete = true;
        int64_t            bar_duration_ms = 0;  // duration in milliseconds
    };

    struct AssetState {
        int64_t last_tick_ms = 0;
        std::unordered_map<Timeframe, TimeframeState> tf_states;
    };

    mutable std::mutex                          mu_;
    std::unordered_map<std::string, AssetState> assets_;
    int max_bars_per_asset_timeframe_ = kDefaultMaxBarsPerAssetPerTimeframe;
    std::atomic<bool> bootstrapped_{false};
    std::atomic<uint64_t> closed_revision_{0};

    int retention(Timeframe tf) const;

    // Find asset state (mutable)
    AssetState* find_locked(const std::string& asset);
    const AssetState* find_locked(const std::string& asset) const;

    // Get timeframe state (mutable)
    TimeframeState& get_tf_state_locked(AssetState& asset_state, Timeframe tf);
    // Get timeframe state (const version)
    const TimeframeState& get_tf_state_locked(const AssetState& asset_state, Timeframe tf) const;
    // Get or create timeframe state
    TimeframeState& ensure_tf_state_locked(AssetState& asset_state, Timeframe tf);

    // Propagate a closed 1-minute candle to higher timeframes
    void propagate_to_higher_timeframes(AssetState& asset_state, const Candle& candle_1m);
};

} // namespace vp::engine
