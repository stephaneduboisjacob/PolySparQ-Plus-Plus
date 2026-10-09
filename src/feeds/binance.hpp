#pragma once
// Binance trade stream feed (BTCUSDT/ETHUSDT) -> AssetTick events.
#include "core/types.hpp"
#include <functional>
#include <memory>
#include <vector>
#include <string>

namespace vp {

class WsClient;

class BinanceFeed {
public:
    using OnTick = std::function<void(const AssetTick&)>;

    BinanceFeed(std::string ws_url,
                std::vector<std::string> streams,
                OnTick on_tick);
    ~BinanceFeed();

    void start();
    void stop();

private:
    std::string              ws_url_;
    std::vector<std::string> streams_;
    OnTick                   on_tick_;
    std::unique_ptr<WsClient> ws_;
};

} // namespace vp

