#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace vp {

class WsClient;

// Best-bid/ask midpoint from a public spot venue. This observation is kept
// separate from AssetTick so it cannot accidentally enter strategy paths.
struct SpotExchangeTick {
    std::string venue;
    std::string asset;
    std::string pair;
    double price = 0.0;
    double bid = 0.0;
    double bid_size = 0.0;
    double ask = 0.0;
    double ask_size = 0.0;
    std::int64_t exchange_ts_ms = 0;
    std::int64_t received_ts_ms = 0;
    std::int64_t received_mono_ns = 0;
};

class SpotExchangeFeed {
public:
    using OnTick = std::function<void(const SpotExchangeTick&)>;

    SpotExchangeFeed(std::string venue, std::string ws_url,
                     std::vector<std::string> instruments, OnTick on_tick);
    ~SpotExchangeFeed();

    void start();
    void stop();

private:
    void send_subscribe();
    void on_frame(std::string_view frame);

    std::string venue_;
    std::string ws_url_;
    std::vector<std::string> instruments_;
    OnTick on_tick_;
    std::unique_ptr<WsClient> ws_;
};

} // namespace vp
