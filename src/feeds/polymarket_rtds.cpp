// Polymarket RTDS oracle feed — Chainlink prices via Polymarket relay.
// URL: wss://ws-live-data.polymarket.com
// TWAP subscriptions require one exact compact symbol filter in practice.
// The initial response is a payload.data history snapshot; later frames are
// scalar updates.  Both contain official Chainlink-computed TWAP values.
// Ping: plain text "PING" every 5s (RTDS drops connection otherwise).
// Message: {"topic":"crypto_prices_chainlink","payload":{"symbol":"btc/usd","value":94512.34,"timestamp":1750000000000}}
#include "polymarket_rtds.hpp"
#include "net/ws_client.hpp"
#include <simdjson.h>
#include <spdlog/spdlog.h>
#include <chrono>
#include <cmath>
#include <thread>

namespace vp {
namespace {

[[maybe_unused]] const char* kSubscribeMsg =
    R"({"action":"subscribe","subscriptions":[{"topic":"crypto_prices_chainlink","type":"*","filters":""},{"topic":"crypto_prices_twap_thirty","type":"update","filters":"{\"symbol\":\"btc/usd\"}"},{"topic":"crypto_prices_twap_thirty","type":"update","filters":"{\"symbol\":\"eth/usd\"}"},{"topic":"crypto_prices_twap_thirty","type":"update","filters":"{\"symbol\":\"sol/usd\"}"},{"topic":"crypto_prices_twap_thirty","type":"update","filters":"{\"symbol\":\"xrp/usd\"}"},{"topic":"crypto_prices_twap_sixty","type":"update","filters":"{\"symbol\":\"btc/usd\"}"},{"topic":"crypto_prices_twap_sixty","type":"update","filters":"{\"symbol\":\"eth/usd\"}"},{"topic":"crypto_prices_twap_sixty","type":"update","filters":"{\"symbol\":\"sol/usd\"}"},{"topic":"crypto_prices_twap_sixty","type":"update","filters":"{\"symbol\":\"xrp/usd\"}"}]})";

std::string symbol_to_asset(std::string_view sym) {
    // sym comes as "btc/usd", "eth/usd", "sol/usd", "xrp/usd" etc.
    if (sym.find("btc") != std::string_view::npos) return "BTC";
    if (sym.find("eth") != std::string_view::npos) return "ETH";
    if (sym.find("sol") != std::string_view::npos) return "SOL";
    if (sym.find("xrp") != std::string_view::npos) return "XRP";
    return {};
}

} // namespace

PolymarketRtds::PolymarketRtds(std::string ws_url, OnTick on_tick)
    : url_(std::move(ws_url)), on_tick_(std::move(on_tick))
{
    ws_ = std::make_unique<WsClient>(
        url_,
        [this](std::string_view frame) {
            // Ignore plain-text PONG/heartbeat frames
            if (frame.empty() || frame[0] != '{') {
                spdlog::debug("rtds skip frame: empty or not JSON (first_byte='{}')",
                    frame.empty() ? '?' : frame[0]);
                return;
            }

            thread_local simdjson::dom::parser parser;
            simdjson::padded_string padded(frame.data(), frame.size());
            simdjson::dom::element doc;
            if (parser.parse(padded).get(doc) != simdjson::SUCCESS) {
                spdlog::debug("rtds parse fail: {}", frame.substr(0, 80));
                return;
            }

            std::string_view topic;
            if (doc["topic"].get(topic) != simdjson::SUCCESS) {
                spdlog::debug("rtds no topic field in: {}", frame.substr(0, 80));
                return;
            }
            if (topic != "crypto_prices_chainlink" && topic != "crypto_prices_twap_thirty" && topic != "crypto_prices_twap_sixty") {
                spdlog::debug("rtds topic='{}' (skip)", topic);
                return;
            }

            simdjson::dom::element payload;
            if (doc["payload"].get(payload) != simdjson::SUCCESS) {
                spdlog::debug("rtds no payload in chainlink msg");
                return;
            }

            std::string_view sym;
            if (payload["symbol"].get(sym) != simdjson::SUCCESS) {
                spdlog::debug("rtds no symbol in payload");
                return;
            }
            std::string asset = symbol_to_asset(sym);
            if (asset.empty()) return;

            int64_t payload_window_s = 0;
            [[maybe_unused]] auto window_err = payload["window_s"].get(payload_window_s);
            const int window_s = payload_window_s > 0 ? static_cast<int>(payload_window_s) :
                                 topic == "crypto_prices_twap_thirty" ? 30 :
                                 topic == "crypto_prices_twap_sixty" ? 60 : 0;

            auto emit = [this, &asset, window_s](simdjson::dom::element point) {
                double value = 0;
                int64_t ts_ms = 0;
                if (point["value"].get(value) != simdjson::SUCCESS ||
                    point["timestamp"].get(ts_ms) != simdjson::SUCCESS)
                    return;
                // Observation time is mandatory. Receipt time must never make
                // an old or malformed reference look fresh.
                if (ts_ms <= 0 || !std::isfinite(value) || value <= 0) return;
                OracleTick t{asset, value, ts_ms, window_s};
                spdlog::debug("rtds oracle {}{} ${:.2f} ts={}", asset,
                              window_s > 0 ? ":TWAP" + std::to_string(window_s) : "",
                              value, ts_ms);
                if (on_tick_) on_tick_(t);
            };

            // Subscription acknowledgement contains recent official values as
            // payload.data.  Ingesting their original observation timestamps
            // provides the opening reference after a process reconnect without
            // inventing or backdating a price.
            simdjson::dom::array history;
            if (payload["data"].get(history) == simdjson::SUCCESS) {
                for (simdjson::dom::element point : history) emit(point);
                return;
            }
            emit(payload);
        },
        [this](const std::string& state) {
            spdlog::info("rtds ws state: {}", state);
            if (state == "connected") {
                send_subscribe();
            }
        }
    );
}

PolymarketRtds::~PolymarketRtds() { stop(); }

void PolymarketRtds::start() {
    running_ = true;
    ws_->start();
    ping_th_ = std::make_unique<std::thread>([this]{ ping_loop(); });
}

void PolymarketRtds::stop() {
    running_ = false;
    ping_cv_.notify_all();
    if (ping_th_ && ping_th_->joinable()) ping_th_->join();
    ping_th_.reset();
    if (ws_) ws_->stop();
}

void PolymarketRtds::send_subscribe() {
    if (!ws_) return;
    // RTDS currently fails a mixed multi-topic AddSubscriptions request with
    // a server-side __subscriptions error. Send one subscription per request.
    // Unfiltered TWAP topics are intentional: they stream every supported
    // symbol, whereas multiple filters on one topic leave only the final
    // symbol receiving ongoing updates.
    ws_->send(R"({"action":"subscribe","subscriptions":[{"topic":"crypto_prices_chainlink","type":"*","filters":""}]})");
    ws_->send(R"({"action":"subscribe","subscriptions":[{"topic":"crypto_prices_twap_thirty","type":"update","filters":""}]})");
    ws_->send(R"({"action":"subscribe","subscriptions":[{"topic":"crypto_prices_twap_sixty","type":"update","filters":""}]})");
}

void PolymarketRtds::ping_loop() {
    using namespace std::chrono_literals;
    while (running_) {
        std::unique_lock<std::mutex> lk(ping_mu_);
        if (ping_cv_.wait_for(lk, 5s, [&] { return !running_.load(); })) break;
        lk.unlock();
        try { if (ws_) ws_->send("PING"); }
        catch (...) {}
    }
}

} // namespace vp
