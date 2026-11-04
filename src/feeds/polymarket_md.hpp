#pragma once
// Polymarket CLOB market-data websocket -> book deltas + trades.
#include "core/types.hpp"
#include <charconv>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vp {

// Polymarket reports the last trade's BUY/SELL direction. Keep it separate
// from the local BBO tick-rule inference used by existing consumers.
inline std::optional<OrderDir> parse_reported_trade_side(
    std::string_view side) noexcept {
    if (side == "BUY" || side == "buy") return OrderDir::Buy;
    if (side == "SELL" || side == "sell") return OrderDir::Sell;
    return std::nullopt;
}

// Market events may omit their exchange timestamp. Keep such observations in
// event-time consumers using the receive time instead of epoch zero.
inline std::int64_t resolve_trade_timestamp_ms(
    std::int64_t exchange_ms, std::int64_t received_ms) noexcept {
    return exchange_ms > 0 ? exchange_ms : received_ms;
}

// A missing or malformed optional trade size must not masquerade as a real
// zero-size print in notional and tape-coverage summaries.
inline std::optional<double> parse_reported_trade_size(
    std::string_view raw) noexcept {
    if (raw.empty()) return std::nullopt;
    double value = 0.0;
    const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != raw.data() + raw.size() ||
        !std::isfinite(value))
        return std::nullopt;
    return value;
}

class WsClient;

class PolymarketMD {
public:
    struct BookUpdate {
        std::string condition_id;
        std::string token_id;
        ShareSide   side_label;
        std::vector<PriceLevel> bids;
        std::vector<PriceLevel> asks;
        std::int64_t exchange_ms = 0;
        bool        is_snapshot = false;
    };
    struct TradeUpdate {
        std::string condition_id;
        std::string token_id;
        std::optional<OrderDir> reported_side;
        double      price = 0;
        double      size  = 0;
        bool        size_available = false;
        std::int64_t exchange_ms = 0;
    };

    // Best bid/ask push event — requires custom_feature_enabled:true in the
    // initial subscription. The server sends one on EVERY BBO change, so it is
    // both a freshness heartbeat per token and a desync detector (compare with
    // the locally-built ladder; mismatch => that book needs a REST re-seed).
    struct BboUpdate {
        std::string condition_id;
        std::string token_id;
        double      best_bid = 0;
        double      best_ask = 0;
        std::int64_t exchange_ms = 0;
    };

    using OnBook  = std::function<void(const BookUpdate&)>;
    using OnTrade = std::function<void(const TradeUpdate&)>;
    using OnBbo   = std::function<void(const BboUpdate&)>;
    using OnState = std::function<void(bool connected)>;

PolymarketMD(std::string ws_url, OnBook on_book, OnTrade on_trade,
             OnBbo on_bbo = nullptr, OnState on_state = nullptr);
     ~PolymarketMD();

     void start();
     void stop();

     void subscribe(const std::vector<std::string>& token_ids);

     // Drop tokens from the live subscription AND the resubscribe set so they
     // don't return on the next reconnect. Prunes closed-window markets — without
     // it the subscribe set grows unbounded over a session, which is what makes
     // Polymarket close the socket (reconnect storms → book-feed death). Sends
     // {"operation":"unsubscribe","assets_ids":[…]}.
     void unsubscribe(const std::vector<std::string>& token_ids);

     // True while the market-data websocket is connected (set on connect, cleared
     // on disconnect). Lets callers report the REAL feed state in health logs.
     bool connected() const noexcept;

 private:
     // Send a (re)subscription for `toks`, batched defensively. Polymarket's market
     // channel treats a {"type":"market",...} message as a REPLACE of the live
     // subscription, so multiple type:market batches leave only the LAST batch
     // subscribed. Per the WS docs, additional assets must be added with
     // {"operation":"subscribe",...}. So: when `fresh_conn` (a new socket), the
     // first batch uses type:market to (re)establish and the rest use
     // operation:subscribe; otherwise (incremental add on a live socket) ALL
     // batches use operation:subscribe.
     void send_batches(const std::vector<std::string>& toks, bool fresh_conn);

     std::string url_;
     OnBook      on_book_;
     OnTrade     on_trade_;
     OnBbo       on_bbo_;
     OnState     on_state_;
     std::unique_ptr<WsClient> ws_;
     std::shared_ptr<void>     state_;   // PmMdState (opaque to header)
 };

} // namespace vp
