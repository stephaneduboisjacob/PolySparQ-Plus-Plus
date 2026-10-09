// Coinbase Exchange websocket ticker adapter.
// URL: wss://ws-feed.exchange.coinbase.com
// Subscribe: {"type":"subscribe","product_ids":["BTC-USD",...],
//             "channels":["ticker","heartbeat"]}
// Ticker: {"type":"ticker","product_id":"BTC-USD","price":"61000.01",
//          "time":"2026-06-10T08:00:00.123456Z","last_size":"0.001",...}
// The heartbeat channel keeps the socket non-idle on quiet products; ticker
// frames carry the price. No client ping is required by this endpoint.
#include "coinbase_ws.hpp"
#include "core/clock.hpp"
#include "net/ws_client.hpp"
#include <simdjson.h>
#include <spdlog/spdlog.h>
#include <cstdio>
#include <ctime>

namespace vp {
namespace {

double sv_to_double(std::string_view s) {
    if (s.empty()) return 0.0;
    try { return std::stod(std::string(s)); } catch (...) { return 0.0; }
}

// "2026-06-10T08:00:00.123456Z" -> epoch milliseconds (0 on parse failure).
int64_t iso8601_to_ms(std::string_view sv) {
    if (sv.size() < 19) return 0;
    std::tm tm{};
    int y, mo, d, h, mi;
    double sec = 0;
    if (std::sscanf(std::string(sv).c_str(), "%d-%d-%dT%d:%d:%lf",
                    &y, &mo, &d, &h, &mi, &sec) != 6) return 0;
    tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
    tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = 0;
    time_t base = timegm(&tm);
    if (base < 0) return 0;
    return (int64_t)base * 1000 + (int64_t)(sec * 1000.0);
}

} // namespace

CoinbaseWsFeed::CoinbaseWsFeed(std::string ws_url,
                               std::vector<std::string> product_ids,
                               OnTick on_tick)
    : ws_url_(std::move(ws_url)), product_ids_(std::move(product_ids)),
      on_tick_(std::move(on_tick)) {
    spdlog::info("coinbase ws feed: {}", ws_url_);
    ws_ = std::make_unique<WsClient>(
        ws_url_,
        [this](std::string_view frame) {
            thread_local simdjson::dom::parser parser;
            simdjson::padded_string padded(frame.data(), frame.size());
            simdjson::dom::element doc;
            if (parser.parse(padded).get(doc)) return;
            std::string_view type;
            if (doc["type"].get(type)) return;
            if (type == "error") {
                std::string_view msg, reason;
                if (doc["message"].get(msg)) msg = "";
                if (doc["reason"].get(reason)) reason = "";
                spdlog::warn("coinbase ws error: {} {}", msg, reason);
                return;
            }
            if (type != "ticker") return;  // heartbeat/subscriptions ignored
            AssetTick t;
            std::string_view sv;
            if (doc["product_id"].get(sv) == simdjson::SUCCESS) t.symbol = std::string(sv);
            if (doc["price"].get(sv) == simdjson::SUCCESS)      t.price  = sv_to_double(sv);
            if (doc["last_size"].get(sv) == simdjson::SUCCESS)  t.qty    = sv_to_double(sv);
            if (doc["time"].get(sv) == simdjson::SUCCESS)       t.exchange_ms = iso8601_to_ms(sv);
            if (t.exchange_ms <= 0) t.exchange_ms = Clock::wall_ns() / 1'000'000LL;
            t.recv_mono_ns = Clock::mono_ns();
            if (on_tick_ && t.price > 0 && !t.symbol.empty()) on_tick_(t);
        },
        [this](const std::string& state) {
            spdlog::info("coinbase ws state: {}", state);
            if (state == "connected") send_subscribe();
        });
}

CoinbaseWsFeed::~CoinbaseWsFeed() { stop(); }

void CoinbaseWsFeed::start() { if (ws_) ws_->start(); }
void CoinbaseWsFeed::stop()  { if (ws_) ws_->stop(); }

void CoinbaseWsFeed::send_subscribe() {
    if (!ws_) return;
    std::string msg = R"({"type":"subscribe","product_ids":[)";
    for (size_t i = 0; i < product_ids_.size(); ++i) {
        if (i) msg += ',';
        msg += '"'; msg += product_ids_[i]; msg += '"';
    }
    msg += R"(],"channels":["ticker","heartbeat"]})";
    ws_->send(std::move(msg));
}

} // namespace vp
