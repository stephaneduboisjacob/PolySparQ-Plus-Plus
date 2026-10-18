// feed_proxy.cpp — C++ single-upstream WebSocket fan-out for the PolySparQ++ fleet.
//
// One TLS socket per venue (reusing vp::WsClient — same battle-tested reconnect
// path the bots use), rebroadcast to every local bot over a plain ws:// loopback
// server. Drop-in replacement for scripts/feed_proxy.py: identical ports and
// dumb-relay semantics, but compiled — lifting the single-core ceiling that
// capped the Python proxy around ~13 instances.
//
//   pmmd (CLOB)   ws://127.0.0.1:9101  <- wss://ws-subscriptions-clob.polymarket.com/ws/market
//   rtds (oracle) ws://127.0.0.1:9102  <- wss://ws-live-data.polymarket.com
//   binance       ws://127.0.0.1:9103  <- wss://stream.binance.com:9443/stream?streams=...
//   coinbase      ws://127.0.0.1:9104  <- wss://ws-feed.exchange.coinbase.com
//
// Design:
//   - upstream frame  -> fan out verbatim (as text) to every connected client.
//   - pm-md subscriptions are reference-counted per downstream client. Only
//     union changes are sent upstream, so a reconnect cannot duplicate the same
//     100+ token subscription on a long-lived venue socket.
//   - other client messages use a short exact-duplicate window so N identical
//     subscribes/PINGs don't N-plicate.
//   - upstream drop    -> every client is closed; the bots' reconnect logic owns
//     re-subscription and reseeds a clean book.
//   - client connects while upstream is down -> rejected immediately.
//   - per-client outbound queue is BOUNDED: a bot that can't keep up is dropped,
//     never buffered without limit (hard memory cap — the Python broadcast()
//     leak that this design fixes).
//
// Threading: each upstream owns its own io_context/thread (inside WsClient). The
// downstream server runs a small io_context thread pool; every client session
// runs on its own strand, so per-session state needs no locking and the hub's
// session set is guarded by one short-held mutex.

#include "net/ws_client.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <simdjson.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace beast = boost::beast;
namespace bws   = boost::beast::websocket;
namespace asio  = boost::asio;
using tcp       = boost::asio::ip::tcp;
using steady_clock = std::chrono::steady_clock;
static int64_t steady_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        steady_clock::now().time_since_epoch()).count();
}

namespace {

// Per-client outbound backlog cap. A healthy local bot drains instantly; this
// only fills when one stalls (a blocking startup step, a wedged socket). At the
// pm-md firehose (~1500 fps) this is ~2.7s of grace — past it we cut the client
// loose rather than buffer unboundedly. The bot reconnects + reseeds a clean
// book within a second. Memory is bounded by both frame count and total bytes.
// Raised from 1024 → 4096: the full-snapshot burst on reconnect at oracle window
// opens (88 tokens × large snap) was consistently exceeding 1024, causing a
// reconnect storm → stale books → FAK kills on stale prices.
constexpr std::size_t kMaxClientQueue      = 4096;
constexpr std::size_t kMaxClientQueueBytes = 32 * 1024 * 1024;
constexpr std::size_t kMaxPmmdFrameBytes   = 8 * 1024 * 1024;
constexpr std::size_t kMaxClientInputBytes = 1024 * 1024;
constexpr auto        kDedupWindow    = std::chrono::seconds(4);

struct Feed { const char* name; const char* url; std::uint16_t port; };

const std::vector<Feed> kFeeds = {
    {"pmmd",     "wss://ws-subscriptions-clob.polymarket.com/ws/market", 9101},
    {"rtds",     "wss://ws-live-data.polymarket.com",                    9102},
    {"binance",  "wss://stream.binance.com:9443/stream?streams="
                 "btcusdt@trade/ethusdt@trade/solusdt@trade/xrpusdt@trade/"
                 "btcusdt@bookTicker/ethusdt@bookTicker/solusdt@bookTicker/"
                 "xrpusdt@bookTicker", 9103},
    {"coinbase", "wss://ws-feed.exchange.coinbase.com",                  9104},
};

class FeedHub;

// One downstream bot connection. Every member runs on ws_'s own strand, so no
// per-session locking is required.
class ClientSession : public std::enable_shared_from_this<ClientSession> {
public:
    ClientSession(tcp::socket&& sock, FeedHub& hub)
        : ws_(std::move(sock)), hub_(hub) {}

    void run();   // dispatched onto the session strand → handshake → read loop
    void enqueue(std::shared_ptr<const std::string> frame);  // any thread
    void close(const char* why);                             // any thread
    asio::any_io_executor executor() { return ws_.get_executor(); }
    const std::string& remote() const { return remote_; }

private:
    void on_run();
    void on_accept(beast::error_code ec);
    void do_read();
    void on_read(beast::error_code ec, std::size_t n);
    void do_write();
    void on_write(beast::error_code ec, std::size_t n);
    void do_close(const char* why);   // on strand

    bws::stream<beast::tcp_stream> ws_;
    FeedHub& hub_;
    beast::flat_buffer rx_;
    std::deque<std::shared_ptr<const std::string>> out_;
    std::size_t out_bytes_ = 0;
    bool writing_ = false;
    bool closed_  = false;
    std::string remote_;
};

class FeedHub {
public:
    FeedHub(asio::io_context& ioc, const Feed& f)
        : ioc_(ioc),
          acceptor_(asio::make_strand(ioc)),
          name_(f.name), url_(f.url), port_(f.port) {}

    // Returns false if the loopback port can't be bound (caller should abort so
    // systemd restarts us once the port is free, rather than spin).
    bool start() {
        if (!open_acceptor()) return false;
        // Upstream: own thread inside WsClient. on_frame fans out; on_state
        // gates client admission and cuts everyone loose on a drop.
        upstream_ = std::make_unique<vp::WsClient>(
            url_,
            [this](std::string_view frame) { on_upstream_frame(frame); },
            [this](const std::string& state) { on_upstream_state(state); });
        upstream_->start();
        do_accept();
        return true;
    }

    void stop() { if (upstream_) upstream_->stop(); }

    const std::string& name() const { return name_; }
    bool upstream_up() const { return upstream_up_.load(std::memory_order_relaxed); }

    // ── called from session strands (under brief hub lock) ──
    void add_session(const std::shared_ptr<ClientSession>& s) {
        std::size_t n;
        { std::lock_guard<std::mutex> lk(mu_); sessions_.push_back(s); n = sessions_.size(); }
        spdlog::info("{}: client {} joined ({} connected)", name_, s->remote(), n);
    }
    void remove_session(const std::shared_ptr<ClientSession>& s) {
        std::size_t n;
        bool removed = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = std::find(sessions_.begin(), sessions_.end(), s);
            if (it != sessions_.end()) {
                sessions_.erase(it);
                removed = true;
            }
            n = sessions_.size();
        }
        unregister_market_session(s.get());
        if (removed) spdlog::info("{}: client left ({} connected)", name_, n);
    }
    void note_dropped(const std::string& who, std::size_t frames, std::size_t bytes) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        spdlog::warn("{}: client {} too slow (backlog {} frames / {} bytes) — dropping",
                     name_, who, frames, bytes);
    }

    // Process pm-md subscription commands as per-client desired state. Everything
    // else is forwarded through the short generic dedup window.
    void handle_client_message(const std::shared_ptr<ClientSession>& session,
                               const std::string& msg) {
        client_msgs_.fetch_add(1, std::memory_order_relaxed);
        if (name_ == "pmmd" && handle_market_command(session.get(), msg)) return;

        {
            std::lock_guard<std::mutex> lk(dedup_mu_);
            auto now = steady_clock::now();
            for (auto it = dedup_.begin(); it != dedup_.end();)
                it = (now - it->second >= kDedupWindow) ? dedup_.erase(it) : std::next(it);
            if (dedup_.find(msg) != dedup_.end()) {
                dedup_count_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            dedup_.emplace(msg, now);
        }
        if (upstream_ && upstream_up()) {
            fwd_.fetch_add(1, std::memory_order_relaxed);
            upstream_->send(msg);
        }
    }

    // Recycle a half-dead upstream: a websocket that stops delivering frames
    // without ever closing (the RTDS outage of Sep 06 16:42-16:47 starved the
    // whole fleet while the socket looked healthy). On a real disconnect,
    // on_upstream_state() also drops downstreams so they re-subscribe.
    void check_stall() {
        if (!upstream_up_.load(std::memory_order_relaxed)) return;
        const int64_t last = last_upstream_frame_ms_.load(std::memory_order_relaxed);
        const int64_t now = steady_now_ms();
        if (last == 0) {
            last_upstream_frame_ms_.store(now, std::memory_order_relaxed);
            return;
        }
        if (now - last > 15000) {
            spdlog::warn("{}: upstream silent for {:.0f}s — forcing reconnect",
                         name_, (now - last) / 1000.0);
            last_upstream_frame_ms_.store(now, std::memory_order_relaxed);
            // stop() is an intentional shutdown, so WsClient does not report
            // an upstream-disconnected state. Drop downstreams explicitly:
            // otherwise their one-time subscribe message is lost when the
            // upstream socket is recreated, leaving a connected but unsubscribed
            // RTDS feed until a bot happens to reconnect on its own.
            reset_downstream_clients("upstream stalled");
            if (upstream_) {
                upstream_->stop();
                upstream_->start();
            }
        }
    }

    void log_stats() {
        std::size_t n, active_tokens = 0;
        { std::lock_guard<std::mutex> lk(mu_); n = sessions_.size(); }
        if (name_ == "pmmd") {
            std::lock_guard<std::mutex> lk(market_mu_);
            active_tokens = market_refcounts_.size();
        }
        spdlog::info("{}: clients={} up_frames={} client_msgs={} fwd={} dedup={} "
                     "dropped={} invalid_frames={} oversized_frames={} active_tokens={}",
                     name_, n, up_frames_.load(), client_msgs_.load(),
                     fwd_.load(), dedup_count_.load(), dropped_.load(),
                     invalid_frames_.load(), oversized_frames_.load(), active_tokens);
    }

private:
    enum class MarketCommandKind { Replace, Subscribe, Unsubscribe };

    struct MarketCommand {
        MarketCommandKind kind = MarketCommandKind::Subscribe;
        std::unordered_set<std::string> tokens;
    };

    static std::optional<MarketCommand> parse_market_command(const std::string& msg) {
        if (msg.empty() || msg.front() != '{') return std::nullopt;
        thread_local simdjson::dom::parser parser;
        simdjson::padded_string padded(msg);
        simdjson::dom::element doc;
        if (parser.parse(padded).get(doc) != simdjson::SUCCESS) return std::nullopt;

        MarketCommand cmd;
        std::string_view value;
        if (doc["type"].get(value) == simdjson::SUCCESS && value == "market") {
            cmd.kind = MarketCommandKind::Replace;
        } else if (doc["operation"].get(value) == simdjson::SUCCESS &&
                   value == "subscribe") {
            cmd.kind = MarketCommandKind::Subscribe;
        } else if (value == "unsubscribe") {
            cmd.kind = MarketCommandKind::Unsubscribe;
        } else {
            return std::nullopt;
        }

        simdjson::dom::array ids;
        if (doc["assets_ids"].get(ids) != simdjson::SUCCESS) return cmd;
        for (auto id : ids) {
            std::string_view token;
            if (id.get(token) == simdjson::SUCCESS && !token.empty())
                cmd.tokens.emplace(token);
        }
        return cmd;
    }

    void send_market_batches(const std::vector<std::string>& tokens,
                             MarketCommandKind kind) {
        if (tokens.empty() || !upstream_ || !upstream_up()) return;
        constexpr std::size_t kBatchSize = 500;
        for (std::size_t begin = 0; begin < tokens.size(); begin += kBatchSize) {
            const std::size_t end = std::min(begin + kBatchSize, tokens.size());
            const bool initial = kind == MarketCommandKind::Replace && begin == 0;
            std::string msg = initial
                ? R"({"type":"market","custom_feature_enabled":true,"assets_ids":[)"
                : (kind == MarketCommandKind::Unsubscribe
                    ? R"({"operation":"unsubscribe","assets_ids":[)"
                    : R"({"operation":"subscribe","assets_ids":[)");
            for (std::size_t i = begin; i < end; ++i) {
                if (i != begin) msg += ',';
                msg += '"'; msg += tokens[i]; msg += '"';
            }
            msg += "]}";
            fwd_.fetch_add(1, std::memory_order_relaxed);
            upstream_->send(std::move(msg));
        }
    }

    void apply_market_command(ClientSession* session, const MarketCommand& cmd) {
        std::lock_guard<std::mutex> lk(market_mu_);
        auto& held = market_sessions_[session];
        std::unordered_set<std::string> target = held;
        if (cmd.kind == MarketCommandKind::Replace) {
            target = cmd.tokens;
        } else if (cmd.kind == MarketCommandKind::Subscribe) {
            target.insert(cmd.tokens.begin(), cmd.tokens.end());
        } else {
            for (const auto& token : cmd.tokens) target.erase(token);
        }

        std::vector<std::string> add, drop;
        for (const auto& token : held) {
            if (target.count(token)) continue;
            auto it = market_refcounts_.find(token);
            if (it != market_refcounts_.end() && --it->second == 0) {
                market_refcounts_.erase(it);
                drop.push_back(token);
            }
        }
        for (const auto& token : target) {
            if (held.count(token)) continue;
            auto [it, inserted] = market_refcounts_.try_emplace(token, 0);
            if (it->second++ == 0) add.push_back(token);
        }
        held = std::move(target);
        if (held.empty()) market_sessions_.erase(session);

        std::sort(add.begin(), add.end());
        std::sort(drop.begin(), drop.end());
        if (!add.empty() || !drop.empty()) {
            spdlog::info("pmmd: subscription union +{} -{} ({} active tokens, {} clients)",
                         add.size(), drop.size(), market_refcounts_.size(),
                         market_sessions_.size());
        }
        if (!market_upstream_initialized_ && !market_refcounts_.empty()) {
            std::vector<std::string> all;
            all.reserve(market_refcounts_.size());
            for (const auto& [token, count] : market_refcounts_) all.push_back(token);
            std::sort(all.begin(), all.end());
            market_upstream_initialized_ = true;
            send_market_batches(all, MarketCommandKind::Replace);
        } else {
            send_market_batches(add, MarketCommandKind::Subscribe);
            send_market_batches(drop, MarketCommandKind::Unsubscribe);
        }
        if (add.empty() && drop.empty())
            dedup_count_.fetch_add(1, std::memory_order_relaxed);
    }

    bool handle_market_command(ClientSession* session, const std::string& msg) {
        auto cmd = parse_market_command(msg);
        if (!cmd) return false;
        apply_market_command(session, *cmd);
        return true;
    }

    void unregister_market_session(ClientSession* session) {
        if (name_ != "pmmd") return;
        MarketCommand empty;
        empty.kind = MarketCommandKind::Replace;
        apply_market_command(session, empty);
    }

    void clear_market_state() {
        if (name_ != "pmmd") return;
        std::lock_guard<std::mutex> lk(market_mu_);
        market_sessions_.clear();
        market_refcounts_.clear();
        market_upstream_initialized_ = false;
    }

    void on_upstream_frame(std::string_view frame) {
        up_frames_.fetch_add(1, std::memory_order_relaxed);
        last_upstream_frame_ms_.store(steady_now_ms(), std::memory_order_relaxed);
        if (name_ == "pmmd") {
            if (frame.size() > kMaxPmmdFrameBytes) {
                oversized_frames_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            const auto first = frame.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos) return;
            const char c = frame[first];
            if (c != '{' && c != '[' && frame != "PONG" && frame != "pong") {
                const auto count = invalid_frames_.fetch_add(1, std::memory_order_relaxed) + 1;
                if (count == 1 || count % 100 == 0)
                    spdlog::warn("pmmd: dropped malformed upstream frame ({} bytes, first='{}', count={})",
                                 frame.size(), c, count);
                return;
            }
        }
        broadcast(std::make_shared<const std::string>(frame));
    }

    void broadcast(std::shared_ptr<const std::string> frame) {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& s : sessions_) s->enqueue(frame);
    }

    void on_upstream_state(const std::string& state) {
        if (state == "connected") {
            upstream_up_.store(true, std::memory_order_relaxed);
            last_upstream_frame_ms_.store(steady_now_ms(), std::memory_order_relaxed);
            spdlog::info("{}: upstream connected", name_);
            return;
        }
        upstream_up_.store(false, std::memory_order_relaxed);
        spdlog::warn("{}: upstream {}", name_, state);
        reset_downstream_clients("upstream lost");
    }

    void reset_downstream_clients(const char* reason) {
        upstream_up_.store(false, std::memory_order_relaxed);
        clear_market_state();
        {
            std::lock_guard<std::mutex> lk(dedup_mu_);
            dedup_.clear();
        }
        // Their reconnect logic owns re-subscription and reseeds feed state.
        std::vector<std::shared_ptr<ClientSession>> snapshot;
        { std::lock_guard<std::mutex> lk(mu_); snapshot.swap(sessions_); }
        if (!snapshot.empty())
            spdlog::warn("{}: reconnecting {} downstream client(s) ({})",
                         name_, snapshot.size(), reason);
        for (auto& s : snapshot) s->close(reason);
    }

    bool open_acceptor() {
        beast::error_code ec;
        tcp::endpoint ep(asio::ip::make_address("127.0.0.1"), port_);
        acceptor_.open(ep.protocol(), ec);
        acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
        acceptor_.bind(ep, ec);
        if (ec) { spdlog::error("{}: bind :{} failed: {}", name_, port_, ec.message()); return false; }
        acceptor_.listen(asio::socket_base::max_listen_connections, ec);
        if (ec) { spdlog::error("{}: listen failed: {}", name_, ec.message()); return false; }
        listening_ = true;
        spdlog::info("{}: listening on ws://127.0.0.1:{} for {}", name_, port_, url_);
        return true;
    }

    void do_accept() {
        if (!listening_ || !acceptor_.is_open()) return;
        // Each connection gets its own strand (standard Beast pattern).
        acceptor_.async_accept(asio::make_strand(ioc_),
            [this](beast::error_code ec, tcp::socket sock) { on_accept(ec, std::move(sock)); });
    }

    void on_accept(beast::error_code ec, tcp::socket sock) {
        if (ec) {
            // operation_aborted = we're shutting down. Any other error on a still-
            // open listener is transient (e.g. EMFILE) — re-arm. If the listener
            // is gone, stop, so a broken acceptor can't spin a tight error loop.
            if (ec == asio::error::operation_aborted) return;
            spdlog::warn("{}: accept: {}", name_, ec.message());
            do_accept();
            return;
        }
        if (!upstream_up()) {
            beast::error_code ic; sock.close(ic);   // reject; bot retries when upstream's back
        } else {
            std::make_shared<ClientSession>(std::move(sock), *this)->run();
        }
        do_accept();
    }

    asio::io_context&                    ioc_;
    tcp::acceptor                        acceptor_;
    std::unique_ptr<vp::WsClient>        upstream_;
    std::string                          name_, url_;
    std::uint16_t                        port_;
    bool                                 listening_ = false;
    std::atomic<bool>                    upstream_up_{false};

    std::mutex                           mu_;       // guards sessions_
    std::vector<std::shared_ptr<ClientSession>> sessions_;
    std::mutex                           dedup_mu_; // guards dedup_
    std::unordered_map<std::string, steady_clock::time_point> dedup_;
    std::mutex                           market_mu_;
    std::unordered_map<ClientSession*, std::unordered_set<std::string>> market_sessions_;
    std::unordered_map<std::string, std::size_t> market_refcounts_;
    bool                                 market_upstream_initialized_ = false;

    std::atomic<int64_t> last_upstream_frame_ms_{0};
    std::atomic<std::uint64_t> up_frames_{0}, client_msgs_{0}, fwd_{0},
                               dedup_count_{0}, dropped_{0}, invalid_frames_{0},
                               oversized_frames_{0};
};

// ── ClientSession impl ──────────────────────────────────────────────────────

void ClientSession::run() {
    // Hop onto our own strand before touching ws_.
    asio::dispatch(ws_.get_executor(),
        [self = shared_from_this()] { self->on_run(); });
}

void ClientSession::on_run() {
    beast::error_code ec;
    auto ep = beast::get_lowest_layer(ws_).socket().remote_endpoint(ec);
    remote_ = ec ? "?" : (ep.address().to_string() + ":" + std::to_string(ep.port()));
    ws_.set_option(bws::stream_base::timeout::suggested(beast::role_type::server));
    ws_.read_message_max(kMaxClientInputBytes);
    ws_.set_option(bws::stream_base::decorator([](bws::response_type& res) {
        res.set(beast::http::field::server, "polypulse-feedproxy");
    }));
    ws_.async_accept([self = shared_from_this()](beast::error_code e) { self->on_accept(e); });
}

void ClientSession::on_accept(beast::error_code ec) {
    if (ec) { do_close("handshake"); return; }
    hub_.add_session(shared_from_this());
    do_read();
}

void ClientSession::do_read() {
    ws_.async_read(rx_, [self = shared_from_this()](beast::error_code ec, std::size_t n) {
        self->on_read(ec, n);
    });
}

void ClientSession::on_read(beast::error_code ec, std::size_t /*n*/) {
    if (ec) { do_close("read"); return; }
    std::string msg = beast::buffers_to_string(rx_.data());
    rx_.consume(rx_.size());
    hub_.handle_client_message(shared_from_this(), msg);
    do_read();
}

void ClientSession::enqueue(std::shared_ptr<const std::string> frame) {
    asio::post(ws_.get_executor(),
        [self = shared_from_this(), frame = std::move(frame)]() mutable {
            if (self->closed_) return;
            const std::size_t next_bytes = self->out_bytes_ + frame->size();
            if (self->out_.size() >= kMaxClientQueue ||
                next_bytes > kMaxClientQueueBytes) {
                self->hub_.note_dropped(self->remote_, self->out_.size(),
                                        next_bytes);
                self->do_close("slow consumer");
                return;
            }
            self->out_bytes_ = next_bytes;
            self->out_.push_back(std::move(frame));
            if (!self->writing_) self->do_write();
        });
}

void ClientSession::do_write() {
    if (out_.empty()) { writing_ = false; return; }
    writing_ = true;
    ws_.text(true);
    // Pin the frame's lifetime to the write. asio::buffer() is a non-owning
    // view into the string; the only owning ref is the shared_ptr in out_. If
    // the upstream drops mid-write, do_close() runs out_.clear() on this same
    // strand and would free the buffer the socket is still reading from — a
    // heap-use-after-free (SIGSEGV/SIGABRT at disconnect). Capturing the
    // shared_ptr in the completion handler keeps the buffer alive until the
    // async_write fully completes (or aborts), regardless of out_.clear().
    auto frame = out_.front();
    ws_.async_write(asio::buffer(*frame),
        [self = shared_from_this(), frame](beast::error_code ec, std::size_t n) {
            self->on_write(ec, n);
        });
}

void ClientSession::on_write(beast::error_code ec, std::size_t /*n*/) {
    if (closed_) return;                      // do_close() already drained out_
    if (ec) { do_close("write"); return; }
    out_bytes_ -= std::min(out_bytes_, out_.front()->size());
    out_.pop_front();
    do_write();
}

void ClientSession::close(const char* why) {
    // Marshal onto our strand; safe to call from any thread.
    asio::post(ws_.get_executor(),
        [self = shared_from_this(), why] { self->do_close(why); });
}

void ClientSession::do_close(const char* /*why*/) {
    if (closed_) return;
    closed_ = true;
    out_.clear();
    out_bytes_ = 0;
    hub_.remove_session(shared_from_this());
    beast::error_code ec;
    beast::get_lowest_layer(ws_).socket().close(ec);  // hard close; bot reconnects + reseeds
}

void schedule_stats(asio::steady_timer& timer,
                    std::vector<std::unique_ptr<FeedHub>>& hubs) {
    timer.expires_after(std::chrono::seconds(5));
    timer.async_wait([&timer, &hubs](beast::error_code ec) {
        if (ec) return;
        static int64_t ticks = 0;
        ++ticks;
        for (auto& h : hubs) h->check_stall();
        if (ticks % 12 == 0)
            for (auto& h : hubs) h->log_stats();
        schedule_stats(timer, hubs);
    });
}

} // namespace

int main() {
    auto logger = spdlog::stdout_logger_mt("feedproxy");
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    spdlog::flush_on(spdlog::level::info);

    // Ports default to 9101-9104; FEEDPROXY_PORT_BASE shifts them (base+0..3) so a
    // test instance can run alongside the live one (e.g. base=9201 → 9201-9204).
    std::vector<Feed> feeds = kFeeds;
    if (const char* base = std::getenv("FEEDPROXY_PORT_BASE")) {
        std::uint16_t b = static_cast<std::uint16_t>(std::atoi(base));
        if (b > 0) for (std::size_t i = 0; i < feeds.size(); ++i)
            feeds[i].port = static_cast<std::uint16_t>(b + i);
    }

    asio::io_context ioc;
    std::vector<std::unique_ptr<FeedHub>> hubs;
    for (const auto& f : feeds) hubs.push_back(std::make_unique<FeedHub>(ioc, f));
    for (auto& h : hubs) {
        if (!h->start()) {
            spdlog::critical("feedproxy: a loopback port is unavailable — exiting "
                             "so systemd retries once it frees");
            for (auto& g : hubs) g->stop();
            return 1;
        }
    }

    asio::steady_timer stats_timer(ioc);
    schedule_stats(stats_timer, hubs);

    // Clean shutdown on SIGINT/SIGTERM (systemd stop).
    asio::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait([&](beast::error_code, int sig) {
        spdlog::warn("feedproxy: signal {} — shutting down", sig);
        for (auto& h : hubs) h->stop();
        ioc.stop();
    });

    // Downstream fan-out thread pool; 4 lets the busy pm-md feed run alongside
    // the others. Upstreams each have their own thread inside WsClient.
    const unsigned n = std::max(2u, std::min(4u, std::thread::hardware_concurrency()));
    spdlog::info("polypulse-feedproxy: starting ({} io threads, {} feeds)", n, hubs.size());
    std::vector<std::thread> pool;
    for (unsigned i = 1; i < n; ++i) pool.emplace_back([&ioc] { ioc.run(); });
    ioc.run();
    for (auto& t : pool) if (t.joinable()) t.join();
    spdlog::info("polypulse-feedproxy: stopped");
    return 0;
}
