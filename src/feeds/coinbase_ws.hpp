#pragma once
// Coinbase Exchange websocket ticker feed (BTC-USD/ETH-USD/SOL-USD/XRP-USD).
// Why it matters: Chainlink's btc/usd-style streams aggregate USD venues with
// Coinbase dominant, so Coinbase LEADS the Polymarket oracle by ~1.5-3s.
// This feed powers (a) the coinbase-lead sniper signal and (b) oracle
// failover (preferred over Binance, whose USDT pairs carry a basis error).
#include "core/types.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vp {

class WsClient;

class CoinbaseWsFeed {
public:
    // Reuses AssetTick: symbol = "BTC-USD" style product id, qty = last_size.
    using OnTick = std::function<void(const AssetTick&)>;

    CoinbaseWsFeed(std::string ws_url,
                   std::vector<std::string> product_ids,
                   OnTick on_tick);
    ~CoinbaseWsFeed();

    void start();
    void stop();

private:
    void send_subscribe();

    std::string               ws_url_;
    std::vector<std::string>  product_ids_;
    OnTick                    on_tick_;
    std::unique_ptr<WsClient> ws_;
};

} // namespace vp
