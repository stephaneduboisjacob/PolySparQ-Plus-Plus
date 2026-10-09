// Async WebSocket client (Boost.Beast). Auto-reconnect with exp backoff.
// wss:// uses TLS with peer verification; ws:// is plain TCP (for the local
// feed-proxy fan-out — never use it across a real network). Owns one
// io_context running on a dedicated thread. Frames go through a user
// callback (Beast's flat_buffer; we hand a string_view per message).
//
// We deliberately keep this Impl a single-stream client; for parallel feeds
// we instantiate multiple WsClients (one io_context each) — total threads
// stay small.

#include "ws_client.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <spdlog/spdlog.h>

#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <regex>

namespace vp {

namespace beast = boost::beast;
namespace http  = boost::beast::http;
namespace bws   = boost::beast::websocket;
namespace asio  = boost::asio;
namespace ssl   = boost::asio::ssl;
using tcp       = boost::asio::ip::tcp;

namespace {
constexpr std::size_t kMaxInboundMessageBytes = 16 * 1024 * 1024;

struct ParsedUrl {
    std::string host, port, target;
    bool tls = true;
};
std::optional<ParsedUrl> parse_ws_url(const std::string& url) {
    static const std::regex re(R"(^(wss?)://([^/:]+)(?::(\d+))?(/.*)?$)",
                               std::regex::icase);
    std::smatch m;
    if (!std::regex_match(url, m, re)) return std::nullopt;
    ParsedUrl out;
    std::string scheme = m[1].str();
    for (auto& c : scheme) c = std::tolower(c);
    out.tls = (scheme == "wss");
    out.host = m[2].str();
    out.port = m[3].matched ? m[3].str() : (out.tls ? "443" : "80");
    out.target = m[4].matched ? m[4].str() : "/";
    return out;
}
} // namespace

struct WsClient::Impl : std::enable_shared_from_this<WsClient::Impl> {
    using Stream      = bws::stream<ssl::stream<beast::tcp_stream>>;
    using PlainStream = bws::stream<beast::tcp_stream>;

    Impl(std::string url, OnFrame on_frame, OnState on_state)
        : url_(std::move(url)),
          on_frame_(std::move(on_frame)),
          on_state_(std::move(on_state)),
          ssl_ctx_(ssl::context::tls_client),
          resolver_(asio::make_strand(ioc_)),
          backoff_(std::chrono::milliseconds(250)) {
        ssl_ctx_.set_default_verify_paths();
        ssl_ctx_.set_verify_mode(ssl::verify_peer);
    }

    ~Impl() {
        // Defensive: if stop() wasn't called, ensure socket + stream are
        // destroyed on the io_context thread before the Impl goes away.
        // Use a gate to avoid double-free with stop().
        if (!stopping_.load()) {
            if (th_.joinable()) {
                // Don't close from here — the thread owns stream_.
                // Just post a stop via the io_context.
                asio::post(ioc_, [this] {
                    if (reconnect_timer_) reconnect_timer_->cancel();
                    close_ws();
                    ioc_.stop();
                });
                ioc_.stop();
                th_.join();
            }
        }
    }

    void start() {
        th_ = std::thread([self = shared_from_this()] {
            asio::post(self->ioc_, [self] { self->schedule_connect(); });
            self->ioc_.run();
        });
    }
    void stop() {
        if (stopping_.exchange(true)) return; // idempotent
        // Cancel the reconnect timer so it doesn't fire during shutdown.
        if (reconnect_timer_) reconnect_timer_->cancel();
        // Close the TCP socket from outside the io_context thread.
        // This interrupts any pending async_read/async_write with
        // operation_aborted → on_socket_error → checks stopping_ → no retry.
        // This is the SAFE way to break out of a stuck WS without depending
        // on a posted handler (which ioc_.stop() might prevent from running).
        with_stream([](auto& s) {
            beast::error_code ec;
            beast::get_lowest_layer(s).socket().close(ec);
        });
        // Now stop the io_context so the thread exits quickly.
        ioc_.stop();
        if (th_.joinable()) th_.join();
        // Clean up TLS/WS state now that the io_context thread is done.
        // If on_socket_error's close_ws() already cleaned up, this is a no-op.
        close_ws();
    }
    void send(std::string text) {
        // Don't queue new sends once stopping
        if (stopping_.load()) return;
        asio::post(ioc_, [this, t = std::move(text)]() mutable {
            if (stopping_.load()) return;
            if (!connected_.load()) {
                spdlog::debug("ws drop send while disconnected: {}", url_);
                return;
            }
            tx_queue_.push_back(std::move(t));
            if (!writing_) drain_tx();
        });
    }
    bool connected() const noexcept { return connected_.load(); }

private:
    void notify(const std::string& s) { if (on_state_) on_state_(s); }

    void schedule_connect() {
        if (stopping_.load()) return;
        // Re-arm the teardown gate and destroy the previous stream now that we're
        // on a fresh attempt: any ops on the old stream were aborted by the socket
        // close in on_socket_error and have since drained (the retry timer gave
        // them a turn), so freeing it here is safe and keeps no SSL_shutdown on a
        // half-open op. On the very first connect stream_ is null (harmless).
        tearing_down_ = false;
        writing_ = false;
        tx_queue_.clear();
        // A failed async_read can leave a partial fragmented WebSocket message in
        // the DynamicBuffer. The next socket is a completely new byte stream, so
        // retaining that tail corrupts every subsequent message (old tail + new
        // payload) and makes the buffer grow without bound across reconnects.
        rx_buffer_.consume(rx_buffer_.size());
        stream_.reset();
        plain_.reset();
        auto u = parse_ws_url(url_);
        if (!u) { spdlog::error("ws: bad url {}", url_); return; }
        url_p_ = *u;
        auto self = shared_from_this();
        resolver_.async_resolve(url_p_.host, url_p_.port,
            [self](beast::error_code ec, tcp::resolver::results_type r) {
                if (ec) { self->retry("resolve: " + ec.message()); return; }
                self->do_connect(std::move(r));
            });
    }
    // Dispatch a generic lambda to whichever stream variant is live.
    template <class F>
    void with_stream(F&& f) {
        if (stream_)      f(*stream_);
        else if (plain_)  f(*plain_);
    }
    bool has_stream() const { return stream_ || plain_; }

    void do_connect(tcp::resolver::results_type results) {
        if (url_p_.tls) {
            stream_ = std::make_unique<Stream>(asio::make_strand(ioc_), ssl_ctx_);
            if (!SSL_set_tlsext_host_name(stream_->next_layer().native_handle(),
                                          url_p_.host.c_str())) {
                beast::error_code ec{static_cast<int>(::ERR_get_error()),
                                     boost::asio::error::get_ssl_category()};
                retry("sni: " + ec.message());
                return;
            }
        } else {
            plain_ = std::make_unique<PlainStream>(asio::make_strand(ioc_));
        }
        auto self = shared_from_this();
        with_stream([&](auto& s) {
            beast::get_lowest_layer(s).expires_after(std::chrono::seconds(15));
            beast::get_lowest_layer(s).async_connect(results,
                [self](beast::error_code ec,
                       tcp::resolver::results_type::endpoint_type) {
                    if (ec) { self->retry("tcp: " + ec.message()); return; }
                    if (self->stream_) self->do_tls();
                    else               self->do_ws();
                });
        });
    }
    void do_tls() {
        beast::get_lowest_layer(*stream_).expires_after(std::chrono::seconds(15));
        auto self = shared_from_this();
        stream_->next_layer().async_handshake(ssl::stream_base::client,
            [self](beast::error_code ec) {
                if (ec) { self->retry("tls: " + ec.message()); return; }
                self->do_ws();
            });
    }
    void do_ws() {
        auto self = shared_from_this();
        with_stream([&](auto& s) {
            beast::get_lowest_layer(s).expires_never();
            // Custom timeout: no TCP timer (expires_never above). The application
            // layer (polymarket_md.cpp) sends PING heartbeats, so we set a
            // generous idle timeout and disable Beast-level keepalive pings — the
            // CLOB server may not respond to WebSocket PING frames, which would
            // cause Beast to tear down the connection even though it's healthy.
            bws::stream_base::timeout to{};
            to.handshake_timeout = std::chrono::seconds(30);
            to.idle_timeout      = std::chrono::seconds(300); // 5min — app heartbeats keep it alive
            to.keep_alive_pings  = false;
            s.set_option(to);
            // Bound memory even if an upstream sends an unterminated fragmented
            // message. Normal venue messages and initial book snapshots are far
            // below this ceiling.
            s.read_message_max(kMaxInboundMessageBytes);
            s.set_option(bws::stream_base::decorator(
                [](bws::request_type& req) {
                    req.set(http::field::user_agent, "vantage-pulse/0.1");
                }));
            s.async_handshake(url_p_.host, url_p_.target,
                [self](beast::error_code ec) {
                    if (ec) { self->retry("ws hs: " + ec.message()); return; }
                    self->on_open();
                });
        });
    }
    void on_open() {
        connected_.store(true);
        // NOTE: do NOT reset backoff here. A socket that completes the WS
        // handshake but is then closed by the server before sending any data
        // (e.g. an auth-rejected user channel) would otherwise reset to 500ms
        // every connect and hammer the server every ~0.6s forever. Instead we
        // reset backoff only on the first DATA frame (see do_read) — proof the
        // subscription/auth actually succeeded. Auth-failing sockets thus
        // escalate 500ms→1s→…→10s and stop flooding.
        notify("connected");
        spdlog::info("ws connected: {}", url_);
        do_read();
        if (!tx_queue_.empty()) drain_tx();
    }
    void do_read() {
        auto self = shared_from_this();
        with_stream([&](auto& s) {
        s.async_read(rx_buffer_,
            [self, &s](beast::error_code ec, std::size_t bytes) {
                if (ec) { self->on_socket_error("read: " + ec.message()); return; }
                // Copy the buffer into a contiguous string to ensure the payload
                // is contiguous (flat_buffer may expose a buffer sequence). Use
                // beast::buffers_to_string which handles the buffer sequence.
                std::string msg = beast::buffers_to_string(self->rx_buffer_.data());
                // The buffer contains exactly the completed WebSocket message.
                // Clear all readable bytes before invoking application code so a
                // throwing/slow callback cannot retain or contaminate the next read.
                self->rx_buffer_.consume(self->rx_buffer_.size());
                std::string_view sv(msg);
                // Reset backoff only on first successful data frame — proves auth worked.
                self->backoff_ = std::chrono::milliseconds(250);
                spdlog::trace("ws raw frame: {} bytes (text={})", bytes,
                    s.got_text());
                if (self->on_frame_) {
                    try { self->on_frame_(sv); }
                    catch (const std::exception& e) {
                        spdlog::error("frame handler threw: {}", e.what());
                    }
                    catch (...) {
                        spdlog::error("frame handler threw unknown exception");
                    }
                }
                self->do_read();
            });
        });
    }
    void drain_tx() {
        if (!has_stream() || !connected_.load() || tx_queue_.empty()) {
            writing_ = false; return;
        }
        writing_ = true;
        auto self = shared_from_this();
        std::string& msg = tx_queue_.front();
        with_stream([&](auto& s) {
            s.text(true);
            s.async_write(asio::buffer(msg),
                [self](beast::error_code ec, std::size_t) {
                    if (ec) { self->writing_ = false;
                              self->on_socket_error("write: " + ec.message()); return; }
                    self->tx_queue_.pop_front();
                    self->drain_tx();
                });
        });
    }
    void on_socket_error(std::string r) {
        // Re-entrancy gate: ONE socket failure surfaces on BOTH the pending
        // async_read AND async_write. Only the FIRST caller tears down; the
        // second (which arrives as operation_aborted once we close the socket
        // below) must do nothing — otherwise we'd destroy / SSL-shutdown the TLS
        // stream while the other op is still mid-flight on it, which corrupts
        // OpenSSL's per-connection state and faults inside libcrypto.
        if (tearing_down_) return;
        tearing_down_ = true;
        connected_.store(false);
        // stop() intentionally closes the raw socket to cancel pending I/O.
        // Operation canceled is expected teardown, not a feed outage; suppress
        // disconnect/retry callbacks and warning noise in that path.
        if (stopping_.load()) return;
        writing_ = false;
        tx_queue_.clear();
        notify("disconnected: " + r);
        spdlog::warn("ws disconnected ({}): {}", url_, r);
        // Break the connection WITHOUT touching the SSL engine here. Closing the
        // raw TCP socket forces any other outstanding op to complete with
        // operation_aborted (it re-enters this fn and no-ops via the gate above),
        // unwinding cleanly. The old stream is destroyed later in
        // schedule_connect(), after the reconnect-timer gap guarantees all pending
        // handlers have drained — never synchronously from inside an op handler.
        with_stream([](auto& s) {
            beast::error_code ec;
            beast::get_lowest_layer(s).socket().close(ec);
        });
        retry(std::move(r));
    }
    void close_ws() {
        tx_queue_.clear();
        writing_ = false;
        if (!has_stream()) return;
        beast::error_code ec;
        if (stream_) {
            stream_->next_layer().shutdown(ec);  // TLS close_notify
            beast::get_lowest_layer(*stream_).socket().close(ec);
        } else {
            plain_->next_layer().socket().shutdown(tcp::socket::shutdown_both, ec);
            beast::get_lowest_layer(*plain_).socket().close(ec);
        }
        stream_.reset();
        plain_.reset();
    }
    void retry(std::string r) {
        if (stopping_.load()) return;
        connected_.store(false);
        tx_queue_.clear();
        auto delay = backoff_;
        backoff_ = std::min<std::chrono::milliseconds>(backoff_ * 2,
                                                       std::chrono::seconds(10));
        spdlog::warn("ws retry {} in {}ms ({})", url_, delay.count(), r);
        reconnect_timer_ = std::make_unique<asio::steady_timer>(ioc_);
        reconnect_timer_->expires_after(delay);
        auto self = shared_from_this();
        reconnect_timer_->async_wait([self](beast::error_code ec) {
            if (ec) return;
            self->schedule_connect();
        });
    }

    std::string  url_;
    ParsedUrl    url_p_{};
    OnFrame      on_frame_;
    OnState      on_state_;
    asio::io_context           ioc_;
    ssl::context               ssl_ctx_;
    tcp::resolver              resolver_;
    std::unique_ptr<Stream>      stream_;  // wss:// — at most one of these
    std::unique_ptr<PlainStream> plain_;   // ws://  — two is ever non-null
    beast::flat_buffer         rx_buffer_;
    std::deque<std::string>    tx_queue_;
    bool                       writing_ = false;
    std::unique_ptr<asio::steady_timer> reconnect_timer_;
    std::chrono::milliseconds  backoff_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> stopping_{false};
    bool              tearing_down_ = false;  // gates re-entrant on_socket_error
    std::thread       th_;
};

WsClient::WsClient(std::string url, OnFrame on_frame, OnState on_state)
    : url_(std::move(url)),
      on_frame_(std::move(on_frame)),
      on_state_(std::move(on_state)) {}

WsClient::~WsClient() { stop(); }

void WsClient::start() {
    if (running_.exchange(true)) return;
    impl_ = std::make_shared<Impl>(url_, on_frame_, on_state_);
    impl_->start();
}
void WsClient::stop() {
    if (!running_.exchange(false)) return;
    if (impl_) impl_->stop();
    impl_.reset();
}
void WsClient::send(std::string text) {
    if (impl_) impl_->send(std::move(text));
}
bool WsClient::connected() const noexcept {
    return impl_ && impl_->connected();
}

} // namespace vp
