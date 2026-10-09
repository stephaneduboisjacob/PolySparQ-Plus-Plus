#pragma once
// Polymarket RTDS oracle feed — streams Chainlink crypto prices via
// wss://ws-live-data.polymarket.com (no API keys required).
//
// Protocol: subscribe to crypto_prices_chainlink topic; send "PING" every 5s.
// Updates oracle_btc / oracle_eth atomically so the rule engine can use the
// exact Chainlink price that settles each market.
//
// Threading: owns one WsClient (io_context on its own thread); sends periodic
// PING on a separate std::thread. Callbacks are fire-and-forget.
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace vp {

class WsClient;

struct OracleTick {
    std::string asset;   // "BTC" or "ETH"
    double      price;
    int64_t     ts_ms;
    int         window_s = 0; // 0 spot, 30/60 official Chainlink TWAP
};

class PolymarketRtds {
public:
    using OnTick = std::function<void(const OracleTick&)>;

    PolymarketRtds(std::string ws_url, OnTick on_tick);
    ~PolymarketRtds();

    void start();
    void stop();

private:
    void send_subscribe();
    void ping_loop();

    std::string              url_;
    OnTick                   on_tick_;
    std::unique_ptr<WsClient> ws_;
    std::atomic<bool>        running_{false};
    std::mutex               ping_mu_;
    std::condition_variable  ping_cv_;
    std::unique_ptr<std::thread> ping_th_;
};

} // namespace vp
