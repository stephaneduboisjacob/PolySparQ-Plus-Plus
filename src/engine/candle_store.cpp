// CandleStore implementation. See header for contracts.
#include "candle_store.hpp"
#include "db/pg.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <stdexcept>
#include <chrono>
#include <cmath>

namespace vp::engine {

namespace {

inline int64_t floor_to_minute_ms(int64_t ts_ms) {
    return (ts_ms / 60000LL) * 60000LL;
}

inline int64_t floor_to_timeframe_ms(int64_t ts_ms, int64_t timeframe_minutes) {
    return (ts_ms / (timeframe_minutes * 60000LL)) * (timeframe_minutes * 60000LL);
}

} // namespace

// Timeframe configurations
constexpr std::array<Timeframe, 4> kAllTimeframes = {
    Timeframe::OneMinute, Timeframe::FiveMinutes, Timeframe::FifteenMinutes, Timeframe::SixtyMinutes
};

CandleStore::CandleStore(int max_bars_per_asset_timeframe)
    : max_bars_per_asset_timeframe_(std::max(1, max_bars_per_asset_timeframe)) {}

int CandleStore::retention(Timeframe tf) const {
    const int cap = tf == Timeframe::SixtyMinutes ? 8761 :
                    tf == Timeframe::FifteenMinutes ? 673 :
                    tf == Timeframe::FiveMinutes ? 2017 : 10081;
    return std::min(max_bars_per_asset_timeframe_, cap);
}

void CandleStore::set_max_bars_per_asset_timeframe(int max_bars) {
    std::lock_guard<std::mutex> lk(mu_);
    max_bars_per_asset_timeframe_ = std::max(1, max_bars);
    for (auto& [asset, asset_state] : assets_) {
        (void)asset;
        for (auto& [tf, tf_state] : asset_state.tf_states) {
            (void)tf;
            while ((int)tf_state.bars.size() > retention(tf))
                tf_state.bars.pop_front();
        }
    }
}

int64_t timeframe_to_minutes(Timeframe tf) {
    switch (tf) {
        case Timeframe::OneMinute: return 1;
        case Timeframe::FiveMinutes: return 5;
        case Timeframe::FifteenMinutes: return 15;
        case Timeframe::SixtyMinutes: return 60;
    }
    throw std::runtime_error("Invalid timeframe");
}

CandleStore::AssetState* CandleStore::find_locked(const std::string& asset) {
    auto it = assets_.find(asset);
    return it == assets_.end() ? nullptr : &it->second;
}

const CandleStore::AssetState* CandleStore::find_locked(const std::string& asset) const {
    auto it = assets_.find(asset);
    return it == assets_.end() ? nullptr : &it->second;
}

CandleStore::TimeframeState& CandleStore::get_tf_state_locked(AssetState& asset_state, Timeframe tf) {
    auto it = asset_state.tf_states.find(tf);
    if (it == asset_state.tf_states.end()) {
        throw std::runtime_error("Timeframe state not found for " + std::to_string(static_cast<int>(tf)));
    }
    return it->second;
}

const CandleStore::TimeframeState& CandleStore::get_tf_state_locked(const AssetState& asset_state, Timeframe tf) const {
    auto it = asset_state.tf_states.find(tf);
    if (it == asset_state.tf_states.end()) {
        throw std::runtime_error("Timeframe state not found for " + std::to_string(static_cast<int>(tf)));
    }
    return it->second;
}

CandleStore::TimeframeState& CandleStore::ensure_tf_state_locked(AssetState& asset_state, Timeframe tf) {
    auto& tf_state = asset_state.tf_states[tf];
    if (tf_state.bars.empty() && tf_state.cur_ts_ms == 0) {
        tf_state.cur_ts_ms = 0;
        tf_state.bar_duration_ms = static_cast<int64_t>(timeframe_to_minutes(tf)) * 60000LL;
    }
    return tf_state;
}

void CandleStore::bootstrap_from_pg(Pg& pg) {
    for (const std::string asset : {"BTC", "ETH", "SOL", "XRP"}) {
        int minute_limit, hourly_limit;
        { std::lock_guard lk(mu_); minute_limit = retention(Timeframe::OneMinute); hourly_limit = retention(Timeframe::SixtyMinutes); }
        const auto rows = pg.load_recent_candles(asset_to_symbol(asset), minute_limit);
        const auto hours = pg.load_recent_hourly_candles(asset_to_symbol(asset), hourly_limit);
        AssetState loaded;
        auto& minute = ensure_tf_state_locked(loaded, Timeframe::OneMinute);
        for (const auto& r : rows) {
            Candle c;
            c.ts_ms=r.ts_ms; c.open=r.open; c.high=r.high; c.low=r.low; c.close=r.close; c.volume=r.volume;
            minute.bars.push_back(c);
            propagate_to_higher_timeframes(loaded, c);
        }
        auto& hourly = ensure_tf_state_locked(loaded, Timeframe::SixtyMinutes);
        hourly.bars.clear();
        for (const auto& r : hours) {
            Candle c;
            c.ts_ms=r.ts_ms; c.open=r.open; c.high=r.high; c.low=r.low; c.close=r.close; c.volume=r.volume;
            hourly.bars.push_back(c);
        }
        // Finalize an aggregate whose closing minute is already available.
        const int64_t completed_through = rows.empty() ? 0 : rows.back().ts_ms + 60000;
        for (auto& [tf, state] : loaded.tf_states) {
            if (tf == Timeframe::OneMinute || state.cur_ts_ms == 0) continue;
            if (state.cur_ts_ms + timeframe_to_minutes(tf)*60000 <= completed_through) {
                if (state.bars.empty() || state.bars.back().ts_ms < state.current.ts_ms)
                    state.bars.push_back(state.current);
                state.current = {}; state.cur_ts_ms = 0;
            }
        }
        // Database work is outside the feed mutex. Preserve ticks received
        // during bootstrap and never append a completed history bar twice.
        std::lock_guard lk(mu_);
        auto& live = assets_[asset];
        for (auto& [tf, old] : live.tf_states) {
            auto& target = ensure_tf_state_locked(loaded, tf);
            for (const auto& c : old.bars)
                if (target.bars.empty() || c.ts_ms > target.bars.back().ts_ms) target.bars.push_back(c);
            if (old.cur_ts_ms > 0 && old.cur_ts_ms >= target.cur_ts_ms &&
                (target.bars.empty() || old.cur_ts_ms > target.bars.back().ts_ms)) {
                target.current = old.current; target.cur_ts_ms = old.cur_ts_ms;
                target.completed_minutes = old.completed_minutes;
                target.current_complete = old.current_complete;
            }
        }
        for (auto& [tf, state] : loaded.tf_states)
            while ((int)state.bars.size() > retention(tf)) state.bars.pop_front();
        loaded.last_tick_ms = live.last_tick_ms;
        live = std::move(loaded);
        ++closed_revision_;
        spdlog::info("candle_store: {} bounded history {} minute / {} hourly bars", asset,
            live.tf_states[Timeframe::OneMinute].bars.size(), live.tf_states[Timeframe::SixtyMinutes].bars.size());
    }
    bootstrapped_.store(true, std::memory_order_release);
}

void CandleStore::refresh_recent_from_pg(Pg& pg, int64_t now_ms) {
    for (const std::string asset : {"BTC", "ETH", "SOL", "XRP"}) {
        // Usually only the last three bars need reconciliation. If continuity
        // was lost, fetch the bounded day needed by the forecast kernel.
        const int limit = copy_recent_closes(asset, Timeframe::OneMinute, 1441).size() < 1441
            ? 1441 : 3;
        const auto rows = pg.load_recent_candles(asset_to_symbol(asset), limit);
        std::vector<Candle> bars;
        bars.reserve(rows.size());
        for (const auto& r : rows)
            bars.push_back({r.ts_ms,r.open,r.high,r.low,r.close,r.volume});
        merge_closed_candles(asset, bars, now_ms);
    }
}

void CandleStore::merge_closed_candles(const std::string& asset,
                                      std::span<const Candle> bars, int64_t now_ms) {
    std::lock_guard lk(mu_);
    auto& live = assets_[asset];
    auto& minute = ensure_tf_state_locked(live, Timeframe::OneMinute);
    int64_t first_changed = INT64_MAX, last_changed = 0;
    int inserted = 0;
    for (const auto& c : bars) {
        if (c.ts_ms <= 0 || c.ts_ms % 60000 != 0 || c.ts_ms + 60000 > now_ms ||
            !std::isfinite(c.open) || !std::isfinite(c.high) || !std::isfinite(c.low) ||
            !std::isfinite(c.close) || !std::isfinite(c.volume) ||
            c.low <= 0 || c.open < c.low || c.close < c.low ||
            c.high < c.open || c.high < c.close || c.volume < 0) continue;
        auto at = std::lower_bound(minute.bars.begin(), minute.bars.end(), c.ts_ms,
            [](const Candle& existing, int64_t ts) { return existing.ts_ms < ts; });
        if (at != minute.bars.end() && at->ts_ms == c.ts_ms) {
            if (at->open == c.open && at->high == c.high && at->low == c.low &&
                at->close == c.close && at->volume == c.volume) continue;
            // Preserve observed aggressor volumes, which the archive lacks.
            const auto buy = at->buy_volume, sell = at->sell_volume;
            *at = c; at->buy_volume = buy; at->sell_volume = sell;
        } else {
            minute.bars.insert(at, c);
            ++inserted;
        }
        first_changed = std::min(first_changed, c.ts_ms);
        last_changed = std::max(last_changed, c.ts_ms);
    }
    if (!last_changed) return;
    // An authoritative completed candle supersedes an incomplete live bar.
    if (minute.cur_ts_ms > 0 && minute.cur_ts_ms <= minute.bars.back().ts_ms) {
        minute.current = {}; minute.cur_ts_ms = 0; minute.current_complete = true;
    }
    while ((int)minute.bars.size() > retention(Timeframe::OneMinute)) minute.bars.pop_front();
    for (const auto tf : {Timeframe::FiveMinutes,Timeframe::FifteenMinutes,Timeframe::SixtyMinutes}) {
        auto& higher = ensure_tf_state_locked(live, tf);
        const int64_t duration = timeframe_to_minutes(tf)*60000;
        for (int64_t bucket = first_changed/duration*duration;
             bucket <= last_changed/duration*duration; bucket += duration) {
            auto at = std::lower_bound(minute.bars.begin(), minute.bars.end(), bucket,
                [](const Candle& c, int64_t ts) { return c.ts_ms < ts; });
            Candle aggregate;
            int count = 0;
            for (; at != minute.bars.end() && at->ts_ms < bucket+duration; ++at) {
                if (count++ == 0) { aggregate = *at; aggregate.ts_ms = bucket; }
                else {
                    aggregate.high = std::max(aggregate.high, at->high);
                    aggregate.low = std::min(aggregate.low, at->low);
                    aggregate.close = at->close;
                    aggregate.volume += at->volume;
                    aggregate.buy_volume += at->buy_volume;
                    aggregate.sell_volume += at->sell_volume;
                }
            }
            if (count == timeframe_to_minutes(tf)) {
                auto dest = std::lower_bound(higher.bars.begin(), higher.bars.end(), bucket,
                    [](const Candle& c, int64_t ts) { return c.ts_ms < ts; });
                if (dest != higher.bars.end() && dest->ts_ms == bucket) *dest = aggregate;
                else higher.bars.insert(dest, aggregate);
                if (higher.cur_ts_ms == bucket) {
                    higher.current = {}; higher.cur_ts_ms = 0; higher.completed_minutes = 0;
                }
            } else if (count > 0 && bucket+duration > minute.bars.back().ts_ms+60000 &&
                       bucket >= higher.cur_ts_ms) {
                higher.current = aggregate; higher.cur_ts_ms = bucket;
                higher.completed_minutes = count;
            }
        }
        while ((int)higher.bars.size() > retention(tf)) higher.bars.pop_front();
    }
    ++closed_revision_;
    if (inserted > 0)
        spdlog::info("candle_store: {} archive repaired {} missing closed minutes", asset, inserted);
}

void CandleStore::on_tick(const std::string& asset, double price, double qty,
                          bool buyer_is_maker, int64_t ts_ms) {
    if (!std::isfinite(price) || !std::isfinite(qty) || price <= 0 || qty < 0 || ts_ms <= 0) return;

    std::lock_guard<std::mutex> lk(mu_);
    AssetState& asset_state = assets_[asset];
    if (ts_ms < asset_state.last_tick_ms) return;
    asset_state.last_tick_ms = ts_ms;

    TimeframeState& tf1m = ensure_tf_state_locked(asset_state, Timeframe::OneMinute);
    int64_t bar_1m_ms = floor_to_minute_ms(ts_ms);
    if (!tf1m.bars.empty() && bar_1m_ms <= tf1m.bars.back().ts_ms) return;

    if (tf1m.cur_ts_ms == 0) {
        tf1m.current_complete = ts_ms == bar_1m_ms;
        tf1m.cur_ts_ms = bar_1m_ms;
        tf1m.current.ts_ms = bar_1m_ms;
        tf1m.current.open = price;
        tf1m.current.high = price;
        tf1m.current.low = price;
        tf1m.current.close = price;
        tf1m.current.volume = qty;
        tf1m.current.buy_volume = buyer_is_maker ? 0 : qty;
        tf1m.current.sell_volume = buyer_is_maker ? qty : 0;
        return;
    }

    if (bar_1m_ms == tf1m.cur_ts_ms) {
        tf1m.current.close = price;
        if (price > tf1m.current.high) tf1m.current.high = price;
        if (price < tf1m.current.low) tf1m.current.low = price;
        tf1m.current.volume += qty;
        if (buyer_is_maker) tf1m.current.sell_volume += qty;
        else tf1m.current.buy_volume += qty;
        return;
    }

    if (bar_1m_ms > tf1m.cur_ts_ms) {
        if (tf1m.current_complete) tf1m.bars.push_back(tf1m.current);
        if (tf1m.current_complete) ++closed_revision_;
        while ((int)tf1m.bars.size() > retention(Timeframe::OneMinute)) tf1m.bars.pop_front();
        if (tf1m.current_complete) propagate_to_higher_timeframes(asset_state, tf1m.current);
        tf1m.current_complete = true;
        tf1m.cur_ts_ms = bar_1m_ms;
        tf1m.current.ts_ms = bar_1m_ms;
        tf1m.current.open = price;
        tf1m.current.high = price;
        tf1m.current.low = price;
        tf1m.current.close = price;
        tf1m.current.volume = qty;
        tf1m.current.buy_volume = buyer_is_maker ? 0 : qty;
        tf1m.current.sell_volume = buyer_is_maker ? qty : 0;

        return;
    }
}

void CandleStore::propagate_to_higher_timeframes(AssetState& asset_state, const Candle& c) {
    for (Timeframe tf : kAllTimeframes) {
        if (tf == Timeframe::OneMinute) continue;
        auto& state = ensure_tf_state_locked(asset_state, tf);
        const int64_t tf_ms = timeframe_to_minutes(tf) * 60000LL;
        const int64_t bucket = floor_to_timeframe_ms(c.ts_ms, timeframe_to_minutes(tf));
        if (state.cur_ts_ms != bucket) {
            // A partial bucket across a disconnect is not a complete bar.
            state.current = c; state.current.ts_ms = bucket;
            state.cur_ts_ms = bucket; state.completed_minutes = 1;
        } else {
            state.current.high = std::max(state.current.high,c.high);
            state.current.low = std::min(state.current.low,c.low);
            state.current.close = c.close;
            state.current.volume += c.volume;
            state.current.buy_volume += c.buy_volume;
            state.current.sell_volume += c.sell_volume;
            ++state.completed_minutes;
        }
        if (c.ts_ms + 60000 >= bucket + tf_ms) {
            if (state.completed_minutes == timeframe_to_minutes(tf)) {
                state.bars.push_back(state.current);
                while ((int)state.bars.size() > retention(tf)) state.bars.pop_front();
            }
            state.current = {}; state.cur_ts_ms = 0; state.completed_minutes = 0;
        }
    }
}

std::vector<double> CandleStore::copy_recent_closes(const std::string& asset, Timeframe tf, int n) const {
    if (n <= 0) return {};
    std::lock_guard<std::mutex> lk(mu_);
    const auto* asset_state = find_locked(asset);
    if (!asset_state) return {};
    // A timeframe state may not exist yet (e.g. when the history bootstrap was
    // skipped and no bar has propagated to this timeframe). Treat as empty
    // rather than throwing — readers (the predictor) handle short/empty series.
    auto tfit = asset_state->tf_states.find(tf);
    if (tfit == asset_state->tf_states.end()) return {};
    const auto& tf_state = tfit->second;
    std::vector<double> closes;
    closes.reserve(std::min(n, (int)tf_state.bars.size()));
    int start = std::max(0, (int)tf_state.bars.size() - n);
    if (tf == Timeframe::OneMinute) {
        for (int i = (int)tf_state.bars.size()-1; i > start; --i)
            if (tf_state.bars[i].ts_ms - tf_state.bars[i-1].ts_ms != 60000) { start=i; break; }
    }
    for (int i = start; i < (int)tf_state.bars.size(); ++i)
        closes.push_back(tf_state.bars[i].close);
    return closes;
}

std::vector<Candle> CandleStore::copy_recent_bars(const std::string& asset, Timeframe tf, int n) const {
    if (n <= 0) return {};
    std::lock_guard<std::mutex> lk(mu_);
    const auto* asset_state = find_locked(asset);
    if (!asset_state) return {};
    auto tfit = asset_state->tf_states.find(tf);
    if (tfit == asset_state->tf_states.end()) return {};
    const auto& tf_state = tfit->second;
    std::vector<Candle> out;
    out.reserve(std::min(n, (int)tf_state.bars.size()));
    int start = std::max(0, (int)tf_state.bars.size() - n);
    if (tf == Timeframe::OneMinute) {
        for (int i = (int)tf_state.bars.size()-1; i > start; --i)
            if (tf_state.bars[i].ts_ms - tf_state.bars[i-1].ts_ms != 60000) { start=i; break; }
    }
    for (int i = start; i < (int)tf_state.bars.size(); ++i)
        out.push_back(tf_state.bars[i]);
    return out;
}

int CandleStore::size(const std::string& asset, Timeframe tf) const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto* asset_state = find_locked(asset);
    if (!asset_state) return 0;
    auto tfit = asset_state->tf_states.find(tf);
    if (tfit == asset_state->tf_states.end()) return 0;
    return (int)tfit->second.bars.size();
}

bool CandleStore::has_asset(const std::string& asset) const {
    std::lock_guard<std::mutex> lk(mu_);
    return assets_.find(asset) != assets_.end();
}

} // namespace vp::engine
