#include "advisor.hpp"
#include "net/http_client.hpp"

#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <simdjson.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace vp {

LlmAdvisor::LlmAdvisor(LlmConfig cfg, std::function<void(std::string)> on_reply)
    : cfg_(std::move(cfg)), on_reply_(std::move(on_reply)) {}

LlmAdvisor::~LlmAdvisor() {
    shutdown_.store(true);
    // Wait (bounded) for any in-flight detached thread to finish before
    // letting *this* be destroyed — otherwise the detached thread reads
    // freed cfg_/on_reply_ → heap corruption ("corrupted size vs prev_size").
    // 16 s caps to just over the 15 s HTTP timeout in query_async.
    for (int i = 0; i < 160 && busy_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

bool LlmAdvisor::is_busy() const { return busy_.load(); }

void LlmAdvisor::query_async(const std::string& context) {
    if (busy_.exchange(true)) {
        spdlog::debug("llm: query dropped — already in flight");
        return;
    }
    if (shutdown_.load()) { busy_.store(false); return; }

    // Capture by value so thread is self-contained
    std::thread([this, context]() {
        struct Guard { LlmAdvisor* a; ~Guard(){ a->busy_.store(false); } } g{this};

        // Build request body
        std::string sys =
            "You are a very short Polymarket prediction-market scalping advisor. "
            "You analyze a BTC or ETH UP/DOWN window market and give a 1-2 sentence "
            "recommendation: BUY UP, BUY DOWN, SELL, or HOLD, and why. "
            "Never issue orders yourself — your output is advisory only. "
            "Keep response under 120 characters.";

        std::string escaped_ctx;
        for (char c : context) {
            if (c == '"')  escaped_ctx += "\\\"";
            else if (c == '\\') escaped_ctx += "\\\\";
            else if (c == '\n') escaped_ctx += "\\n";
            else escaped_ctx += c;
        }

        std::string body = fmt::format(
            R"({{"model":"{}","max_tokens":200,"messages":[)"
            R"({{"role":"system","content":"{}"}},)"
            R"({{"role":"user","content":"{}"}}]}})",
            cfg_.model, sys, escaped_ctx);

        http::Headers hdrs;
        if (!cfg_.api_key.empty())
            hdrs["Authorization"] = "Bearer " + cfg_.api_key;

        auto resp = http::post(cfg_.url, body, hdrs, 15000);

        if (!resp.error.empty()) {
            spdlog::warn("llm: HTTP error: {}", resp.error);
            if (on_reply_) on_reply_("[LLM unavailable]");
            return;
        }
        if (resp.status != 200) {
            spdlog::warn("llm: status={} body={}", resp.status, resp.body.substr(0,200));
            if (on_reply_) on_reply_("[LLM error " + std::to_string(resp.status) + "]");
            return;
        }

        // Parse choices[0].message.content
        std::string reply;
        try {
            simdjson::ondemand::parser parser;
            auto pad = resp.body + "    ";
            auto doc = parser.iterate(pad);
            auto content = doc["choices"].at(0)["message"]["content"].get_string();
            reply = std::string(content.value());
        } catch (...) {
            reply = "[LLM parse error]";
        }

        // Truncate to 200 chars to keep TUI tidy
        if (reply.size() > 200) reply = reply.substr(0, 197) + "...";

        spdlog::info("llm: {}", reply);
        if (on_reply_) on_reply_(std::move(reply));
    }).detach();
}

} // namespace vp
