#pragma once
#include <simdjson.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

namespace vp {
struct CockpitCommand {
    std::string instance, op, condition_id, side, expected_mode;
    std::string token_id, yes_token_id, no_token_id, direction, outcome_label, question;
    std::string client_order_id, exchange_order_id;
    bool enabled = false;
    double size_usd = 0, size_shares = 0, limit_price = 0;
    std::int64_t ts_ms = 0, window_close_ms = 0;
};
inline std::optional<CockpitCommand> parse_cockpit_command(const std::string& body) {
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    if (parser.parse(body).get(doc)) return std::nullopt;
    auto string_field = [&](const char* key) {
        std::string_view value;
        return doc[key].get_string().get(value) ? std::string{} : std::string(value);
    };
    CockpitCommand command;
    command.instance = string_field("instance");
    command.op = string_field("op");
    if (command.op.empty()) command.op = string_field("cmd"); // legacy force_sell
    if (command.instance.empty()) return std::nullopt;
    if (command.op == "autopilot") {
        if (doc["enabled"].get_bool().get(command.enabled)) return std::nullopt;
    } else if (command.op == "manual_buy" || command.op == "force_sell") {
        command.condition_id = string_field("condition_id");
        command.side = string_field("side");
        if (command.condition_id.empty() || (command.side != "UP" && command.side != "DOWN")) return std::nullopt;
        if (command.op == "manual_buy") {
            command.direction = string_field("direction");
            if (command.direction.empty()) command.direction = "BUY";
            if (command.direction != "BUY" && command.direction != "SELL") return std::nullopt;
            if (command.direction == "BUY") {
                if (doc["size_usd"].get_double().get(command.size_usd) ||
                    !std::isfinite(command.size_usd) || command.size_usd <= 0) return std::nullopt;
            } else if (doc["size_shares"].get_double().get(command.size_shares) ||
                       !std::isfinite(command.size_shares) || command.size_shares <= 0) {
                return std::nullopt;
            }
            if (doc["limit_price"].get_double().get(command.limit_price) ||
                !std::isfinite(command.limit_price) ||
                command.limit_price < 0.01 || command.limit_price > 0.99) return std::nullopt;
            command.expected_mode = string_field("expected_mode");
            if (command.expected_mode != "PAPER" && command.expected_mode != "LIVE") return std::nullopt;
            if (doc["ts"].get_int64().get(command.ts_ms) || command.ts_ms <= 0) return std::nullopt;
            command.token_id = string_field("token_id");
            command.yes_token_id = string_field("yes_token_id");
            command.no_token_id = string_field("no_token_id");
            command.outcome_label = string_field("outcome_label");
            command.question = string_field("question");
            if (!doc["window_close_ms"].get_int64().get(command.window_close_ms) &&
                command.window_close_ms < 0) return std::nullopt;
            if (command.token_id.size() > 100 || command.yes_token_id.size() > 100 ||
                command.no_token_id.size() > 100 || command.outcome_label.size() > 100 ||
                command.question.size() > 500) return std::nullopt;
            auto valid_token = [](const std::string& token) {
                return !token.empty() && token.size() <= 100 &&
                    std::all_of(token.begin(), token.end(),
                        [](unsigned char c) { return std::isdigit(c); });
            };
            if (!command.token_id.empty()) {
                if (!valid_token(command.token_id) || !valid_token(command.yes_token_id) ||
                    !valid_token(command.no_token_id) ||
                    command.token_id != (command.side == "UP" ? command.yes_token_id : command.no_token_id))
                    return std::nullopt;
            }
            if (command.direction == "SELL" && command.token_id.empty()) return std::nullopt;
        } else {
            // Keep old force-sell payloads parseable for migration, but the
            // consumer drops them because a missing timestamp is expired.
            command.expected_mode = string_field("expected_mode");
            if (doc["ts"].get_int64().get(command.ts_ms)) command.ts_ms = 0;
        }
    } else if (command.op == "cancel_order") {
        command.client_order_id = string_field("client_order_id");
        command.exchange_order_id = string_field("exchange_order_id");
        command.expected_mode = string_field("expected_mode");
        if (command.client_order_id.empty() || command.exchange_order_id.empty() ||
            command.client_order_id.size() != 36 || command.exchange_order_id.size() > 200 ||
            (command.expected_mode != "PAPER" && command.expected_mode != "LIVE") ||
            doc["ts"].get_int64().get(command.ts_ms) || command.ts_ms <= 0) return std::nullopt;
    } else if (command.op != "cancel_open_orders") return std::nullopt;
    return command;
}
} // namespace vp
