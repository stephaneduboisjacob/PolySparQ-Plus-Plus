#pragma once
// Replay/backtest core for the CB-lead / oracle-lag strategy family.
//
// Reconstructs per-window entry signals from recorded oracle and Coinbase
// ticks (vp.oracle_ticks / vp.coinbase_ticks, collected by the cb instance)
// and scores threshold sets net of fees. Recorded books are NOT archived, so
// the modeled fill price is the CB-implied fair value from the same forecast
// kernel the live engine uses plus a configurable slippage allowance — the
// same cost model the live entry gates charge before taking an order.
//
// Settlement follows the engine's fallback semantics: the window resolves
// against the oracle print inside (open, close) (UP wins when close >= open).
// Windows without an oracle tick stay unresolved, exactly like the live
// settlement defers.
//
// Pure and header-only, like forecast.hpp. See polypulse-replay (CLI) and
// tests/test_replay.cpp.
#include "forecast.hpp"
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace vp::engine {

struct ReplayTick {
    std::int64_t ts_ms = 0;
    double price = 0.0;
};

struct ReplayParams {
    int    horizon_s = 300;
    double min_cb_delta_pct = 0.039;      // CB-lead trigger magnitude
    double min_oracle_delta_pct = 0.039;  // oracle "flat" threshold for the lead
    double band_min = 0.46;               // modeled entry price band
    double band_max = 0.68;
    int    min_secs_left = 30;            // entry time gate
    int    max_secs_left = 260;
    double fee_rate = 0.07;
    int    fee_exponent = 1;
    double slippage = 0.005;              // per-share half-spread proxy
    double sigma_minute = 0.0;            // candle EWMA vol for the price model
    int    vol_samples = 0;
};

struct ReplayEntry {
    std::int64_t ts_ms = 0;
    int    secs_left = 0;
    bool   up = true;
    double entry_price = 0.0;   // modeled cost per share (fair + slippage)
    double cb_delta_pct = 0.0;  // trigger magnitude at entry
};

struct ReplayWindowResult {
    std::optional<ReplayEntry> entry;
    std::optional<bool> settles_up;   // nullopt = no oracle tick inside window
};

struct ReplayCursor {
    // Advancing read positions into the tick arrays; monotonic across
    // successive windows processed in chronological order. Each window owns
    // ticks in [open, close): a tick exactly at the boundary opens the NEXT
    // window. last_oracle/last_cb carry the most recent price seen so the
    // next window's opening reference survives ticks consumed as events by
    // the previous window's scan.
    std::size_t oracle = 0;
    std::size_t cb = 0;
    double last_oracle = 0.0;
    double last_cb = 0.0;
};

inline ReplayWindowResult replay_window(const ReplayParams& p,
                                        std::int64_t open_ms,
                                        std::span<const ReplayTick> oracle,
                                        std::span<const ReplayTick> cb,
                                        ReplayCursor& cur) {
    ReplayWindowResult out;
    const std::int64_t close_ms = open_ms + std::int64_t(p.horizon_s) * 1000;

    // Opening references: last tick at-or-before open_ms (from this window's
    // pre-advance or carried over from earlier windows).
    while (cur.oracle < oracle.size() && oracle[cur.oracle].ts_ms <= open_ms)
        cur.last_oracle = oracle[cur.oracle++].price;
    while (cur.cb < cb.size() && cb[cur.cb].ts_ms <= open_ms)
        cur.last_cb = cb[cur.cb++].price;
    const double oracle_open = cur.last_oracle;
    const double cb_open = cur.last_cb;
    if (!(oracle_open > 0) || !(cb_open > 0)) return out;

    double oracle_now = oracle_open, cb_at_oracle = cb_open;
    std::size_t cb_ref = cur.cb;  // sub-pointer: cb tick aligned to oracle print

    // Merge-scan events in [open, close). The CB stream drives the lead
    // trigger; oracle prints update the projection baseline (cb_at_oracle
    // mirrors main.cpp fast_forecast: only CB's move since the last official
    // print is projected onto the strike).
    std::size_t oi = cur.oracle, ci = cur.cb;
    while (oi < oracle.size() || ci < cb.size()) {
        const bool oracle_first =
            ci >= cb.size() ||
            (oi < oracle.size() && oracle[oi].ts_ms <= cb[ci].ts_ms);
        if (oracle_first) {
            const auto& t = oracle[oi];
            if (t.ts_ms >= close_ms) break;
            oracle_now = t.price;
            cur.last_oracle = t.price;
            while (cb_ref < cb.size() && cb[cb_ref].ts_ms <= t.ts_ms)
                cb_at_oracle = cb[cb_ref++].price;
            if (t.ts_ms > open_ms && t.price > 0)
                out.settles_up = t.price >= oracle_open;
            ++oi;
        } else {
            const auto& t = cb[ci];
            if (t.ts_ms >= close_ms) break;
            cur.last_cb = t.price;
            const int secs_left = int((close_ms - t.ts_ms) / 1000);
            if (t.price > 0 && oracle_now > 0 && cb_at_oracle > 0 &&
                secs_left >= p.min_secs_left && secs_left <= p.max_secs_left &&
                !out.entry) {
                const double cb_delta = 100.0 * (t.price - cb_open) / cb_open;
                const double oracle_delta =
                    100.0 * (oracle_now - oracle_open) / oracle_open;
                if (std::abs(cb_delta) >= p.min_cb_delta_pct &&
                    std::abs(oracle_delta) < p.min_oracle_delta_pct) {
                    // CB leads, oracle has not confirmed: price the side the
                    // live fast_forecast would, then apply the band gate.
                    const double projected =
                        oracle_now * (t.price / cb_at_oracle);
                    const ForecastInput in{projected, oracle_open,
                                           double(secs_left), p.sigma_minute,
                                           0.0, 0.0, p.vol_samples, 0, 0.0};
                    const auto f = forecast(in);
                    if (f.ready) {
                        const bool up = cb_delta > 0.0;
                        const double px =
                            (up ? f.probability : 1.0 - f.probability) +
                            p.slippage;
                        if (px >= p.band_min && px <= p.band_max)
                            out.entry =
                                ReplayEntry{t.ts_ms, secs_left, up, px,
                                            cb_delta};
                    }
                }
            }
            ++ci;
        }
    }
    cur.oracle = oi;
    cur.cb = ci;
    return out;
}

// Net P&L per share for a hold-to-settlement entry: the winner redeems at
// $1.00 with no redemption fee; the entry taker fee is charged at the entry
// price (same model as paper.cpp / live.cpp).
inline double replay_pnl_per_share(double entry_price, bool win,
                                   const ReplayParams& p) {
    double pe = 1.0, qe = 1.0;
    for (int i = 0; i < p.fee_exponent; ++i) {
        pe *= entry_price;
        qe *= (1.0 - entry_price);
    }
    const double fee = p.fee_rate * pe * qe;
    return win ? (1.0 - entry_price - fee) : (-entry_price - fee);
}

} // namespace vp::engine
