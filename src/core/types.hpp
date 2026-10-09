#pragma once
#include <cstdint>
#include <string>
#include <chrono>

namespace vp {

enum class TradeMode : std::uint8_t { Live, Paper, Replay };
enum class ShareSide : std::uint8_t { UP, DOWN };
enum class OrderDir  : std::uint8_t { Buy, Sell };

inline const char* to_string(TradeMode m) noexcept {
    switch (m) { case TradeMode::Live: return "live";
                 case TradeMode::Paper: return "paper";
                 case TradeMode::Replay: return "replay"; }
    return "?";
}
inline const char* to_string(ShareSide s) noexcept {
    return s == ShareSide::UP ? "UP" : "DOWN";
}
inline const char* to_string(OrderDir d) noexcept {
    return d == OrderDir::Buy ? "buy" : "sell";
}

// Tick of the external asset (BTC/ETH) coming from Binance.
struct AssetTick {
    std::string symbol;       // BTCUSDT / ETHUSDT
    double      price = 0;
    double      qty   = 0;    // trade size in base asset
    bool        buyer_is_maker = false; // true => seller hit bid (sell aggressor)
    std::int64_t exchange_ms = 0;       // exchange-side ms
    std::int64_t recv_mono_ns = 0;      // local arrival
    // ── bookTicker payloads (Phase 6.3) ──
    // Binance bookTicker delivers sub-100ms top-of-book updates. When
    // is_book_ticker is true, bid/ask fields carry the live best quote and
    // price/qty/buyer_is_maker are unused (price = mid for convenience).
    bool        is_book_ticker = false;
    double      bid_price = 0, bid_qty = 0;
    double      ask_price = 0, ask_qty = 0;
    // ── futures forceOrder (liquidation) payloads (Phase 4.1) ──
    // is_force_order = true: qty = liquidated size, buyer_is_maker = true
    // for BUY-side liquidations (forced buy → pushes price UP), false for
    // SELL-side liquidations (forced sell → pushes price DOWN).
    bool        is_force_order = false;
};

// Single price level (for top-of-book or shallow depth).
struct PriceLevel {
    double price = 0;
    double size  = 0;
};

} // namespace vp

