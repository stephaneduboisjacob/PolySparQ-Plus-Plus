// PolySparQ++ Data Collector — headless research data collection binary.
//
// Runs 24/7 to accumulate tick-level data for ML/pattern analysis:
//   - Binance trade ticks (price, qty, aggressor)
//   - Polymarket RTDS oracle ticks (Chainlink BTC/ETH prices)
//   - Polymarket CLOB order book snapshots + public trades
//   - Predictor feature snapshots (RSI, MACD, BB, fair value, edge, etc.)
//   - Window lifecycle events (opened, first_oracle_seed, settled)
//
// No TUI, no trading, no executor, no positions — pure data collection.

#include "config/config.hpp"
#include "core/clock.hpp"
#include "core/types.hpp"
#include "core/oracle_buffer.hpp"
#include "db/pg.hpp"
#include "db/persist_queue.hpp"
#include "engine/candle_store.hpp"
#include "engine/predictor.hpp"
#include "feeds/binance.hpp"
#include "feeds/polymarket_md.hpp"
#include "feeds/polymarket_rest.hpp"
#include "feeds/polymarket_rtds.hpp"
#include "net/http_client.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <fmt/format.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <functional>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
std::atomic<bool> g_stop{false};
constexpr int kPredictorRecent1mBars = 10080;          // 7 days
constexpr int kPredictorRecent1mOhlcvBars = 10080;     // 7 days
constexpr int kPredictorLong60mBars = 24 * 365 * 5;    // 5 years
void on_sig(int) { g_stop.store(true); }
} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT,  on_sig);
    std::signal(SIGTERM, on_sig);

    // Logging: stdout + file
    std::filesystem::create_directories("logs");
    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
        "logs/polypulse-collector.log", false);
    file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
    auto logger = std::make_shared<spdlog::logger>("vp_collector",
        spdlog::sinks_init_list{file_sink, console_sink});
    spdlog::set_default_logger(logger);
    spdlog::flush_on(spdlog::level::info);

    std::string cfg_path = (argc > 1) ? argv[1] : "config/polypulse.toml";
    vp::Config cfg;
    try { cfg = vp::Config::load(cfg_path); }
    catch (const std::exception& e) {
        std::fprintf(stderr, "config load failed (%s): %s\n", cfg_path.c_str(), e.what());
        return 2;
    }
    spdlog::set_level(spdlog::level::from_str(cfg.runtime.log_level));
    cfg.dump_summary();

    // Postgres connection
    std::unique_ptr<vp::Pg> pg;
    try { pg = std::make_unique<vp::Pg>(cfg.postgres.conninfo); }
    catch (const std::exception& e) {
        std::fprintf(stderr, "postgres connect failed: %s\n", e.what());
        return 3;
    }
    if (!pg->ping()) { std::fprintf(stderr, "postgres ping failed\n"); return 4; }
    spdlog::info("postgres OK");

    // Persist queue — drains on a background thread
    vp::BoundedQueue<std::function<void()>> persist_q;
    std::thread persist_th([&] {
        std::function<void()> task;
        while (!g_stop.load()) {
            if (persist_q.pop(task, 1000))
                task();
        }
        while (persist_q.pop(task, 0)) task();
    });

    // Oracle price buffer (for seeding window open prices)
    vp::core::OracleBuffer oracle_buf;

    // Candle store (builds 1-min bars from Binance ticks; needed for predictor)
    vp::engine::CandleStore candle_store;
    candle_store.bootstrap_from_pg(*pg);

    // Track which {asset, window_open_ms} pairs have been seeded
    std::set<std::pair<std::string, std::int64_t>> oracle_seeded_windows;

    // Lightweight state shared between feed callbacks and refresh thread
    struct CollectorState {
        std::mutex mu;
        std::vector<vp::PmMarket> markets;
        // Per-asset price maps (keyed by "BTC"/"ETH"/"SOL"/"XRP")
        std::unordered_map<std::string, double> asset_price;
        std::unordered_map<std::string, std::vector<double>> asset_recent;
        std::unordered_map<std::string, double> cb_asset_price;
        std::unordered_map<std::string, double> oracle_price;
        // Token → condition_id mapping (for PolymarketMD callbacks)
        std::unordered_map<std::string, std::string> token_to_cond;
        std::unordered_map<std::string, vp::ShareSide> token_to_side;
        // UP price tick history for velocity computation
        std::unordered_map<std::string, std::deque<std::pair<std::int64_t, double>>> up_price_hist;
    };
    CollectorState cstate;

    // Polymarket REST market discovery
    vp::PolymarketRest rest(cfg.polymarket.clob_rest_url, cfg.polymarket.gamma_rest_url);

    // Feeds
    std::unique_ptr<vp::PolymarketMD>  pm_md;
    std::unique_ptr<vp::BinanceFeed>   binance;
    std::unique_ptr<vp::PolymarketRtds> rtds;

    // Rate limiter for book snapshot persistence (1 Hz per token)
    std::unordered_map<std::string, std::int64_t> last_snap_ms;

    // Boostrap: initial market discovery + feed startup
    std::thread bootstrap_th([&] {
        auto markets = rest.discover_updown_markets();
        spdlog::info("discovered {} UP/DOWN markets", markets.size());

        {
            std::lock_guard<std::mutex> lk(cstate.mu);
            cstate.markets = std::move(markets);
            for (const auto& m : cstate.markets) {
                cstate.token_to_cond[m.yes_token_id] = m.condition_id;
                cstate.token_to_cond[m.no_token_id]  = m.condition_id;
                cstate.token_to_side[m.yes_token_id] = vp::ShareSide::UP;
                cstate.token_to_side[m.no_token_id]  = vp::ShareSide::DOWN;
            }
        }

        // Upsert markets into Postgres + log opened events
        for (const auto& m : cstate.markets) {
            persist_q.push([pg=pg.get(), m]() noexcept {
                pg->upsert_market(m.condition_id, m.asset, m.horizon_seconds,
                                  m.target_price, m.window_open_ms, m.window_close_ms,
                                  m.yes_token_id, m.no_token_id, m.question, m.slug,
                                  m.settlement_window_s, m.reference_is_binance);
            });
            if (cfg.data_collection.window_events_enabled && m.window_open_ms > 0) {
                persist_q.push([pg=pg.get(), cid=m.condition_id]() noexcept {
                    pg->insert_window_event(cid, "opened", "");
                });
            }
        }

        // Polymarket CLOB market-data WebSocket (order books + trades)
        pm_md = std::make_unique<vp::PolymarketMD>(
            cfg.polymarket.clob_ws_url,
            [&cstate, &persist_q, &last_snap_ms, pg=pg.get()]
            (const vp::PolymarketMD::BookUpdate& bu) {
                auto cit = cstate.token_to_cond.find(bu.token_id);
                if (cit == cstate.token_to_cond.end()) return;
                auto sit = cstate.token_to_side.find(bu.token_id);
                if (sit == cstate.token_to_side.end()) return;
                // Rate-limited persist: 1 snapshot/token/s
                // Note: only snapshot messages give us the true top-of-book.
                // Delta updates carry only changed levels, so best_bid/ask may be stale.
                // Full OrderBook tracking is done by the main polypulse binary.
                std::int64_t now_ms = vp::Clock::wall_ns() / 1'000'000LL;
                auto& last = last_snap_ms[bu.token_id];
                if (now_ms - last >= 1000) {
                    last = now_ms;
                    double bb = bu.bids.empty() ? 0 : bu.bids[0].price;
                    double ba = bu.asks.empty() ? 0 : bu.asks[0].price;
                    persist_q.push([pg, cid=cit->second, tok=bu.token_id,
                                    side=sit->second, bb, ba]() noexcept {
                        std::vector<vp::PriceLevel> empty;
                        pg->insert_book_snapshot(cid, tok, side, empty, empty, bb, ba);
                    });
                }
            },
            [&persist_q, pg=pg.get()](const vp::PolymarketMD::TradeUpdate& tu) {
                persist_q.push([pg, tu]() noexcept {
                    pg->insert_pm_trade(tu.condition_id, tu.token_id,
                                        tu.side_label,
                                        tu.price, tu.size, tu.exchange_ms);
                });
            });
        pm_md->start();
        {
            std::vector<std::string> tokens;
            std::lock_guard<std::mutex> lk(cstate.mu);
            tokens.reserve(cstate.markets.size() * 2);
            for (const auto& m : cstate.markets) {
                tokens.push_back(m.yes_token_id);
                tokens.push_back(m.no_token_id);
            }
            pm_md->subscribe(tokens);
            spdlog::info("pm_md subscribed to {} tokens", tokens.size());
        }

        // Binance trade stream
        binance = std::make_unique<vp::BinanceFeed>(
            cfg.binance.ws_url,
            cfg.binance.streams,
            [&cstate, &candle_store, &persist_q, pg=pg.get(), &cfg](const vp::AssetTick& t) {
                std::string asset_key = t.symbol.substr(0, t.symbol.size() - 4);
                {
                    std::lock_guard<std::mutex> lk(cstate.mu);
                    auto& price  = cstate.asset_price[asset_key];
                    auto& recent = cstate.asset_recent[asset_key];
                    price = t.price;
                    recent.push_back(t.price);
                    if (recent.size() > 80) recent.erase(recent.begin());
                }
                std::string asset = asset_key;
                candle_store.on_tick(asset, t.price, t.qty, t.buyer_is_maker, t.exchange_ms);
                // Persist Binance tick
                if (cfg.data_collection.binance_ticks_enabled) {
                    persist_q.push([pg, sym=t.symbol, price=t.price,
                                    qty=t.qty, maker=t.buyer_is_maker,
                                    exch_ms=t.exchange_ms]() noexcept {
                        pg->insert_binance_tick(sym, price, qty, maker, exch_ms);
                    });
                }
            });
        binance->start();

        // Polymarket RTDS oracle feed
        if (cfg.polymarket.rtds_enabled && !cfg.polymarket.rtds_ws_url.empty()) {
            spdlog::info("rtds feed: {}", cfg.polymarket.rtds_ws_url);
            rtds = std::make_unique<vp::PolymarketRtds>(
                cfg.polymarket.rtds_ws_url,
                [&cstate, &oracle_buf, &oracle_seeded_windows, &persist_q,
                 pg=pg.get(), &cfg](const vp::OracleTick& t) {
                    // Update shared state
                    {
                        std::lock_guard<std::mutex> lk(cstate.mu);
                        if (t.asset == "BTC") cstate.oracle_price[t.asset] = t.price;
                        else if (t.asset == "ETH") cstate.oracle_price[t.asset] = t.price;
                    }

                    // Find windows needing oracle seeding
                    using namespace std::chrono;
                    std::int64_t now_ms = duration_cast<milliseconds>(
                        system_clock::now().time_since_epoch()).count();
                    std::vector<std::pair<std::int64_t, std::string>> windows_to_seed;
                    {
                        std::lock_guard<std::mutex> lk(cstate.mu);
                        for (const auto& m : cstate.markets) {
                            if (m.asset != t.asset) continue;
                            if (m.window_open_ms <= 0 || m.window_open_ms > now_ms) continue;
                            if (m.window_close_ms <= now_ms) continue;
                            if (!oracle_seeded_windows.count({t.asset, m.window_open_ms}))
                                windows_to_seed.push_back({m.window_open_ms, m.condition_id});
                        }
                    }

                    // Seed oracle buffer using the current Chainlink RTDS price.
                    // Consistent single-source approach — no cross-contamination from Binance.
                    for (const auto& [open_ms, cid] : windows_to_seed) {
                        double open_ts_s = (double)open_ms / 1000.0;
                        if (oracle_buf.lookup_at_or_before(t.asset, open_ts_s) > 0.0) {
                            oracle_seeded_windows.insert({t.asset, open_ms});
                            continue;
                        }
                        oracle_buf.add(t.asset, open_ts_s, t.price);
                        oracle_seeded_windows.insert({t.asset, open_ms});
                        if (cfg.data_collection.window_events_enabled) {
                            std::string seed_payload = fmt::format(
                                R"({{"price":{},"ts_ms":{},"source":"chainlink_rtds"}})",
                                t.price, open_ms);
                            persist_q.push([pg, cid,
                                            p=std::move(seed_payload)]() noexcept {
                                pg->insert_window_event(cid, "first_oracle_seed", p);
                            });
                        }
                    }

                    // Append real-time tick
                    oracle_buf.add(t.asset, (double)t.ts_ms / 1000.0, t.price);

                    // Persist oracle tick
                    if (cfg.data_collection.oracle_ticks_enabled) {
                        persist_q.push([pg, asset=t.asset, price=t.price,
                                        ts_ms=t.ts_ms]() noexcept {
                            pg->insert_oracle_tick(asset, price, ts_ms);
                        });
                    }
                });
            rtds->start();
        }
    });

    // Coinbase REST poller — pulls BTC-USD / ETH-USD spot every 2s
    std::thread coinbase_th([&] {
        while (!g_stop.load()) {
            for (auto sym : {"BTC", "ETH"}) {
                std::string url = std::string("https://api.coinbase.com/v2/prices/")
                                + sym + "-USD/spot";
                auto r = vp::http::get(url, {}, 4000);
                if (r.status == 200 && !r.body.empty()) {
                    auto p = r.body.find("\"amount\":\"");
                    if (p != std::string::npos) {
                        p += 10;
                        auto q = r.body.find('"', p);
                        if (q != std::string::npos) {
                            try {
                                double px = std::stod(r.body.substr(p, q - p));
                                int64_t ts_ms = vp::Clock::wall_ns() / 1'000'000LL;
                                {
                                    std::lock_guard<std::mutex> lk(cstate.mu);
                                    if (std::string(sym) == "BTC") cstate.cb_asset_price[sym] = px;
                                    else if (std::string(sym) == "ETH") cstate.cb_asset_price[sym] = px;
                                }
                                if (cfg.data_collection.coinbase_ticks_enabled) {
                                    persist_q.push([pg=pg.get(), asset=std::string(sym),
                                                    price=px, ts_ms]() noexcept {
                                        pg->insert_coinbase_tick(asset, price, ts_ms);
                                    });
                                }
                            } catch (...) {}
                        }
                    }
                }
            }
            for (int i = 0; i < 20 && !g_stop.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    // Refresh thread: feature computation + periodic market re-discovery
    std::thread refresh_th([&] {
        std::int64_t last_feature_ms = 0;
        std::int64_t last_discover_ms = vp::Clock::wall_ns() / 1'000'000LL;
        constexpr std::int64_t kDiscoverIntervalMs = 3 * 60 * 1000LL; // 3 min

        while (!g_stop.load()) {
            std::int64_t now_ms = vp::Clock::wall_ns() / 1'000'000LL;

            // ── Feature snapshots (predictor output) ──
            if (cfg.data_collection.features_enabled &&
                (now_ms - last_feature_ms) >= cfg.data_collection.features_write_interval_ms) {
                last_feature_ms = now_ms;

                std::vector<vp::PmMarket> markets_snap;
                std::unordered_map<std::string, double> asset_price_snap;
                std::unordered_map<std::string, double> oracle_price_snap;
                std::unordered_map<std::string, double> cb_asset_price_snap;
                {
                    std::lock_guard<std::mutex> lk(cstate.mu);
                    markets_snap = cstate.markets;
                    asset_price_snap = cstate.asset_price;
                    oracle_price_snap = cstate.oracle_price;
                    cb_asset_price_snap = cstate.cb_asset_price;
                }

                for (const auto& m : markets_snap) {
                    if (m.window_open_ms <= 0 || m.window_open_ms > now_ms) continue;
                    double asset_price = asset_price_snap[m.asset];
                    if (asset_price <= 0) continue;

                    double window_open_ts_s = (double)m.window_open_ms / 1000.0;
                    double oracle_now   = oracle_price_snap[m.asset];
                    double oracle_strike = oracle_buf.lookup_at_or_before(m.asset, window_open_ts_s);

                    vp::engine::PredictorInput pin;
                    pin.asset       = m.asset;
                    pin.horizon_s   = m.horizon_seconds;
                    pin.secs_left   = (int)((m.window_close_ms - now_ms) / 1000);
                    pin.asset_price = asset_price;
                    pin.strike      = oracle_strike > 0 ? oracle_strike : m.target_price;
                    pin.coinbase_price = cb_asset_price_snap[m.asset];
                    pin.closes_1m = candle_store.copy_recent_closes(
                        m.asset, vp::engine::Timeframe::OneMinute, kPredictorRecent1mBars);
                    pin.bars_1m = candle_store.copy_recent_bars(
                        m.asset, vp::engine::Timeframe::OneMinute, kPredictorRecent1mOhlcvBars);
                    pin.closes_5m = candle_store.copy_recent_closes(
                        m.asset, vp::engine::Timeframe::FiveMinutes, 288);
                    pin.closes_15m = candle_store.copy_recent_closes(
                        m.asset, vp::engine::Timeframe::FifteenMinutes, 96);
                    pin.closes_60m = candle_store.copy_recent_closes(
                        m.asset, vp::engine::Timeframe::SixtyMinutes, kPredictorLong60mBars);
                    pin.bars_5m = candle_store.copy_recent_bars(
                        m.asset, vp::engine::Timeframe::FiveMinutes, 30);
                    pin.bars_15m = candle_store.copy_recent_bars(
                        m.asset, vp::engine::Timeframe::FifteenMinutes, 24);
                    pin.bars_60m = candle_store.copy_recent_bars(
                        m.asset, vp::engine::Timeframe::SixtyMinutes, 24);

                    auto pred = vp::engine::predict(pin);

                    // Serialize to JSON and persist (sanitize NaN/Inf to 0)
                    auto sanitize = [](double v) -> double {
                        return (std::isnan(v) || std::isinf(v)) ? 0.0 : v;
                    };
                    std::string json = fmt::format(
                        R"({{"pred_up":{},"fv_up":{},"edge_up":{},"edge_down":{},)"
                        R"("conf":{},"rsi14":{},"macd_hist":{},"bb_z":{},)"
                        R"("tech_score":{},"atr_pct":{},"oracle_delta_pct":{},)"
                        R"("rv_1h":{},"rv_1d":{},"rv_30d":{},"rv_90d":{},"rv_1y":{},)"
                        R"("slope_7d_pct":{},"slope_30d_pct":{},"slope_1y_pct":{},)"
                        R"("range_pos_30d":{},"range_pos_1y":{},"history_regime_score":{},)"
                        R"("asset_price":{}}})",
                        sanitize(pred.prediction), sanitize(pred.fair_value_up),
                        sanitize(pred.edge_up), sanitize(pred.edge_down),
                        sanitize(pred.confidence),
                        sanitize(pred.parts.rsi14), sanitize(pred.parts.macd_hist),
                        sanitize(pred.parts.bb_z),
                        sanitize(pred.parts.tech_score), sanitize(pred.parts.atr_pct),
                        sanitize(pred.parts.oracle_delta_pct),
                        sanitize(pred.parts.rv_1h), sanitize(pred.parts.rv_1d),
                        sanitize(pred.parts.rv_30d), sanitize(pred.parts.rv_90d),
                        sanitize(pred.parts.rv_1y),
                        sanitize(pred.parts.slope_7d_pct),
                        sanitize(pred.parts.slope_30d_pct),
                        sanitize(pred.parts.slope_1y_pct),
                        sanitize(pred.parts.range_pos_30d),
                        sanitize(pred.parts.range_pos_1y),
                        sanitize(pred.parts.history_regime_score),
                        sanitize(asset_price));
                    persist_q.push([pg=pg.get(), cid=m.condition_id,
                                    j=std::move(json)]() noexcept {
                        pg->insert_feature(cid, j, 1);
                    });
                }
            }

            // ── Periodic market re-discovery ──
            if (now_ms - last_discover_ms >= kDiscoverIntervalMs) {
                last_discover_ms = now_ms;
                auto new_markets = rest.discover_updown_markets();
                std::vector<vp::PmMarket> added;
                std::vector<std::string> new_tokens;
                {
                    std::lock_guard<std::mutex> lk(cstate.mu);
                    std::unordered_set<std::string> known;
                    known.reserve(cstate.markets.size());
                    for (const auto& m : cstate.markets)
                        known.insert(m.condition_id);
                    for (auto& m : new_markets) {
                        if (known.count(m.condition_id)) continue;
                        new_tokens.push_back(m.yes_token_id);
                        new_tokens.push_back(m.no_token_id);
                        cstate.token_to_cond[m.yes_token_id] = m.condition_id;
                        cstate.token_to_cond[m.no_token_id]  = m.condition_id;
                        cstate.token_to_side[m.yes_token_id] = vp::ShareSide::UP;
                        cstate.token_to_side[m.no_token_id]  = vp::ShareSide::DOWN;
                        added.push_back(m);
                        cstate.markets.push_back(std::move(m));
                    }
                }
                if (!added.empty()) {
                    if (pm_md) pm_md->subscribe(new_tokens);
                    spdlog::info("market refresh: +{} new markets", added.size());
                    for (const auto& m : added) {
                        persist_q.push([pg=pg.get(), m]() noexcept {
                            pg->upsert_market(m.condition_id, m.asset, m.horizon_seconds,
                                              m.target_price, m.window_open_ms, m.window_close_ms,
                                              m.yes_token_id, m.no_token_id, m.question, m.slug,
                                              m.settlement_window_s, m.reference_is_binance);
                        });
                        if (cfg.data_collection.window_events_enabled && m.window_open_ms > 0) {
                            persist_q.push([pg=pg.get(), cid=m.condition_id]() noexcept {
                                pg->insert_window_event(cid, "opened", "");
                            });
                        }
                    }
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(
                cfg.data_collection.features_enabled
                    ? cfg.data_collection.features_write_interval_ms
                    : 1000));
        }
    });

    // Wait for shutdown signal
    spdlog::info("collector running — waiting for shutdown signal...");
    while (!g_stop.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Clean shutdown
    spdlog::info("shutdown: stopping feeds...");
    persist_q.shutdown();
    if (bootstrap_th.joinable()) bootstrap_th.join();
    refresh_th.join();
    coinbase_th.join();
    persist_th.join();
    if (binance) binance->stop();
    if (pm_md)   pm_md->stop();
    if (rtds)    rtds->stop();

    spdlog::info("clean shutdown");
    return 0;
}
