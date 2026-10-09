// polypulse-replay — offline threshold sweeper for the CB-lead strategy.
//
// Replays recorded oracle/Coinbase ticks through the replay_window core over
// a historical range and reports, per threshold set, entries / win rate /
// net-of-fee P&L using the forecast-kernel entry price model. Books are not
// archived, so fill quality (band position, partial fills, queue) is
// approximated by the slippage allowance — treat per-$ results as an upper
// bound and the RELATIVE ranking of threshold sets as the usable signal.
//
// Usage:
//   polypulse-replay [--asset BTC] [--horizon 300] [--days 14]
//                    [--from "2026-08-23"] [--to "2026-09-06 12:00"]
//                    [--min-cb-delta 0.039] [--min-oracle-delta 0.039]
//                    [--band-min 0.46] [--band-max 0.68]
//                    [--min-secs 30] [--max-secs 260]
//                    [--fee-rate 0.07] [--fee-exp 1] [--slippage 0.005]
//                    [--sweep] [--csv]
//   --sweep loops min-cb-delta × band-max grids; --csv prints one
//   machine-readable row per threshold set.
//
// DB access uses libpq defaults; override with POLYPULSE_DSN
// (default: dbname=vantagepulse).
#include "replay.hpp"

#include <pqxx/pqxx>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

namespace {

// Known libpqxx static-teardown double-free (documented in
// memory/prediction-revamp-paused-sep05.md): pqxx template statics corrupt
// the heap in global destructors at process exit. This is an offline CLI —
// by the time we exit, all work and output are complete, so flush and skip
// the broken static teardown. Do NOT copy this into long-running services;
// the underlying libpqxx issue still needs a real fix (see memory).
[[noreturn]] void finish(int code) {
    std::cout.flush();
    std::cerr.flush();
    std::fflush(nullptr);
    std::_Exit(code);
}

std::int64_t parse_local_utc_to_ms(const std::string& s,
                                   std::int64_t fallback_ms) {
    if (s.empty()) return fallback_ms;
    int Y = 0, M = 0, D = 0, h = 0, m = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%d %d:%d", &Y, &M, &D, &h, &m) < 3)
        return fallback_ms;
    std::tm tm{};
    tm.tm_year = Y - 1900; tm.tm_mon = M - 1; tm.tm_mday = D;
    tm.tm_hour = h; tm.tm_min = m;
    return static_cast<std::int64_t>(::timegm(&tm)) * 1000;
}

std::vector<vp::engine::ReplayTick> load_ticks(
    pqxx::connection& c, const std::string& table, const std::string& asset_col,
    const std::string& asset, std::int64_t from_ms, std::int64_t to_ms) {
    std::vector<vp::engine::ReplayTick> ticks;
    pqxx::work tx{c};
    const std::string sql = fmt::format(
        "SELECT (EXTRACT(EPOCH FROM ts)*1000)::bigint AS ms, price "
        "FROM vp.{} WHERE {}=$1 AND price > 0 AND ts >= to_timestamp($2/1000.0) "
        "AND ts <  to_timestamp($3/1000.0) ORDER BY ts",
        table, asset_col);
    const auto r = tx.exec_params(sql, asset, from_ms, to_ms);
    for (const auto& row : r)
        ticks.push_back({row[0].as<std::int64_t>(), row[1].as<double>()});
    tx.commit();
    return ticks;
}

// Minute closes ending at from_ms (for the EWMA volatility warm-up) plus the
// range itself; one flat array, sliced per window.
std::vector<double> load_closes(pqxx::connection& c, const std::string& symbol,
                                std::int64_t from_ms, std::int64_t to_ms) {
    std::vector<double> closes;
    pqxx::work tx{c};
    constexpr std::int64_t warmup_ms = 1441 * 60 * 1000;
    const auto r = tx.exec_params(
        "SELECT close FROM vp.candles_1m WHERE symbol=$1 AND ts >= "
        "to_timestamp($2/1000.0) AND ts < to_timestamp($3/1000.0) ORDER BY ts",
        symbol, from_ms - warmup_ms, to_ms);
    for (const auto& row : r) closes.push_back(row[0].as<double>());
    tx.commit();
    return closes;
}

struct SweepStats {
    long long windows = 0, entries = 0, wins = 0, losses = 0, unresolved = 0;
    double entry_cost = 0.0, pnl = 0.0;
};

SweepStats run_range(const std::vector<vp::engine::ReplayTick>& oracle,
                     const std::vector<vp::engine::ReplayTick>& cb,
                     const std::vector<double>& closes, std::int64_t from_ms,
                     std::int64_t to_ms, const vp::engine::ReplayParams& base) {
    using namespace vp::engine;
    SweepStats st;
    ReplayCursor cur;
    const std::int64_t step_ms = std::int64_t(base.horizon_s) * 1000;
    // load_closes() starts warmup_ms before from_ms; the first candle's open
    // minute, aligned down, anchors close-index arithmetic.
    constexpr std::int64_t warmup_ms = 1441 * 60 * 1000;
    const std::int64_t first_open = ((from_ms - warmup_ms) / 60000) * 60000;
    for (std::int64_t open = from_ms; open + step_ms <= to_ms; open += step_ms) {
        ReplayParams p = base;
        // EWMA volatility from the 1441 closes ending at window open.
        const std::size_t want = 1441;
        const std::int64_t idx = (open - first_open) / 60000 - 1;  // close index
        if (idx >= 1 && std::size_t(idx) < closes.size()) {
            const std::size_t end = std::size_t(idx) + 1;
            const std::size_t begin = end > want ? end - want : 0;
            const auto vol = estimate_volatility(
                std::span<const double>(closes.data() + begin, end - begin));
            p.sigma_minute = vol.sigma_minute;
            p.vol_samples = vol.samples;
        }
        ++st.windows;
        const auto r = replay_window(p, open, oracle, cb, cur);
        if (!r.entry) continue;
        ++st.entries;
        st.entry_cost += r.entry->entry_price;
        if (!r.settles_up) { ++st.unresolved; continue; }
        if (*r.settles_up) ++st.wins; else ++st.losses;
        st.pnl += replay_pnl_per_share(r.entry->entry_price, *r.settles_up, p);
    }
    return st;
}

void report(const std::string& label, const vp::engine::ReplayParams& p,
            const SweepStats& st, bool csv) {
    const double wr =
        (st.wins + st.losses) > 0
            ? 100.0 * double(st.wins) / double(st.wins + st.losses) : 0.0;
    const double avg_px =
        st.entries > 0 ? st.entry_cost / double(st.entries) : 0.0;
    const double per_dollar =
        st.entry_cost > 0 ? st.pnl / st.entry_cost : 0.0;
    if (csv) {
        fmt::print("{},{:.3f},{:.2f},{},{},{:.2f},{:.3f},{:.2f},{:.4f}\n",
                   label, p.min_cb_delta_pct, p.band_max, st.entries,
                   st.wins + st.losses, wr, avg_px, st.pnl, per_dollar);
        return;
    }
    fmt::print(
        "cb>={:.3f}% band<={:.2f} | windows={} entries={} settled={} "
        "win={:.1f}% avg_px={:.3f} net=${:+.2f} per_$1={:+.4f} unresolved={}\n",
        p.min_cb_delta_pct, p.band_max, st.windows, st.entries,
        st.wins + st.losses, wr, avg_px, st.pnl, per_dollar, st.unresolved);
}

} // namespace

int main(int argc, char** argv) {
    std::string asset = "BTC";
    int horizon = 300, days = 14;
    std::string from_s, to_s;
    vp::engine::ReplayParams p;
    bool sweep = false, csv = false;
    for (int i = 1; i < argc; ++i) {
        auto next = [&]() -> std::string { return argv[++i]; };
        if (!std::strcmp(argv[i], "--asset")) asset = next();
        else if (!std::strcmp(argv[i], "--horizon")) horizon = std::atoi(next().c_str());
        else if (!std::strcmp(argv[i], "--days")) days = std::atoi(next().c_str());
        else if (!std::strcmp(argv[i], "--from")) from_s = next();
        else if (!std::strcmp(argv[i], "--to")) to_s = next();
        else if (!std::strcmp(argv[i], "--min-cb-delta")) p.min_cb_delta_pct = std::atof(next().c_str());
        else if (!std::strcmp(argv[i], "--min-oracle-delta")) p.min_oracle_delta_pct = std::atof(next().c_str());
        else if (!std::strcmp(argv[i], "--band-min")) p.band_min = std::atof(next().c_str());
        else if (!std::strcmp(argv[i], "--band-max")) p.band_max = std::atof(next().c_str());
        else if (!std::strcmp(argv[i], "--min-secs")) p.min_secs_left = std::atoi(next().c_str());
        else if (!std::strcmp(argv[i], "--max-secs")) p.max_secs_left = std::atoi(next().c_str());
        else if (!std::strcmp(argv[i], "--fee-rate")) p.fee_rate = std::atof(next().c_str());
        else if (!std::strcmp(argv[i], "--fee-exp")) p.fee_exponent = std::atoi(next().c_str());
        else if (!std::strcmp(argv[i], "--slippage")) p.slippage = std::atof(next().c_str());
        else if (!std::strcmp(argv[i], "--sweep")) sweep = true;
        else if (!std::strcmp(argv[i], "--csv")) csv = true;
        else { std::cerr << "unknown arg: " << argv[i] << "\n"; finish(2); }
    }
    p.horizon_s = horizon;

    try {
        const char* dsn_env = std::getenv("POLYPULSE_DSN");
        pqxx::connection c{dsn_env ? dsn_env : "dbname=vantagepulse"};
        const std::int64_t now_ms =
            static_cast<std::int64_t>(std::time(nullptr)) * 1000;
        const std::int64_t to_ms = parse_local_utc_to_ms(to_s, now_ms);
        const std::int64_t from_ms =
            parse_local_utc_to_ms(from_s, to_ms - std::int64_t(days) * 86400000);

        const auto oracle =
            load_ticks(c, "oracle_ticks", "asset", asset, from_ms, to_ms);
        const auto cb =
            load_ticks(c, "coinbase_ticks", "asset", asset, from_ms, to_ms);
        const auto closes =
            load_closes(c, asset + "USDT", from_ms, to_ms);
        if (csv)
            fmt::print("label,min_cb_delta,band_max,entries,settled,win_pct,avg_px,net_pnl,per_dollar\n");
        else
            fmt::print(
                "replay {} {}s  ticks: oracle={} cb={}  candles={}  range {}..{}\n",
                asset, horizon, oracle.size(), cb.size(), closes.size(),
                from_ms, to_ms);
        if (oracle.empty() || cb.empty() || closes.empty()) {
            std::cerr << "no data in range\n";
            finish(1);
        }

        if (!sweep) {
            const auto st = run_range(oracle, cb, closes, from_ms, to_ms, p);
            report(asset, p, st, csv);
            finish(0);
        }
        for (double d : {0.020, 0.030, 0.039, 0.050, 0.065, 0.080}) {
            for (double bmax : {0.60, 0.68, 0.80}) {
                vp::engine::ReplayParams q = p;
                q.min_cb_delta_pct = d;
                q.band_max = bmax;
                const auto st = run_range(oracle, cb, closes, from_ms, to_ms, q);
                report(asset, q, st, csv);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "replay failed: " << e.what() << "\n";
        finish(1);
    }
    finish(0);
}
