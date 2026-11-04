#include "engine/window_bots.hpp"

#include <cmath>
#include <stdexcept>
#include <string>

namespace {

using vp::ShareSide;
using vp::engine::LateSniperConfig;
using vp::engine::LateSniperInput;

LateSniperInput base_input() {
    LateSniperInput in;
    in.condition_id = "0xcondition";
    in.asset = "BTC";
    in.horizon_s = 300;
    in.window_close_ms = 1'000'000;
    in.now_ms = 950'000;
    in.secs_left = 60;
    in.tick_size = 0.01;
    in.up_bid = 0.87;
    in.up_ask = 0.88;
    in.down_bid = 0.11;
    in.down_ask = 0.12;
    in.conditional_up_low = 0.93;
    in.conditional_up_high = 0.95;
    in.conditional_up_probability = 0.94;
    in.ensemble_up_probability = 0.93;
    in.ensemble_confidence = 0.6;
    in.forecast_ready = true;
    in.cb_fresh = true;
    in.oracle_fresh = true;
    in.fee_rate = 0.0;
    in.fee_exponent = 1;
    return in;
}

LateSniperConfig base_config() {
    LateSniperConfig cfg;
    cfg.enabled = true;
    cfg.min_secs_5m = 1;
    cfg.max_secs_5m = 120;
    cfg.min_price = 0.82;
    cfg.max_price = 0.97;
    cfg.min_net_edge_pct = 3.0;
    cfg.max_entries_per_window = 1;
    return cfg;
}

std::string reject_of(vp::engine::WindowBots& bots, const LateSniperInput& in,
                      const LateSniperConfig& cfg) {
    std::string reason;
    const auto signal = bots.late_sniper_entry(in, cfg, 0, &reason);
    if (signal) return "accepted";
    return reason;
}

} // namespace

int main() {
    // A fresh conservative forecast that clears the passive limit by more than
    // the configured edge is the only accepted case.
    {
        vp::engine::WindowBots bots;
        const auto signal = bots.late_sniper_entry(
            base_input(), base_config(), 0, nullptr);
        if (!signal || signal->side != ShareSide::UP ||
            std::abs(signal->limit_price - 0.87) > 1e-9)
            throw std::runtime_error("qualified conservative edge was not accepted");
        if (signal->edge < 0.03 - 1e-9)
            throw std::runtime_error("accepted edge does not clear the minimum");
    }

    // The live book favorite alone is not evidence of value: with a neutral
    // forecast the favorite must be rejected even at a high price.
    {
        vp::engine::WindowBots bots;
        auto in = base_input();
        in.conditional_up_low = 0.50;
        in.conditional_up_high = 0.50;
        in.conditional_up_probability = 0.50;
        in.ensemble_up_probability = 0.50;
        if (reject_of(bots, in, base_config()) != "conservative_net_edge_too_small")
            throw std::runtime_error("market favorite was accepted without model edge");
    }

    // A model edge smaller than the configured threshold is still rejected.
    {
        vp::engine::WindowBots bots;
        auto in = base_input();
        in.conditional_up_low = 0.89;
        in.conditional_up_high = 0.91;
        in.conditional_up_probability = 0.90;
        in.ensemble_up_probability = 0.89;
        if (reject_of(bots, in, base_config()) != "conservative_net_edge_too_small")
            throw std::runtime_error("sub-threshold model edge was accepted");
    }

    // Stale references or an unavailable forecast fail closed.
    {
        vp::engine::WindowBots bots;
        auto in = base_input();
        in.oracle_fresh = false;
        if (reject_of(bots, in, base_config()) != "forecast_or_reference_stale")
            throw std::runtime_error("stale oracle reference did not fail closed");
    }
    {
        vp::engine::WindowBots bots;
        auto in = base_input();
        in.forecast_ready = false;
        if (reject_of(bots, in, base_config()) != "forecast_or_reference_stale")
            throw std::runtime_error("unavailable forecast did not fail closed");
    }
    {
        vp::engine::WindowBots bots;
        auto in = base_input();
        in.conditional_up_low = 0.95;
        in.conditional_up_high = 0.90; // low > high
        if (reject_of(bots, in, base_config()) != "invalid_forecast")
            throw std::runtime_error("inverted forecast interval was accepted");
    }

    // When the DOWN side carries the conservative edge, it is selected even
    // though UP is the book favorite.
    {
        vp::engine::WindowBots bots;
        auto in = base_input();
        in.up_bid = 0.87;
        in.up_ask = 0.88;
        in.down_bid = 0.12;
        in.down_ask = 0.13;
        in.conditional_up_low = 0.05;
        in.conditional_up_high = 0.10;
        in.conditional_up_probability = 0.08;
        in.ensemble_up_probability = 0.06;
        auto cfg = base_config();
        cfg.min_price = 0.10; // both quotes are in band for this selection test
        const auto signal = bots.late_sniper_entry(in, cfg, 0, nullptr);
        if (!signal || signal->side != ShareSide::DOWN ||
            std::abs(signal->limit_price - 0.12) > 1e-9)
            throw std::runtime_error("model-selected DOWN quote was not accepted");
    }
}
