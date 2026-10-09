#pragma once
// Async TLS WebSocket client (Boost.Beast). Auto-reconnect with backoff.
// Owns one io_context on a dedicated thread. Frames are surfaced as
// std::string_view via the user callback (do NOT capture the view past
// the call — copy if you need to keep it).

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace vp {

class WsClient {
public:
    using OnFrame = std::function<void(std::string_view)>;
    using OnState = std::function<void(const std::string&)>; // "connected" / "disconnected: reason"

    WsClient(std::string url, OnFrame on_frame, OnState on_state);
    ~WsClient();

    void start();
    void stop();
    void send(std::string text);

    bool connected() const noexcept;

private:
    struct Impl;
    std::string         url_;
    OnFrame             on_frame_;
    OnState             on_state_;
    std::shared_ptr<Impl> impl_;
    std::atomic<bool>   running_{false};
};

} // namespace vp
