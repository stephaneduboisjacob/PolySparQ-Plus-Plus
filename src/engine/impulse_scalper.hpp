#pragma once

#include "core/types.hpp"
#include <array>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vp::engine {

// Event-time Binance top-of-book impulse features. This detector is research
// only: callers may record its candidates, but it never creates an order.
struct ImpulseFeatures {
    bool ready = false;
    bool candidate = false;
    int direction = 0; // +1 = buy UP, -1 = buy DOWN
    std::string gate_reason = "quote_warmup";
    double ofi_250ms = 0.0;
    double ofi_500ms = 0.0;
    double ofi_threshold = 0.0;
    double move_250ms_bps = 0.0;
    double move_500ms_bps = 0.0;
    double move_z_250ms = 0.0;
    double move_z_500ms = 0.0;
    double realized_vol_1m_bps = 0.0;
    int quote_samples_60s = 0;
};

class ImpulseScalperDetector {
public:
    void on_book_ticker(const std::string& asset, double bid, double bid_size,
                        double ask, double ask_size, int64_t recv_mono_ns);
    ImpulseFeatures evaluate(const std::string& asset, int64_t now_mono_ns);

private:
    struct QuoteSample {
        int64_t ts_ns = 0;
        double mid = 0.0;
        double bid = 0.0;
        double bid_size = 0.0;
        double ask = 0.0;
        double ask_size = 0.0;
        double normalized_ofi_delta = 0.0;
    };
    struct AssetHistory {
        std::deque<QuoteSample> quotes;
        std::deque<std::pair<int64_t, double>> ofi_scores;
        int64_t last_eval_ns = 0;
    };

    std::mutex mu_;
    std::unordered_map<std::string, AssetHistory> history_;
};

struct ImpulseShadowCandidate {
    std::string id;
    std::string condition_id;
    std::string instance;
    std::string session_id;
    ShareSide side = ShareSide::UP;
    int64_t created_ms = 0;
    double maker_quote = 0.0;
    double target_bid = 0.0;
    double stop_bid = 0.0; // zero means no configured stop barrier
    double last_bid = 0.0;
    double last_ask = 0.0;
    int64_t last_quote_ms = 0;
    std::array<bool, 4> markout_written{};
    bool target_touched = false;
    bool stop_touched = false;
    std::string first_barrier;
    int64_t first_barrier_ms = 0;
};

struct ImpulseShadowEvent {
    std::string condition_id;
    std::string event_type;
    std::string payload_json;
};

// Tracks executable Polymarket bid markouts for hypothetical post-only quotes.
// It records quote-path touches, never assumes the hypothetical maker order
// filled, and is bounded to a short 5-second observation horizon.
class ImpulseShadowTracker {
public:
    bool admit(const std::string& condition_id, ShareSide side, int direction,
               int64_t now_ms);
    bool sample_due(const std::string& condition_id, int64_t now_ms);
    void add(ImpulseShadowCandidate candidate);
    std::vector<ImpulseShadowEvent> on_book_update(
        const std::string& condition_id, ShareSide side,
        double best_bid, double best_ask, int64_t now_ms);
    std::vector<ImpulseShadowEvent> advance(int64_t now_ms);

private:
    static std::string make_markout(const ImpulseShadowCandidate& c,
                                    int milestone_ms, int64_t now_ms,
                                    bool terminal, const char* terminal_reason);
    static std::string make_outcome(const ImpulseShadowCandidate& c,
                                    int64_t now_ms, const char* reason);

    std::mutex mu_;
    std::unordered_map<std::string, int64_t> last_candidate_ms_;
    std::unordered_map<std::string, int64_t> last_sample_ms_;
    std::unordered_map<std::string, std::vector<ImpulseShadowCandidate>> pending_;
};

} // namespace vp::engine
