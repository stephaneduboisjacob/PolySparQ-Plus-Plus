#pragma once
#include "core/oracle_buffer.hpp"
#include "engine/candle_store.hpp"
#include "engine/forecast.hpp"
#include "tui/tui.hpp"
#include <cmath>
#include <unordered_map>

namespace vp::engine {

// Shared inputs for CB, latency and endgame. Estimate unaveraged price risk
// from closed exchange candles, never from the smoothed TWAP tick increments.
// Per-thread minute cache avoids copying history for every market/sweep.
inline void populate_reference_forecasts(vp::WatchlistRow& row, const vp::PmMarket& market,
                                  const vp::core::OracleBuffer& oracle,
                                  const vp::core::OracleBuffer& coinbase,
                                  const vp::engine::CandleStore& candles, int64_t now_ms) {
    struct CachedVol { int64_t ts = 0; uint64_t revision = 0; vp::engine::VolatilityEstimate vol; };
    thread_local std::unordered_map<std::string,CachedVol> cache;
    thread_local const vp::engine::CandleStore* cache_owner = nullptr;
    if (cache_owner != &candles) { cache.clear(); cache_owner = &candles; }
    auto bars = candles.copy_recent_bars(market.asset,vp::engine::Timeframe::OneMinute,1);
    auto& item = cache[market.asset];
    const int64_t ts = bars.empty() ? 0 : bars.back().ts_ms;
    const auto revision = candles.closed_revision();
    if (ts != item.ts || revision != item.revision) {
        item.ts = ts;
        item.revision = revision;
        item.vol = vp::engine::estimate_volatility(candles.copy_recent_closes(
            market.asset,vp::engine::Timeframe::OneMinute,1441));
    }
    const auto key = market.reference_price_key();
    const double spot_ts = oracle.latest_timestamp(key);
    const double spot = oracle.latest(key);
    // Regime gates must measure the unaveraged underlying used by forecast().
    // TWAP increments suppress volatility and falsely trip the latency floor.
    row.oracle_vol_per_sec = item.vol.sigma_minute / std::sqrt(60.0);
    row.oracle_jump_z = oracle.last_move_z(key, 60);
    const double age = std::max(now_ms/1000.0-spot_ts,
        now_ms/1000.0-oracle.latest_timestamp(market.reference_key()));
    vp::engine::ForecastInput in{spot,row.coin_open_price,
        (market.window_close_ms-now_ms)/1000.0,item.vol.sigma_minute,age,
        ts ? (now_ms-ts-60000)/1000.0 : -1,item.vol.samples,market.settlement_window_s};
    row.reference_forecast = vp::engine::forecast(in);
    const double cb_age = now_ms/1000.0-coinbase.latest_timestamp(market.asset);
    const double cb_open = coinbase.lookup_at_or_before(market.asset,market.window_open_ms/1000.0);
    const double cb_aligned = coinbase.lookup_at_or_before(market.asset,spot_ts);
    const double cb_now = coinbase.latest(market.asset);
    row.cb_data_ready = cb_age >= -1 && cb_age <= 3 && cb_open > 0 && cb_now > 0;
    if (!row.cb_data_ready) row.cb_delta_pct = 0;
    // Transport only the Coinbase move SINCE the latest official spot update.
    // Comparing a Coinbase window-open spot with a TWAP strike adds a false basis.
    if (row.cb_data_ready && cb_aligned > 0) {
        in.spot = spot*cb_now/cb_aligned;
        row.fast_forecast = vp::engine::forecast(in);
    }
}

} // namespace vp::engine
