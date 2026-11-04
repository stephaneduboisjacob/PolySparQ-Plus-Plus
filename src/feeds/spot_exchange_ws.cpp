#include "feeds/spot_exchange_ws.hpp"

#include "core/clock.hpp"
#include "net/ws_client.hpp"

#include <simdjson.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>

namespace vp {
namespace {

double decimal(simdjson::dom::element value) {
    std::string_view text;
    if (value.get(text) == simdjson::SUCCESS) {
        try { return std::stod(std::string(text)); } catch (...) { return 0.0; }
    }
    double number = 0.0;
    return value.get(number) == simdjson::SUCCESS ? number : 0.0;
}

std::int64_t integer(simdjson::dom::element value) {
    std::string_view text;
    if (value.get(text) == simdjson::SUCCESS) {
        std::int64_t result = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
        return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? result : 0;
    }
    std::int64_t result = 0;
    return value.get(result) == simdjson::SUCCESS ? result : 0;
}

std::int64_t iso8601_to_ms(std::string_view text) {
    if (text.size() < 19) return 0;
    std::tm tm{};
    int year = 0, month = 0, day = 0, hour = 0, minute = 0;
    double seconds = 0.0;
    if (std::sscanf(std::string(text).c_str(), "%d-%d-%dT%d:%d:%lf",
                    &year, &month, &day, &hour, &minute, &seconds) != 6) return 0;
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    const auto base = timegm(&tm);
    return base < 0 ? 0 : static_cast<std::int64_t>(base) * 1000 +
        static_cast<std::int64_t>(seconds * 1000.0);
}

std::string asset_from_symbol(std::string_view symbol) {
    if (symbol.find("BTC") != std::string_view::npos ||
        symbol.find("XBT") != std::string_view::npos ||
        symbol.find("btc") != std::string_view::npos ||
        symbol.find("xbt") != std::string_view::npos) return "BTC";
    if (symbol.find("ETH") != std::string_view::npos ||
        symbol.find("eth") != std::string_view::npos) return "ETH";
    if (symbol.find("SOL") != std::string_view::npos ||
        symbol.find("sol") != std::string_view::npos) return "SOL";
    if (symbol.find("XRP") != std::string_view::npos ||
        symbol.find("xrp") != std::string_view::npos) return "XRP";
    return {};
}

bool string_field(simdjson::dom::element object, const char* key,
                  std::string_view& value) {
    return object[key].get(value) == simdjson::SUCCESS;
}

void read_quote(SpotExchangeTick& tick, double bid, double bid_size,
                double ask, double ask_size, std::int64_t ts_ms,
                const SpotExchangeFeed::OnTick& on_tick) {
    if (tick.asset.empty() || bid <= 0 || ask < bid) return;
    tick.bid = bid;
    tick.bid_size = std::max(0.0, bid_size);
    tick.ask = ask;
    tick.ask_size = std::max(0.0, ask_size);
    tick.price = 0.5 * (bid + ask);
    tick.exchange_ts_ms = ts_ms > 0 ? ts_ms : tick.received_ts_ms;
    if (on_tick) on_tick(tick);
}

void parse_kraken(simdjson::dom::element doc,
                  const std::string& venue, std::int64_t received_ms,
                  std::int64_t mono_ns,
                  const SpotExchangeFeed::OnTick& on_tick) {
    std::string_view channel;
    if (!string_field(doc, "channel", channel) || channel != "ticker") return;
    simdjson::dom::array rows;
    if (doc["data"].get(rows) != simdjson::SUCCESS) return;
    for (auto row : rows) {
        std::string_view symbol, timestamp;
        if (!string_field(row, "symbol", symbol)) continue;
        SpotExchangeTick tick;
        tick.venue = venue;
        tick.pair = std::string(symbol);
        tick.asset = asset_from_symbol(symbol);
        tick.received_ts_ms = received_ms;
        tick.received_mono_ns = mono_ns;
        const auto source_ms = string_field(row, "timestamp", timestamp)
            ? iso8601_to_ms(timestamp) : 0;
        read_quote(tick, decimal(row["bid"]), decimal(row["bid_qty"]),
                   decimal(row["ask"]), decimal(row["ask_qty"]), source_ms, on_tick);
    }
}

void parse_gemini(simdjson::dom::element root,
                  const std::string& venue, std::int64_t received_ms,
                  std::int64_t mono_ns,
                  const SpotExchangeFeed::OnTick& on_tick) {
    simdjson::dom::element data = root;
    if (root["data"].get(data) != simdjson::SUCCESS) data = root;
    std::string_view stream, symbol;
    (void)string_field(root, "stream", stream);
    if (string_field(data, "s", symbol) || string_field(data, "symbol", symbol)) {
        // Gemini's multiplexed symbols are lower-case, e.g. btcusd.
    } else if (stream.size() >= 6) {
        symbol = stream.substr(0, stream.find('@'));
    }
    if (stream.find("bookTicker") == std::string_view::npos && decimal(data["b"]) <= 0) return;
    if (symbol.empty()) symbol = "btcusd";
    const auto asset = asset_from_symbol(symbol);
    if (asset != "BTC") return; // Predator's validated Gemini adapter supports BTC-USD.
    SpotExchangeTick tick;
    tick.venue = venue;
    tick.asset = asset;
    tick.pair = "BTC-USD";
    tick.received_ts_ms = received_ms;
    tick.received_mono_ns = mono_ns;
    // Predator's Gemini adapter records the exchange event time in `E` as
    // epoch nanoseconds; normalize it to the millisecond contract used by the
    // other feeds and by vp.spot_exchange_ticks.
    auto ts = integer(data["E"]);
    if (ts > 100'000'000'000'000LL) ts /= 1'000'000LL;
    read_quote(tick, decimal(data["b"]), decimal(data["B"]),
               decimal(data["a"]), decimal(data["A"]), ts, on_tick);
}

bool first_level(simdjson::dom::element side, double& price, double& size) {
    simdjson::dom::array levels;
    if (side.get(levels) != simdjson::SUCCESS || levels.begin() == levels.end()) return false;
    simdjson::dom::array level;
    if ((*levels.begin()).get(level) != simdjson::SUCCESS || level.size() < 2) return false;
    price = decimal(level.at(0));
    size = decimal(level.at(1));
    return price > 0;
}

void parse_okx(simdjson::dom::element root,
               const std::string& venue, std::int64_t received_ms,
               std::int64_t mono_ns,
               const SpotExchangeFeed::OnTick& on_tick) {
    std::string_view channel, instrument;
    if (!string_field(root["arg"], "channel", channel) || channel != "books5" ||
        !string_field(root["arg"], "instId", instrument)) return;
    simdjson::dom::array rows;
    if (root["data"].get(rows) != simdjson::SUCCESS) return;
    for (auto row : rows) {
        double bid = 0.0, bid_size = 0.0, ask = 0.0, ask_size = 0.0;
        if (!first_level(row["bids"], bid, bid_size) ||
            !first_level(row["asks"], ask, ask_size)) continue;
        SpotExchangeTick tick;
        tick.venue = venue;
        tick.asset = asset_from_symbol(instrument);
        tick.pair = std::string(instrument);
        tick.received_ts_ms = received_ms;
        tick.received_mono_ns = mono_ns;
        const auto source_ms = integer(row["ts"]);
        read_quote(tick, bid, bid_size, ask, ask_size, source_ms, on_tick);
    }
}

} // namespace

SpotExchangeFeed::SpotExchangeFeed(std::string venue, std::string ws_url,
                                   std::vector<std::string> instruments,
                                   OnTick on_tick)
    : venue_(std::move(venue)), ws_url_(std::move(ws_url)),
      instruments_(std::move(instruments)), on_tick_(std::move(on_tick)) {
    spdlog::info("{} spot BBO feed: {}", venue_, ws_url_);
    ws_ = std::make_unique<WsClient>(
        ws_url_, [this](std::string_view frame) { on_frame(frame); },
        [this](const std::string& state) {
            spdlog::info("{} spot feed state: {}", venue_, state);
            if (state == "connected") send_subscribe();
        });
}

SpotExchangeFeed::~SpotExchangeFeed() { stop(); }
void SpotExchangeFeed::start() { if (ws_) ws_->start(); }
void SpotExchangeFeed::stop() { if (ws_) ws_->stop(); }

void SpotExchangeFeed::send_subscribe() {
    if (!ws_) return;
    std::string message;
    if (venue_ == "kraken") {
        message = R"({"method":"subscribe","params":{"channel":"ticker","symbol":[)";
        for (std::size_t i = 0; i < instruments_.size(); ++i) {
            if (i) message += ',';
            message += '"' + instruments_[i] + '"';
        }
        message += R"(],"event_trigger":"bbo","snapshot":true},"req_id":1})";
    } else if (venue_ == "gemini") {
        message = R"({"id":"1","method":"subscribe","params":["btcusd@bookTicker"]})";
    } else if (venue_ == "okx") {
        message = R"({"op":"subscribe","args":[)";
        for (std::size_t i = 0; i < instruments_.size(); ++i) {
            if (i) message += ',';
            message += R"({"channel":"books5","instId":")" + instruments_[i] + R"("})";
        }
        message += "]}";
    }
    if (!message.empty()) ws_->send(std::move(message));
}

void SpotExchangeFeed::on_frame(std::string_view frame) {
    thread_local simdjson::dom::parser parser;
    simdjson::padded_string padded(frame.data(), frame.size());
    simdjson::dom::element doc;
    if (parser.parse(padded).get(doc)) return;
    const auto received_ms = Clock::wall_ns() / 1'000'000LL;
    const auto mono_ns = Clock::mono_ns();
    if (venue_ == "kraken") parse_kraken(doc, venue_, received_ms, mono_ns, on_tick_);
    else if (venue_ == "gemini") parse_gemini(doc, venue_, received_ms, mono_ns, on_tick_);
    else if (venue_ == "okx") parse_okx(doc, venue_, received_ms, mono_ns, on_tick_);
}

} // namespace vp
