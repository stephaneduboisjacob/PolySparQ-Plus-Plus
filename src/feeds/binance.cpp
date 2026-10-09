// Binance combined stream adapter (trade + bookTicker).
// URL: wss://stream.binance.com:9443/stream?streams=btcusdt@trade/ethusdt@trade
// Frame (trade): {"stream":"<sym>@trade","data":{"e":"trade","E":<epoch_ms>,"s":"BTCUSDT",
//                                        "p":"<px>","q":"<qty>","T":<trade_ms>,"m":<bool>}}
// Frame (bookTicker): {"stream":"<sym>@bookTicker","data":{"u":<id>,"s":"BTCUSDT",
//                        "b":"<bidPx>","B":"<bidQty>","a":"<askPx>","A":"<askQty>"}}
//   Note: bookTicker may carry no "e" field (older payloads) — detect by
//   presence of b/a instead of relying on the event type alone.
#include "binance.hpp"
#include "core/clock.hpp"
#include "net/ws_client.hpp"
#include <simdjson.h>
#include <spdlog/spdlog.h>
namespace vp {
namespace {
double sv_to_double(std::string_view s) {
    if (s.empty()) return 0.0;
    try { return std::stod(std::string(s)); } catch (...) { return 0.0; }
}
} // namespace
BinanceFeed::BinanceFeed(std::string ws_url,
                         std::vector<std::string> streams,
                         OnTick on_tick)
    : ws_url_(std::move(ws_url)), streams_(std::move(streams)),
      on_tick_(std::move(on_tick)) {
    std::string full_url = ws_url_;
    if (full_url.find("streams=") == std::string::npos && !streams_.empty()) {
        full_url += (full_url.find('?') == std::string::npos) ? "?streams=" : "&streams=";
        for (size_t i = 0; i < streams_.size(); ++i) {
            if (i) full_url += '/';
            full_url += streams_[i];
        }
    }
    spdlog::info("binance feed: {}", full_url);
    ws_ = std::make_unique<WsClient>(
        full_url,
        [this](std::string_view frame) {
            thread_local simdjson::dom::parser parser;
            simdjson::padded_string padded(frame.data(), frame.size());
            simdjson::dom::element doc;
            if (parser.parse(padded).get(doc)) return;
            // combined-stream wrapper has top-level "data"; if absent, use root.
            simdjson::dom::element data = doc;
            simdjson::dom::element wrapped;
            if (doc["data"].get(wrapped) == simdjson::SUCCESS) data = wrapped;
            std::string_view e_type;
            bool has_e = (data["e"].get(e_type) == simdjson::SUCCESS);
            if (has_e && e_type == "forceOrder") {
                // ── Futures forceOrder (Phase 4.1): liquidation event ──
                // data.o: {"s":sym,"S":"SELL|BUY","q":qty,"p":px,"T":ts}
                simdjson::dom::element o;
                if (data["o"].get(o) == simdjson::SUCCESS) {
                    AssetTick t;
                    std::string_view sv, side;
                    if (o["s"].get(sv) == simdjson::SUCCESS) t.symbol = std::string(sv);
                    if (o["p"].get(sv) == simdjson::SUCCESS) t.price  = sv_to_double(sv);
                    if (o["q"].get(sv) == simdjson::SUCCESS) t.qty    = sv_to_double(sv);
                    (void)o["S"].get(side);
                    // Forced SELL liquidation = sell aggressor (buyer_is_maker=false)
                    // → pushes price DOWN. Forced BUY = opposite.
                    t.buyer_is_maker = (side == "BUY");
                    int64_t ts = 0; (void)o["T"].get(ts);
                    t.exchange_ms = ts;
                    t.is_force_order = true;
                    t.recv_mono_ns = Clock::mono_ns();
                    if (on_tick_ && t.price > 0) on_tick_(t);
                }
                return;
            }
            if (has_e && e_type == "trade") {
                AssetTick t;
                std::string_view sv;
                if (data["s"].get(sv) == simdjson::SUCCESS) t.symbol = std::string(sv);
                if (data["p"].get(sv) == simdjson::SUCCESS) t.price  = sv_to_double(sv);
                if (data["q"].get(sv) == simdjson::SUCCESS) t.qty    = sv_to_double(sv);
                int64_t ts = 0; (void)data["T"].get(ts);
                t.exchange_ms = ts;
                bool m = false; (void)data["m"].get(m);
                t.buyer_is_maker = m;
                t.recv_mono_ns = Clock::mono_ns();
                if (on_tick_ && t.price > 0) on_tick_(t);
                return;
            }

            // ── bookTicker (Phase 6.3): sub-100ms best bid/ask ──
            // Detect by fields (payloads without "e") or by e=="bookTicker".
            std::string_view bs, bq, as_, aq, sym;
            bool has_quote = (data["b"].get(bs) == simdjson::SUCCESS &&
                              data["B"].get(bq) == simdjson::SUCCESS &&
                              data["a"].get(as_) == simdjson::SUCCESS &&
                              data["A"].get(aq) == simdjson::SUCCESS &&
                              data["s"].get(sym) == simdjson::SUCCESS);
            if ((has_e && e_type != "bookTicker") || !has_quote) return;
            AssetTick t;
            t.symbol = std::string(sym);
            t.is_book_ticker = true;
            t.bid_price = sv_to_double(bs);
            t.bid_qty   = sv_to_double(bq);
            t.ask_price = sv_to_double(as_);
            t.ask_qty   = sv_to_double(aq);
            // price = mid (convenience for generic consumers; book consumers
            // read the bid/ask fields directly).
            t.price = (t.bid_price > 0 && t.ask_price > 0)
                ? 0.5 * (t.bid_price + t.ask_price) : 0.0;
            int64_t ts = 0; (void)data["E"].get(ts);
            if (ts <= 0) (void)data["T"].get(ts);
            t.exchange_ms = ts;
            t.recv_mono_ns = Clock::mono_ns();
            if (on_tick_ && t.price > 0) on_tick_(t);
        },
        [](const std::string& state) {
            spdlog::info("binance ws state: {}", state);
        });
}
BinanceFeed::~BinanceFeed() { stop(); }
void BinanceFeed::start() { if (ws_) ws_->start(); }
void BinanceFeed::stop()  { if (ws_) ws_->stop(); }
} // namespace vp
