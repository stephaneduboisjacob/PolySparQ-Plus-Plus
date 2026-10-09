#include "engine/replay.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <vector>

using vp::engine::ReplayCursor;
using vp::engine::ReplayParams;
using vp::engine::ReplayTick;

namespace {

// ~0.1% alternating 1m moves -> EWMA sigma_minute near 0.001 with enough
// samples for the forecast kernel to report ready.
std::vector<double> synthetic_closes() {
    std::vector<double> closes;
    closes.reserve(1500);
    double px = 100.0;
    for (int i = 0; i < 1500; ++i) {
        px = (i % 2 == 0) ? 100.0 : 100.1;
        closes.push_back(px);
    }
    return closes;
}

ReplayParams test_params() {
    ReplayParams p;
    p.sigma_minute = 0.001;
    p.vol_samples = 100;
    p.min_cb_delta_pct = 0.039;
    p.min_oracle_delta_pct = 0.039;
    p.band_min = 0.40;
    p.band_max = 0.80;
    p.min_secs_left = 10;
    p.max_secs_left = 260;
    return p;
}

} // namespace

int main() {
    // Aligned 5m window: t0 must satisfy t0 % 300000 == 0.
    const std::int64_t t0 = 1'800'000'000'000 -
                            (1'800'000'000'000 % 300'000);

    // ── 1. CB leads, oracle confirms later -> entry + winning settlement ──
    {
        const std::vector<ReplayTick> oracle{
            {t0 - 1'000, 100.0}, {t0 + 25'000, 100.0}, {t0 + 295'000, 100.06}};
        const std::vector<ReplayTick> cb{
            {t0 - 1'000, 100.0}, {t0 + 60'000, 100.05}, {t0 + 70'000, 100.06}};
        ReplayCursor cur;
        const auto r = vp::engine::replay_window(
            test_params(), t0, oracle, cb, cur);
        assert(r.entry.has_value());
        assert(r.entry->up);
        assert(std::abs(r.entry->cb_delta_pct - 0.05) < 1e-9);
        assert(r.entry->secs_left == 240);
        assert(r.entry->entry_price > 0.40 && r.entry->entry_price < 0.80);
        assert(r.settles_up.has_value() && *r.settles_up);
        const double pnl = vp::engine::replay_pnl_per_share(
            r.entry->entry_price, true, test_params());
        assert(pnl > 0.0);
    }

    // ── 2. Same signal, oracle closes below open -> losing settlement ──
    {
        const std::vector<ReplayTick> oracle{
            {t0 - 1'000, 100.0}, {t0 + 25'000, 100.0}, {t0 + 295'000, 99.90}};
        const std::vector<ReplayTick> cb{
            {t0 - 1'000, 100.0}, {t0 + 60'000, 100.05}};
        ReplayCursor cur;
        const auto r = vp::engine::replay_window(
            test_params(), t0, oracle, cb, cur);
        assert(r.entry.has_value() && r.entry->up);
        assert(r.settles_up.has_value() && !*r.settles_up);
        assert(vp::engine::replay_pnl_per_share(
                   r.entry->entry_price, false, test_params()) < 0.0);
    }

    // ── 3. Oracle already confirmed before CB -> no lead entry ──
    {
        const std::vector<ReplayTick> oracle{
            {t0 - 1'000, 100.0}, {t0 + 10'000, 100.06}, {t0 + 295'000, 100.06}};
        const std::vector<ReplayTick> cb{
            {t0 - 1'000, 100.0}, {t0 + 60'000, 100.05}};
        ReplayCursor cur;
        const auto r = vp::engine::replay_window(
            test_params(), t0, oracle, cb, cur);
        assert(!r.entry.has_value());
    }

    // ── 4. Entry band filter (min above any modeled price) blocks entry ──
    {
        const std::vector<ReplayTick> oracle{
            {t0 - 1'000, 100.0}, {t0 + 25'000, 100.0}, {t0 + 295'000, 100.06}};
        const std::vector<ReplayTick> cb{
            {t0 - 1'000, 100.0}, {t0 + 60'000, 100.05}};
        ReplayParams p = test_params();
        p.band_min = 0.99;
        ReplayCursor cur;
        const auto r = vp::engine::replay_window(p, t0, oracle, cb, cur);
        assert(!r.entry.has_value());
        // Settlement is independent of the entry band.
        assert(r.settles_up.has_value() && *r.settles_up);
    }

    // ── 5. Fee math matches the paper model at a fixed price ──
    {
        ReplayParams p = test_params();
        p.fee_rate = 0.07;
        p.fee_exponent = 1;
        const double fee = 0.07 * 0.60 * 0.40;  // 0.0168 at px 0.60
        const double win =
            vp::engine::replay_pnl_per_share(0.60, true, p);
        const double lose =
            vp::engine::replay_pnl_per_share(0.60, false, p);
        assert(std::abs(win - (1.0 - 0.60 - fee)) < 1e-12);
        assert(std::abs(lose - (-0.60 - fee)) < 1e-12);
    }

    // ── 6. Cursor monotonicity across consecutive windows ──
    {
        const std::int64_t t1 = t0 + 300'000;
        const std::vector<ReplayTick> oracle{
            {t0 - 1'000, 100.0}, {t0 + 150'000, 100.06},
            {t1 + 250'000, 99.95}};
        const std::vector<ReplayTick> cb{
            {t0 - 1'000, 100.0}, {t0 + 60'000, 100.05},
            {t1 - 1'000, 100.06}, {t1 + 60'000, 99.99}};
        ReplayCursor cur;
        const auto w0 = vp::engine::replay_window(
            test_params(), t0, oracle, cb, cur);
        const auto w1 = vp::engine::replay_window(
            test_params(), t1, oracle, cb, cur);
        // Window 0 settles UP. Window 1's open reference is the 100.06 print
        // inside window 0 (last tick at-or-before t1); its only oracle print
        // (99.95) settles DOWN, and CB leads DOWN there too.
        assert(w0.settles_up.has_value() && *w0.settles_up);
        assert(w1.settles_up.has_value() && !*w1.settles_up);
        assert(w1.entry.has_value() && !w1.entry->up);  // CB leads DOWN
    }

    // ── 7. Missing opening reference -> window skipped, no crash ──
    {
        const std::vector<ReplayTick> oracle{{t0 + 150'000, 100.0}};
        const std::vector<ReplayTick> cb{{t0 + 60'000, 100.05}};
        ReplayCursor cur;
        const auto r = vp::engine::replay_window(
            test_params(), t0, oracle, cb, cur);
        assert(!r.entry.has_value() && !r.settles_up.has_value());
    }
}
