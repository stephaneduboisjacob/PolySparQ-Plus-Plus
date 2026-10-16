#pragma once

#include "core/oracle_buffer.hpp"
#include "feeds/polymarket_rest.hpp"
#include "tui/tui.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace vp::engine {

struct WindowBotSignal {
    ShareSide side = ShareSide::UP;
    double confidence = 0.0;
    double edge = 0.0;
    double ask = 0.0;
    std::string reason;
};

struct LeadLagInput {
    std::string condition_id;
    std::string asset;
    int horizon_s = 0;
    int64_t window_close_ms = 0;
    int64_t now_ms = 0;
    int secs_left = 0;
    double cb_move_2s_bps = 0.0;
    double cb_move_5s_bps = 0.0;
    double cb_chainlink_lead_bps = 0.0;
    double oracle_move_2s_bps = 0.0;
    double binance_move_2s_bps = 0.0;
    double ensemble_up_probability = 0.5;
    double ensemble_confidence = 0.0;
    bool cb_fresh = false;
    bool oracle_fresh = false;
    bool binance_fresh = false;
    int up_book_age_ms = 0;
    int down_book_age_ms = 0;
    double up_ask = 0.0;
    double down_ask = 0.0;
    double up_spread_pct = 999.0;
    double down_spread_pct = 999.0;
    double conservative_up_probability = 0.5;
    double conservative_down_probability = 0.5;
    double fee_rate = 0.07;
    int fee_exponent = 1;
};

struct LeadLagConfig {
    bool enabled = false;
    int horizon_s = 0; // 0 enables both supported windows (300s and 900s)
    double min_signal_confidence = 0.55;
    double min_cb_move_bps = 0.5;
    double min_cb_move_5s_bps = 1.0;
    double min_cb_lead_bps = 0.5;
    double min_model_confidence = 0.20;
    double profit_target_pct = 15.0;
    double max_oracle_counter_bps = 1.5;
    double min_net_edge_pct = 1.5;
    double min_price = 0.03;
    double max_price = 0.95;
    double max_spread_pct = 8.0;
    int max_book_age_ms = 1000;
    int stop_entry_secs = 45;
    int stop_entry_5m_secs = 90;
    int stop_entry_15m_secs = 270;
    int max_entries_per_window = 24;
    double rearm_bps = 0.25;
};

struct LateSniperInput {
    std::string condition_id;
    std::string asset;
    int horizon_s = 0;
    int64_t window_close_ms = 0;
    int64_t now_ms = 0;
    int secs_left = 0;
    double up_bid = 0.0;
    double up_ask = 0.0;
    double down_bid = 0.0;
    double down_ask = 0.0;
    double tick_size = 0.01;
    double conditional_up_probability = 0.5;
    double conditional_up_low = 0.5;
    double conditional_up_high = 0.5;
    double ensemble_up_probability = 0.5;
    double ensemble_confidence = 0.0;
    bool forecast_ready = false;
    bool cb_fresh = false;
    bool oracle_fresh = false;
    double fee_rate = 0.07;
    int fee_exponent = 1;
};

struct LateSniperConfig {
    bool enabled = false;
    int min_secs_5m = 1;
    int max_secs_5m = 45;
    int min_secs_15m = 1;
    int max_secs_15m = 45;
    int max_entries_per_window = 5;
    int maker_improve_ticks = 0;
    double min_price = 0.82;
    double max_price = 0.97;
    double min_net_edge_pct = 3.0;
};

struct ConditionalTwapProbability {
    bool ready = false;
    double probability_up = 0.5;
    double low_up = 0.5;
    double high_up = 0.5;
};

// Estimate the final settlement TWAP probability conditioned on the TWAP
// segment already observed. Future spot is Coinbase-adjusted to the latest
// official Chainlink spot; the already observed segment is kept factual.
ConditionalTwapProbability conditional_twap_probability(
    const PmMarket& market,
    const vp::core::OracleBuffer& oracle,
    const vp::core::OracleBuffer& coinbase,
    int64_t now_ms,
    double strike,
    double sigma_per_sec);

class WindowBots {
public:
    std::optional<WindowBotSignal> lead_lag_entry(
        const LeadLagInput& in, const LeadLagConfig& cfg,
        int entries_this_window, std::string* reject_reason = nullptr);

    std::optional<WindowBotSignal> late_sniper_entry(
        const LateSniperInput& in, const LateSniperConfig& cfg,
        int entries_this_window, std::string* reject_reason = nullptr);

private:
    struct LeadState {
        int64_t window_close_ms = 0;
        int64_t last_signal_ms = 0;
        int entries_at_signal = 0;
        bool armed = true;
        bool left_rearm_zone = false;
        int last_direction = 0;
    };
    struct LateSniperState {
        int64_t window_close_ms = 0;
        int64_t last_signal_ms = 0;
    };
    std::unordered_map<std::string, LeadState> lead_state_;
    std::unordered_map<std::string, LateSniperState> late_sniper_state_;
};

} // namespace vp::engine
