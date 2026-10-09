// Polymarket CLOB market-data WebSocket adapter.
// URL : wss://ws-subscriptions-clob.polymarket.com/ws/market
// Subscribe: {"type":"market","assets_ids":["<tok>",...]}
// Keepalive: server closes idle connections; send literal "PING"
//            text frame under the documented 10s cadence.
// Reconnect: WsClient handles reconnect; we re-send the subscribe on each
//            "connected" state so we don't go silent after a disconnect.
#include "polymarket_md.hpp"
#include "net/ws_client.hpp"
#include <simdjson.h>
#include <spdlog/spdlog.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <cmath>
#include <cstdio>
namespace vp {
namespace {
constexpr std::size_t kMaxMarketFrameBytes = 8 * 1024 * 1024;

double sv_to_double(std::string_view s) {
    if (s.empty()) return 0.0;
    try { double v = std::stod(std::string(s)); return std::isfinite(v) ? v : 0.0; } catch (...) { return 0.0; }
}
int64_t sv_to_i64(std::string_view s) {
    if (s.empty()) return 0;
    try { return std::stoll(std::string(s)); } catch (...) { return 0; }
}
double field_as_double(simdjson::simdjson_result<simdjson::dom::element> r) {
    simdjson::dom::element e;
    if (r.get(e) != simdjson::SUCCESS) return 0.0;
    std::string_view sv;
    if (!e.get(sv)) return sv_to_double(sv);
    double d = 0;
    if (!e.get(d)) return std::isfinite(d) ? d : 0.0;
    int64_t i = 0;
    if (!e.get(i)) return double(i);
    return 0.0;
}
int64_t field_as_i64(simdjson::simdjson_result<simdjson::dom::element> r) {
    simdjson::dom::element e;
    if (r.get(e) != simdjson::SUCCESS) return 0;
    std::string_view sv;
    if (!e.get(sv)) return sv_to_i64(sv);
    int64_t i = 0;
    if (!e.get(i)) return i;
    uint64_t u = 0;
    if (!e.get(u)) return static_cast<int64_t>(u);
    double d = 0;
    if (!e.get(d)) return static_cast<int64_t>(d);
    return 0;
}
bool is_nonempty_array(simdjson::simdjson_result<simdjson::dom::element> r) {
    simdjson::dom::element e;
    if (r.get(e) != simdjson::SUCCESS) return false;
    simdjson::dom::array arr;
    if (e.get(arr) != simdjson::SUCCESS) return false;
    return arr.size() > 0;
}
std::string extract_asset_id(simdjson::simdjson_result<simdjson::dom::element> r) {
    simdjson::dom::element e;
    if (r.get(e) != simdjson::SUCCESS) return {};
    std::string_view sv;
    if (e.get(sv) == simdjson::SUCCESS) return std::string(sv);
    uint64_t u64 = 0;
    if (e.get(u64) == simdjson::SUCCESS) return std::to_string(u64);
    int64_t i64 = 0;
    if (e.get(i64) == simdjson::SUCCESS) return std::to_string(i64);
    return {};
}

bool side_is_buy(std::string_view side, bool& buy) {
    if (side == "BUY" || side == "buy") {
        buy = true;
        return true;
    }
    if (side == "SELL" || side == "sell") {
        buy = false;
        return true;
    }
    return false;
}

bool read_side(simdjson::simdjson_result<simdjson::dom::element> r, bool& buy) {
    simdjson::dom::element e;
    if (r.get(e) != simdjson::SUCCESS) return false;
    std::string_view side;
    if (e.get(side) != simdjson::SUCCESS) return false;
    return side_is_buy(side, buy);
}
} // namespace
struct PmMdState {
    std::mutex                mu;
    std::vector<std::string>  tokens;        // last requested token list
    std::atomic<bool>         running{false};
    std::atomic<bool>         connected{false};
    std::mutex                wait_mu;
    std::condition_variable   wait_cv;
    std::thread               keepalive_th;
    int                       keepalive_counter{0};  // per-instance PING counter
    std::atomic<uint64_t>     frames{0};   // total WS frames seen (diagnostic)
    std::atomic<uint64_t>     books{0};    // total book updates dispatched (diagnostic)
    // Event-type breakdown (diagnostic): tells "frames arrive but books stale"
    // apart by WHAT the frames are. snap/delta = book-producing; trade/ticksz =
    // benign; unknown = an event_type we don't handle (the smoking gun if high).
    std::atomic<uint64_t>     ev_snap{0}, ev_delta{0}, ev_trade{0}, ev_ticksz{0}, ev_bbo{0}, ev_unknown{0};
    std::atomic<uint64_t>     ev_parsefail{0};          // frames that failed JSON parse
    char                      last_unknown[64] = {0};   // last unhandled event_type seen
    char                      last_frame_sample[200] = {0};  // head of a recent raw frame
};
// Keep the state in a shared_ptr captured by lambdas so it outlives the
// PolymarketMD destructor's stop sequence ordering.
PolymarketMD::PolymarketMD(std::string url, OnBook onb, OnTrade ont, OnBbo onbbo,
                           OnState ons)
    : url_(std::move(url)), on_book_(std::move(onb)), on_trade_(std::move(ont)),
      on_bbo_(std::move(onbbo)), on_state_(std::move(ons)) {
    auto st = std::make_shared<PmMdState>();
    state_ = std::static_pointer_cast<void>(st);
    auto on_frame = [this, st](std::string_view frame) {
        // Server pong is {} — parsed as empty JSON object, falls through to the
        // no-event_type / no-bids path and returns silently. Also filter legacy PONG.
        if (frame.empty() || frame == "PONG" || frame == "pong") return;
        // Never run the concatenated-document recovery scanner over an arbitrary
        // mid-stream fragment. A reconnect used to retain partial receive-buffer
        // bytes, producing megabyte messages that began inside a JSON object and
        // consumed a full CPU core while the scanner misread nested objects.
        const auto first = frame.find_first_not_of(" \t\r\n");
        if (frame.size() > kMaxMarketFrameBytes ||
            first == std::string_view::npos ||
            (frame[first] != '{' && frame[first] != '[')) {
            const auto failures = st->ev_parsefail.fetch_add(1, std::memory_order_relaxed) + 1;
            if (failures == 1 || failures % 100 == 0) {
                spdlog::warn("pm-md dropped malformed/oversized frame: bytes={} first='{}' count={}",
                             frame.size(),
                             first == std::string_view::npos ? '?' : frame[first],
                             failures);
            }
            return;
        }
        uint64_t fn = st->frames.fetch_add(1, std::memory_order_relaxed);
        // DIAGNOSTIC: sample one raw frame's head every ~5000 frames so we can
        // SEE what "frames arrive but events=0" actually is (parse failures /
        // an unrecognised envelope). Cheap, throttled.
        if (fn % 5000 == 0) {
            std::snprintf(st->last_frame_sample, sizeof(st->last_frame_sample),
                          "%.*s", (int)std::min<size_t>(frame.size(), 180), frame.data());
        }
        thread_local simdjson::dom::parser parser;
        auto handle_one = [this, st](simdjson::dom::element ev) {
            // Check event_type first for proper dispatch.
            std::string_view et_sv;
            bool has_event_type = (ev["event_type"].get(et_sv) == simdjson::SUCCESS);
            std::string et;
            if (has_event_type) et = std::string(et_sv);
            // Snapshot: event_type == "book" (Polymarket CLOB), "market", or
            // "book_snapshot"; or missing event_type but has bids/asks (old format).
            bool is_snapshot = false;
            if (!has_event_type && (ev["bids"].error() == simdjson::SUCCESS || ev["asks"].error() == simdjson::SUCCESS)) {
                // No event_type but bids or asks key is present → snapshot (backward compat).
                is_snapshot = true;
            } else if (et == "book" || et == "market" || et == "book_snapshot") {
                is_snapshot = true;
            }
            // Snapshot path: full book replacement
            if (is_snapshot) {
                BookUpdate bu; bu.is_snapshot = true;
                std::string_view sv;
                if (ev["market"].get(sv) == simdjson::SUCCESS)   bu.condition_id = std::string(sv);
                bu.token_id = extract_asset_id(ev["asset_id"]);
                bu.exchange_ms = field_as_i64(ev["timestamp"]);
                if (bu.token_id.empty()) {
                    // No asset_id on polymarket snapshot events — skip silently
                    return;
                }
                simdjson::dom::array bids_arr, asks_arr;
                if (ev["bids"].get(bids_arr) == simdjson::SUCCESS) {
                    for (auto lvl : bids_arr) {
                        PriceLevel l;
                        l.price = field_as_double(lvl["price"]);
                        l.size  = field_as_double(lvl["size"]);
                        if (l.price > 0) bu.bids.push_back(l);
                    }
                }
                if (ev["asks"].get(asks_arr) == simdjson::SUCCESS) {
                    for (auto lvl : asks_arr) {
                        PriceLevel l;
                        l.price = field_as_double(lvl["price"]);
                        l.size  = field_as_double(lvl["size"]);
                        if (l.price > 0) bu.asks.push_back(l);
                    }
                }
                spdlog::debug("book snapshot: token={} bids={} asks={}", bu.token_id, bu.bids.size(), bu.asks.size());
                st->ev_snap.fetch_add(1, std::memory_order_relaxed);
                if (on_book_) on_book_(bu);
                return;
            }
            // Delta / trade path: must have event_type
            if (!has_event_type) return;
            if (et == "price_change") {
                std::string_view mkt_sv;
                bool has_mkt = (ev["market"].get(mkt_sv) == simdjson::SUCCESS);
                // CURRENT Polymarket format: top-level "price_changes" array; each
                // entry carries its OWN asset_id/price/size/side. A single message
                // can update BOTH tokens of a market, so group by asset_id and emit
                // one BookUpdate per token. (The old code read a top-level asset_id
                // that does not exist → token_id empty → every delta dropped → books
                // only ever got their initial snapshot → "all stale / never updated".)
                simdjson::dom::array pc;
                if (ev["price_changes"].get(pc) == simdjson::SUCCESS) {
                    std::unordered_map<std::string, BookUpdate> by_tok;
                    for (auto ch : pc) {
                        std::string aid = extract_asset_id(ch["asset_id"]);
                        if (aid.empty()) continue;
                        PriceLevel l;
                        l.price = field_as_double(ch["price"]);
                        l.size  = field_as_double(ch["size"]);   // size "0" = level removed
                        if (!std::isfinite(l.price) || !std::isfinite(l.size) || l.price <= 0) continue;
                        bool buy = false;
                        if (!read_side(ch["side"], buy)) continue;
                        auto& bu = by_tok[std::string(aid)];
                        if (bu.token_id.empty()) {
                            bu.is_snapshot = false;
                            bu.token_id = std::string(aid);
                            bu.exchange_ms = field_as_i64(ev["timestamp"]);
                            if (has_mkt) bu.condition_id = std::string(mkt_sv);
                        }
                        if (buy) bu.bids.push_back(l);
                        else     bu.asks.push_back(l);
                    }
                    for (auto& kv : by_tok) {
                        BookUpdate& bu = kv.second;
                        spdlog::debug("book delta: token={} bids={} asks={}",
                                      bu.token_id, bu.bids.size(), bu.asks.size());
                        if (on_book_ && (!bu.bids.empty() || !bu.asks.empty())) {
                            st->ev_delta.fetch_add(1, std::memory_order_relaxed);
                            on_book_(bu);
                        }
                    }
                    return;
                }
                // ---- LEGACY fallback: top-level asset_id + "changes" field ----
                BookUpdate bu; bu.is_snapshot = false;
                std::string_view sv;
                if (has_mkt) bu.condition_id = std::string(mkt_sv);
                bu.token_id = extract_asset_id(ev["asset_id"]);
                bu.exchange_ms = field_as_i64(ev["timestamp"]);
                if (bu.token_id.empty()) return;
                // "changes" can be array of objects, array of arrays, or pipe-delimited string.
                // Handle all three formats robustly.
                simdjson::dom::array arr;
                if (ev["changes"].get(arr) == simdjson::SUCCESS) {
                    for (auto ch : arr) {
                        PriceLevel l;
                        bool buy = false;
                        // Try object: {"price":"...","size":"...","side":"BUY"}
                        if (ch["price"].error() == simdjson::SUCCESS) {
                            l.price = field_as_double(ch["price"]);
                            l.size  = field_as_double(ch["size"]);
                            if (!read_side(ch["side"], buy)) continue;
                        } else {
                            // Try array: [side, price, size]
                            simdjson::dom::array ch_arr;
                            if (ch.get(ch_arr) == simdjson::SUCCESS && ch_arr.size() >= 3) {
                                auto it = ch_arr.begin();
                                if (!read_side(*it, buy)) continue;
                                ++it; l.price = field_as_double(*it);
                                ++it; l.size  = field_as_double(*it);
                            } else {
                                continue; // unrecognised change format
                            }
                        }
                        if (!std::isfinite(l.price) || !std::isfinite(l.size)) continue;
                        if (l.price > 0) {
                            if (buy) bu.bids.push_back(l);
                            else     bu.asks.push_back(l);
                        }
                    }
                } else {
                    // Try pipe-delimited string format: "BUY|0.50|100,SELL|0.60|50"
                    std::string_view ch_str;
                    if (ev["changes"].get(ch_str) == simdjson::SUCCESS && !ch_str.empty()) {
                        std::string s(ch_str);
                        size_t pos = 0;
                        while (pos < s.size()) {
                            size_t comma = s.find(',', pos);
                            std::string part = s.substr(pos, comma - pos);
                            pos = (comma == std::string::npos) ? s.size() : comma + 1;
                            // part = "SIDE|price|size"
                            size_t p1 = part.find('|');
                            if (p1 == std::string::npos) continue;
                            size_t p2 = part.find('|', p1 + 1);
                            if (p2 == std::string::npos) continue;
                            std::string_view side(part.data(), p1);
                            double price = sv_to_double(std::string_view(part.data() + p1 + 1, p2 - p1 - 1));
                            double size  = sv_to_double(std::string_view(part.data() + p2 + 1, part.size() - p2 - 1));
                            if (!std::isfinite(price) || !std::isfinite(size)) continue;
                            bool buy = false;
                            if (!side_is_buy(side, buy)) continue;
                            PriceLevel l; l.price = price; l.size = size;
                            if (price > 0) {
                                if (buy) bu.bids.push_back(l);
                                else     bu.asks.push_back(l);
                            }
                        }
                    }
                }
                spdlog::debug("book delta: token={} bids={} asks={}", bu.token_id, bu.bids.size(), bu.asks.size());
                if (on_book_ && (!bu.bids.empty() || !bu.asks.empty())) on_book_(bu);
                return;
            }
            if (et == "best_bid_ask") {
                // Pushed on every BBO change (custom_feature_enabled:true).
                // No sizes in this event — do NOT touch the ladder here; the
                // consumer uses it for freshness + desync detection only.
                st->ev_bbo.fetch_add(1, std::memory_order_relaxed);
                if (!on_bbo_) return;
                BboUpdate bb;
                std::string_view sv;
                if (ev["market"].get(sv) == simdjson::SUCCESS) bb.condition_id = std::string(sv);
                bb.token_id = extract_asset_id(ev["asset_id"]);
                if (bb.token_id.empty()) return;
                bb.best_bid    = field_as_double(ev["best_bid"]);
                bb.best_ask    = field_as_double(ev["best_ask"]);
                bb.exchange_ms = field_as_i64(ev["timestamp"]);
                on_bbo_(bb);
                return;
            }
            if (et == "last_trade_price") {
                st->ev_trade.fetch_add(1, std::memory_order_relaxed);
                if (!on_trade_) return;
                TradeUpdate tu;
                std::string_view sv;
                if (ev["market"].get(sv) == simdjson::SUCCESS)    tu.condition_id = std::string(sv);
                if (ev["asset_id"].get(sv) == simdjson::SUCCESS)  tu.token_id     = std::string(sv);
                if (tu.token_id.empty()) return;
                tu.price = field_as_double(ev["price"]);
                tu.size  = field_as_double(ev["size"]);
                tu.exchange_ms = field_as_i64(ev["timestamp"]);
                on_trade_(tu);
                return;
            }
            // Any other event_type we don't act on (tick_size_change, best_bid_ask,
            // market_resolved, …) — count it so "frames arrive but books are stale"
            // can be diagnosed by WHAT the frames are.
            if (has_event_type) {
                if (et == "tick_size_change")
                    st->ev_ticksz.fetch_add(1, std::memory_order_relaxed);
                else {
                    st->ev_unknown.fetch_add(1, std::memory_order_relaxed);
                    std::snprintf(st->last_unknown, sizeof(st->last_unknown), "%.*s",
                                  (int)et.size(), et.data());
                }
            }
        };
        // Polymarket can batch multiple JSON objects in one WS message,
        // e.g. {...}{...} or even [...][...]. simdjson::parser.parse()
        // expects a single document and fails on concatenated objects.
        // Strategy: scan for top-level brace/bracket boundaries and parse
        // each sub-document individually, then dispatch via handle_one.
        auto parse_one_object = [&](const char* data, size_t len) -> bool {
            simdjson::padded_string sub(data, len);
            simdjson::dom::element d;
            if (parser.parse(sub).get(d)) return false;
            simdjson::dom::array arr;
            if (d.get_array().get(arr) == simdjson::SUCCESS) {
                for (auto ev : arr) handle_one(ev);
            } else {
                handle_one(d);
            }
            return true;
        };
        // Fast path: try the whole frame as a single document first.
        if (parse_one_object(frame.data(), frame.size())) { /* ok */ }
        else {
            // Slow path: split on top-level {…} or […].
            size_t pos = 0;
            bool any_parsed = false;
            while (pos < frame.size()) {
                // skip whitespace
                while (pos < frame.size() && (frame[pos] == ' ' || frame[pos] == '\t' ||
                       frame[pos] == '\n' || frame[pos] == '\r')) ++pos;
                if (pos >= frame.size()) break;
                char opener = frame[pos];
                char closer;
                if      (opener == '{') closer = '}';
                else if (opener == '[') closer = ']';
                else { ++pos; continue; }
                // track brace/bracket depth to find the matching closer
                int depth = 0;
                size_t start = pos;
                bool in_str = false;
                while (pos < frame.size()) {
                    char c = frame[pos];
                    if (c == '"') {
                        std::size_t slashes = 0;
                        for (std::size_t p = pos; p > start && frame[p - 1] == '\\'; --p)
                            ++slashes;
                        if (slashes % 2 == 0) in_str = !in_str;
                    }
                    if (!in_str) {
                        if      (c == opener) ++depth;
                        else if (c == closer && --depth == 0) {
                            size_t obj_len = pos - start + 1;
                            if (parse_one_object(frame.data() + start, obj_len))
                                any_parsed = true;
                            else
                                st->ev_parsefail.fetch_add(1, std::memory_order_relaxed);
                            ++pos;
                            break;
                        }
                    }
                    ++pos;
                }
                if (depth != 0) {
                    // unmatched brace — skip remaining data
                    st->ev_parsefail.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
            if (!any_parsed)
                st->ev_parsefail.fetch_add(1, std::memory_order_relaxed);
        }
    };
    auto on_state = [this, st](const std::string& state) {
        spdlog::info("pm-md ws state: {}", state);
        if (state == "connected") {
            st->connected.store(true);
            if (on_state_) on_state_(true);
            // Re-send the FULL subscription on every (re)connect. This is a fresh
            // socket, so the first batch (re)establishes with type:market and the
            // rest are additive operation:subscribe — see send_batches().
            std::vector<std::string> toks;
            { std::lock_guard<std::mutex> lk(st->mu); toks = st->tokens; }
            if (!toks.empty()) {
                send_batches(toks, /*fresh_conn=*/true);
                spdlog::info("pm-md (re)subscribed to {} tokens after reconnect", toks.size());
            }
        } else {
            st->connected.store(false);
            if (on_state_) on_state_(false);
        }
    };
    ws_ = std::make_unique<WsClient>(url_, std::move(on_frame), std::move(on_state));
}
PolymarketMD::~PolymarketMD() { stop(); }
void PolymarketMD::start() {
    auto st = std::static_pointer_cast<PmMdState>(state_);
    st->running.store(true);
    st->keepalive_counter = 0;
    if (ws_) ws_->start();
    // Keepalive thread: PING every 8s while running.
    st->keepalive_th = std::thread([this, st]{
        using namespace std::chrono;
        uint64_t last_frames = 0;
        int diag_counter = 0;
        while (st->running.load()) {
            {
                std::unique_lock<std::mutex> lk(st->wait_mu);
                if (st->wait_cv.wait_for(lk, seconds(1),
                        [&] { return !st->running.load(); })) break;
            }
            // Polymarket market channel: send "PING" heartbeat every <=10s.
            // WsClient drops the frame if a reconnect is in progress.
            if (++st->keepalive_counter >= 8) {
                st->keepalive_counter = 0;
                if (st->running.load() && ws_) {
                    try { ws_->send("PING"); }
                    catch (const std::exception& e) {
                        spdlog::debug("keepalive send failed: {}", e.what());
                    }
                    catch (...) {}
                }
            }
            // Diagnostic: log WS frame throughput every 10s. Lets us tell
            // "frames stop arriving" (WS/subscription) apart from "frames arrive
            // but books go stale" (downstream lock starvation / callback stall).
            if (++diag_counter >= 10) {
                diag_counter = 0;
                uint64_t f = st->frames.load(std::memory_order_relaxed);
                spdlog::info("pm-md frames: total={} (+{} in 10s, {:.1f}/s) connected={}",
                             f, f - last_frames, (f - last_frames) / 10.0,
                             st->connected.load());
                last_frames = f;
                // Event-type breakdown so "frames arrive but books stale" is
                // diagnosable: snap+delta should dominate when books are healthy;
                // a high `unknown` (with last='...') means Polymarket is sending an
                // event_type we don't parse into book updates.
                spdlog::info("pm-md events/10s: snap={} delta={} trade={} ticksz={} bbo={} unknown={} parsefail={} last_unknown='{}'",
                             st->ev_snap.exchange(0, std::memory_order_relaxed),
                             st->ev_delta.exchange(0, std::memory_order_relaxed),
                             st->ev_trade.exchange(0, std::memory_order_relaxed),
                             st->ev_ticksz.exchange(0, std::memory_order_relaxed),
                             st->ev_bbo.exchange(0, std::memory_order_relaxed),
                             st->ev_unknown.exchange(0, std::memory_order_relaxed),
                             st->ev_parsefail.exchange(0, std::memory_order_relaxed),
                             st->last_unknown);
                if (st->last_frame_sample[0])
                    spdlog::info("pm-md frame sample: {}", st->last_frame_sample);
            }
        }
    });
}
void PolymarketMD::stop() {
    auto st = std::static_pointer_cast<PmMdState>(state_);
    if (!st) return;
    st->running.store(false);
    st->wait_cv.notify_all();
    if (st->keepalive_th.joinable()) st->keepalive_th.join();
    if (ws_) ws_->stop();
}
bool PolymarketMD::connected() const noexcept {
    auto st = std::static_pointer_cast<PmMdState>(state_);
    return st && st->connected.load();
}
void PolymarketMD::send_batches(const std::vector<std::string>& toks, bool fresh_conn) {
    if (!ws_ || toks.empty()) return;
    using namespace std::chrono;
    // The subscription universe is hard-capped upstream (~160 tokens), and the WS
    // docs allow up to 500 assets_ids per message, so a single batch covers us.
    // This matters because the reconnect path calls send_batches() ON the WS
    // io_context thread (inside the "connected" state callback); the old
    // inter-batch sleep_for therefore BLOCKED the read loop for seconds on every
    // reconnect — turning one drop into a stall. One batch = one non-blocking
    // post = no stall. The loop/gap below is kept only as a safety net for the
    // (now impossible) >500-token case.
    constexpr size_t kBatchSize = 500;
    for (size_t bs = 0; bs < toks.size(); bs += kBatchSize) {
        if (bs > 0) {
            // Inter-batch gap so Polymarket doesn't see us as a rate-limited
            // burst and drop the socket. Large token sets need breathing room.
            std::this_thread::sleep_for(milliseconds(1000));  // 1s gap
        }
        size_t be = std::min(bs + kBatchSize, toks.size());
        // First batch on a fresh connection establishes the channel; everything
        // else is an additive subscribe so we don't replace the prior batches.
        // custom_feature_enabled unlocks best_bid_ask / new_market /
        // market_resolved push events (official market-channel docs). The BBO
        // push is the permanent stale-book fix: per-token freshness heartbeat
        // + ladder-desync detection without blanket REST sweeps.
        const bool initial = (fresh_conn && bs == 0);
        std::string msg = initial
            ? std::string(R"({"type":"market","custom_feature_enabled":true,"assets_ids":[)")
            : std::string(R"({"operation":"subscribe","assets_ids":[)");
        for (size_t i = bs; i < be; ++i) {
            if (i != bs) msg += ',';
            msg += '"'; msg += toks[i]; msg += '"';
        }
        msg += "]}";
        ws_->send(std::move(msg));
    }
}
void PolymarketMD::subscribe(const std::vector<std::string>& token_ids) {
    auto st = std::static_pointer_cast<PmMdState>(state_);
    if (!ws_ || token_ids.empty()) return;
    std::vector<std::string> fresh;  // tokens new to the set (to add on the wire)
    {
        std::lock_guard<std::mutex> lk(st->mu);
        // ACCUMULATE (union) into the resubscribe set — never replace. Callers
        // subscribe incrementally (e.g. "+4 new markets" = 8 tokens). The
        // resubscribe-on-reconnect path (on_state) must always hold the full set.
        std::unordered_set<std::string> seen(st->tokens.begin(), st->tokens.end());
        for (const auto& t : token_ids)
            if (seen.insert(t).second) { st->tokens.push_back(t); fresh.push_back(t); }
    }
    if (fresh.empty()) return;  // nothing new — don't re-send (would be a no-op churn)
    if (!st->connected.load()) {
        spdlog::info("pm-md staged +{} tokens ({} total in set) until websocket connect",
                     fresh.size(), st->tokens.size());
        return;
    }
    // Incremental add on the LIVE socket: additive operation:subscribe only, so
    // existing subscriptions are preserved (a type:market message would replace
    // them and leave only this batch subscribed — the 462/512-never-updated bug).
    send_batches(fresh, /*fresh_conn=*/false);
    spdlog::info("pm-md subscribed +{} new tokens ({} total in set)",
                 fresh.size(), st->tokens.size());
}
void PolymarketMD::unsubscribe(const std::vector<std::string>& token_ids) {
    auto st = std::static_pointer_cast<PmMdState>(state_);
    if (!ws_ || token_ids.empty()) return;
    std::vector<std::string> drop;  // tokens actually in the set (to remove on the wire)
    {
        std::lock_guard<std::mutex> lk(st->mu);
        std::unordered_set<std::string> remove(token_ids.begin(), token_ids.end());
        std::vector<std::string> kept;
        kept.reserve(st->tokens.size());
        for (auto& t : st->tokens) {
            if (remove.count(t)) drop.push_back(t);
            else                 kept.push_back(std::move(t));
        }
        st->tokens.swap(kept);
    }
    if (drop.empty()) return;
    if (!st->connected.load()) {
        spdlog::info("pm-md staged unsubscribe -{} dead tokens ({} remain in set)",
                     drop.size(), st->tokens.size());
        return;
    }
    // Per the WS docs, drop assets with {"operation":"unsubscribe","assets_ids":[…]}.
    constexpr size_t kBatchSize = 50;
    for (size_t bs = 0; bs < drop.size(); bs += kBatchSize) {
        size_t be = std::min(bs + kBatchSize, drop.size());
        std::string msg = R"({"operation":"unsubscribe","assets_ids":[)";
        for (size_t i = bs; i < be; ++i) {
            if (i != bs) msg += ',';
            msg += '"'; msg += drop[i]; msg += '"';
        }
        msg += "]}";
        ws_->send(std::move(msg));
    }
    spdlog::info("pm-md unsubscribed -{} dead tokens ({} remain in set)",
                 drop.size(), st->tokens.size());
}
} // namespace vp
