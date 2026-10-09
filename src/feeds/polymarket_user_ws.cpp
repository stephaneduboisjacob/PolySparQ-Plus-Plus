// Polymarket user WebSocket — receives fill/order updates for the authenticated user.
// URL: wss://ws-subscriptions-clob.polymarket.com/ws/user
// Auth: {"type":"user","apiKey":"...","secret":"...","passphrase":"...","timestamp":"...","signature":"..."}
// Keepalive: PING text under the documented 10s cadence.
#include "polymarket_user_ws.hpp"
#include "net/ws_client.hpp"
#include "sign/hmac_auth.hpp"
#include <simdjson.h>
#include <spdlog/spdlog.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace vp {

namespace {
double sv_to_d(std::string_view s) {
    if (s.empty()) return 0;
    try { return std::stod(std::string(s)); } catch (...) { return 0; }
}
} // namespace

struct UserWsState {
    std::atomic<bool> running{false};
    std::atomic<bool> connected{false};
    std::mutex wait_mu;
    std::condition_variable wait_cv;
    std::thread       keepalive_th;
};

PolymarketUserWs::PolymarketUserWs(
    std::string url,
    std::string api_key, std::string api_secret, std::string api_passphrase,
    std::string address, OnFill on_fill)
    : url_(std::move(url)),
      api_key_(std::move(api_key)), api_secret_(std::move(api_secret)),
      api_passphrase_(std::move(api_passphrase)), address_(std::move(address)),
      on_fill_(std::move(on_fill)) {

    auto st = std::make_shared<UserWsState>();
    state_ = std::static_pointer_cast<void>(st);

    auto on_frame = [this](std::string_view frame) {
        if (frame.empty() || frame == "PONG" || frame == "pong" || frame == "{}") return;
        thread_local simdjson::dom::parser parser;
        simdjson::padded_string padded(frame.data(), frame.size());
        simdjson::dom::element doc;
        if (parser.parse(padded).get(doc)) return;

        auto handle_one = [this](simdjson::dom::element ev) {
            std::string_view et;
            if (ev["event_type"].get(et)) return;
            if (et != "order" && et != "trade") return;

            // We only care about fills (trade events with type == "MATCH")
            if (et == "trade") {
                std::string_view type;
                (void)ev["type"].get(type);
                if (type != "MATCH" && type != "match") return;

                FillUpdate f;
                std::string_view sv;
                if (!ev["id"].get(sv))       f.order_id = std::string(sv);
                if (!ev["asset_id"].get(sv)) f.token_id  = std::string(sv);
                f.price = sv_to_d(ev["price"].get_string().value_unsafe());
                f.size  = sv_to_d(ev["size"].get_string().value_unsafe());
                int64_t ts = 0; (void)ev["match_time"].get(ts);
                f.exchange_ms = ts;
                if (on_fill_) on_fill_(f);
            }
        };

        simdjson::dom::array arr;
        if (!doc.get_array().get(arr)) { for (auto e : arr) handle_one(e); }
        else                           { handle_one(doc); }
    };

    auto on_state = [this, st](const std::string& state) {
        spdlog::info("pm-user-ws state: {}", state);
        if (state == "connected") {
            st->connected.store(true);
            // Polymarket CLOB user channel auth: the L2 credential tuple goes
            // NESTED under "auth" (apiKey/secret/passphrase IS the auth — no
            // timestamp/signature needed). The previous flat format was rejected
            // by the server, which closed the socket ~100ms after every connect.
            std::string msg = fmt::format(
                R"({{"auth":{{"apiKey":"{}","secret":"{}","passphrase":"{}"}},"type":"user","markets":[]}})",
                api_key_, api_secret_, api_passphrase_);
            spdlog::debug("pm-user-ws: sending auth frame for {}", address_);
            if (ws_) ws_->send(std::move(msg));
        } else {
            st->connected.store(false);
        }
    };

    ws_ = std::make_unique<WsClient>(url_, std::move(on_frame), std::move(on_state));
}

PolymarketUserWs::~PolymarketUserWs() { stop(); }

void PolymarketUserWs::start() {
    auto st = std::static_pointer_cast<UserWsState>(state_);
    if (st && st->running.exchange(true)) return;
    if (ws_) ws_->start();
    if (st) {
        st->keepalive_th = std::thread([this, st] {
            using namespace std::chrono_literals;
            while (st->running.load()) {
                std::unique_lock<std::mutex> lk(st->wait_mu);
                if (st->wait_cv.wait_for(lk, 8s,
                        [&] { return !st->running.load(); })) break;
                lk.unlock();
                try { if (ws_) ws_->send("PING"); }
                catch (...) {}
            }
        });
    }
}

void PolymarketUserWs::stop() {
    auto st = std::static_pointer_cast<UserWsState>(state_);
    if (st) {
        st->running.store(false);
        st->wait_cv.notify_all();
        if (st->keepalive_th.joinable()) st->keepalive_th.join();
    }
    if (ws_) ws_->stop();
}

} // namespace vp
