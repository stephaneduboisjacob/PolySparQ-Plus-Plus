#include "window_bots.hpp"
#include "engine/predictor.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <fmt/format.h>

namespace vp::engine {

namespace {
bool supported_window_bot_asset(const std::string& asset) {
    return asset == "BTC" || asset == "ETH" || asset == "SOL" || asset == "XRP";
}
bool supported_lead_horizon(int horizon_s) {
    return horizon_s == 300 || horizon_s == 900;
}
bool lead_horizon_enabled(int configured_horizon_s, int market_horizon_s) {
    return supported_lead_horizon(market_horizon_s) &&
        (configured_horizon_s == 0 || configured_horizon_s == market_horizon_s);
}
}

ConditionalTwapProbability conditional_twap_probability(
    const PmMarket& market,
    const vp::core::OracleBuffer& oracle,
    const vp::core::OracleBuffer& coinbase,
    int64_t now_ms,
    double strike,
    double sigma_per_sec) {
    ConditionalTwapProbability out;
    const double secs_left = (market.window_close_ms - now_ms) / 1000.0;
    if (secs_left < 0.0 || !(strike > 0.0) || !(sigma_per_sec > 0.0) ||
        !std::isfinite(sigma_per_sec)) return out;

    const std::string spot_key = market.reference_price_key();
    const double spot_ts = oracle.latest_timestamp(spot_key);
    const double spot = oracle.latest(spot_key);
    const double cb_now = coinbase.latest(market.asset);
    const double cb_ts = coinbase.latest_timestamp(market.asset);
    const double cb_at_spot = spot_ts > 0.0
        ? coinbase.lookup_at_or_before(market.asset, spot_ts) : 0.0;
    const double now_s = now_ms / 1000.0;
    const double official_age = now_s -
        oracle.latest_timestamp(market.reference_key());
    const double spot_age = now_s - spot_ts;
    const double cb_age = now_ms / 1000.0 - cb_ts;
    if (!(spot > 0.0 && cb_now > 0.0 && cb_at_spot > 0.0) ||
        spot_ts <= 0.0 || official_age < -1.0 || official_age > 10.0 ||
        spot_age < -1.0 || spot_age > 10.0 ||
        cb_age < -1.0 || cb_age > 3.0) return out;

    // Remove static exchange basis using Coinbase at the Chainlink spot's
    // timestamp, then carry only Coinbase's newer move into the future leg.
    const double forecast_spot = spot * cb_now / cb_at_spot;
    if (!(forecast_spot > 0.0) || !std::isfinite(forecast_spot)) return out;

    auto probability_for_scale = [&](double scale) -> double {
        if (market.settlement_window_s <= 0) {
            const double sigma = sigma_per_sec * scale;
            const double sd = forecast_spot * sigma * std::sqrt(std::max(1.0, secs_left));
            return sd > 0.0
                ? norm_cdf((forecast_spot - strike) / sd)
                : (forecast_spot >= strike ? 1.0 : 0.0);
        }

        const double window = static_cast<double>(market.settlement_window_s);
        const double start = market.window_close_ms / 1000.0 - window;
        const bool interval_started = now_s >= start;
        const double observed_end = interval_started
            ? std::clamp(spot_ts, start, now_s) : start;
        const double known_seconds = std::clamp(observed_end - start, 0.0, window);
        const double lag_seconds = std::max(0.0, now_s - observed_end);
        double known_average = 0.0;
        if (known_seconds > 0.0) {
            known_average = oracle.time_weighted_average(
                spot_key, start, observed_end);
            if (!(known_average > 0.0)) return -1.0;
        }

        double lag_average = forecast_spot;
        if (lag_seconds > 0.0) {
            const double cb_average = coinbase.time_weighted_average(
                market.asset, observed_end, now_s);
            if (cb_average > 0.0)
                lag_average = spot * cb_average / cb_at_spot;
        }

        const double future_seconds = interval_started
            ? std::max(0.0, secs_left) : window;
        double mean_final = (known_average * known_seconds +
                             lag_average * lag_seconds +
                             forecast_spot * future_seconds) / window;
        double variance_time = 0.0;
        // Chainlink observations through spot_ts are factual. The recent gap
        // after that timestamp is estimated from Coinbase with basis aligned
        // at spot_ts. Only that proxy segment and the future segment contribute
        // uncertainty; use a conservative fractional diffusion variance for
        // the Coinbase-filled lag and the exact Brownian integral variance for
        // the time still remaining before settlement.
        variance_time = interval_started
            ? 0.25 * lag_seconds * lag_seconds * lag_seconds /
                  (3.0 * window * window) +
                  future_seconds * future_seconds * future_seconds /
                  (3.0 * window * window)
            : std::max(0.0, secs_left - 2.0 * window / 3.0);
        const double sd = forecast_spot * sigma_per_sec * scale *
                          std::sqrt(variance_time);
        if (!(sd > 0.0)) return mean_final >= strike ? 1.0 : 0.0;
        return norm_cdf((mean_final - strike) / sd);
    };

    const double point = probability_for_scale(1.0);
    const double low_vol = probability_for_scale(1.5);
    const double high_vol = probability_for_scale(0.75);
    if (point < 0.0 || low_vol < 0.0 || high_vol < 0.0 ||
        !std::isfinite(point) || !std::isfinite(low_vol) ||
        !std::isfinite(high_vol)) return out;
    out.probability_up = std::clamp(point, 0.01, 0.99);
    out.low_up = std::clamp(std::min(low_vol, high_vol), 0.01, 0.99);
    out.high_up = std::clamp(std::max(low_vol, high_vol), 0.01, 0.99);
    out.ready = true;
    return out;
}

std::optional<WindowBotSignal> WindowBots::lead_lag_entry(
    const LeadLagInput& in, const LeadLagConfig& cfg, int entries_this_window,
    std::string* reject_reason) {
    if (reject_reason) reject_reason->clear();
    auto reject = [&](const char* why) -> std::optional<WindowBotSignal> {
        if (reject_reason) *reject_reason = why;
        return std::nullopt;
    };
    const int stop_entry_secs = in.horizon_s == 300
        ? cfg.stop_entry_5m_secs : cfg.stop_entry_15m_secs;
    const int max_secs_remaining = in.horizon_s == 300
        ? cfg.max_secs_remaining_5m : cfg.max_secs_remaining_15m;
    const bool horizon_allowed = lead_horizon_enabled(cfg.horizon_s, in.horizon_s);
    if (!cfg.enabled || !supported_window_bot_asset(in.asset) ||
        !horizon_allowed ||
        (max_secs_remaining > 0 && in.secs_left > max_secs_remaining) ||
        in.secs_left <= stop_entry_secs ||
        entries_this_window >= cfg.max_entries_per_window ||
        !in.cb_fresh || !in.oracle_fresh ||
        !std::isfinite(in.cb_move_2s_bps) ||
        !std::isfinite(in.cb_move_5s_bps) ||
        !std::isfinite(in.cb_chainlink_lead_bps) ||
        !std::isfinite(in.ensemble_up_probability) ||
        !std::isfinite(in.ensemble_confidence)) {
        if (reject_reason) {
            if (!cfg.enabled) *reject_reason = "disabled";
            else if (!supported_window_bot_asset(in.asset)) *reject_reason = "asset_mismatch";
            else if (!horizon_allowed) *reject_reason = "horizon_mismatch";
            else if (max_secs_remaining > 0 && in.secs_left > max_secs_remaining)
                *reject_reason = "opening_delay";
            else if (in.secs_left <= stop_entry_secs) *reject_reason = "entry_cutoff";
            else if (entries_this_window >= cfg.max_entries_per_window) *reject_reason = "entry_limit";
            else if (!in.cb_fresh) *reject_reason = "coinbase_stale_or_unaligned";
            else if (!in.oracle_fresh) *reject_reason = "chainlink_stale";
            else *reject_reason = "non_finite_input";
        }
        return std::nullopt;
    }

    auto& state = lead_state_[in.condition_id];
    if (state.window_close_ms != in.window_close_ms) {
        state = LeadState{};
        state.window_close_ms = in.window_close_ms;
    }
    const double lead = in.cb_chainlink_lead_bps;
    const int direction = lead > 0.0 ? 1 : (lead < 0.0 ? -1 : 0);
    const int model_direction = in.ensemble_up_probability > 0.5 ? 1
        : in.ensemble_up_probability < 0.5 ? -1 : 0;
    if (!state.armed) {
        if (std::abs(lead) > cfg.rearm_bps) state.left_rearm_zone = true;
        const bool returned_to_rearm_zone = state.left_rearm_zone &&
            std::abs(lead) <= cfg.rearm_bps;
        const bool confirmed_reversal = direction != 0 &&
            state.last_direction != 0 && direction != state.last_direction &&
            std::abs(lead) >= cfg.min_cb_lead_bps;
        // The caller only evaluates entries when there is no open position or
        // pending entry in this market. A completed accepted order cycle can
        // therefore re-arm independently of the Coinbase/Chainlink basis,
        // which often stays near zero while both feeds track each other.
        const int64_t since_signal_ms = in.now_ms - state.last_signal_ms;
        const bool completed_cycle = entries_this_window > state.entries_at_signal &&
            since_signal_ms >= 2000;
        // If a signal was rejected before an order was accepted, allow a fresh
        // attempt after a short cooldown instead of permanently disarming this
        // market for the rest of its window.
        const bool retry_after_rejection = entries_this_window <= state.entries_at_signal &&
            since_signal_ms >= 10000;
        const bool confirmed_model_reversal = model_direction != 0 &&
            state.last_direction != 0 && model_direction != state.last_direction &&
            in.ensemble_confidence >= cfg.min_model_confidence;
        if (returned_to_rearm_zone || confirmed_reversal || completed_cycle ||
            retry_after_rejection || confirmed_model_reversal) {
            state.armed = true;
            state.left_rearm_zone = false;
        }
    }
    if (!state.armed) return reject("awaiting_rearm");
    if (in.ensemble_confidence < cfg.min_model_confidence) return reject("model_confidence");

    const bool coinbase_impulse = direction != 0 &&
        direction * in.cb_move_2s_bps >= cfg.min_cb_move_bps &&
        direction * in.cb_move_5s_bps >= -cfg.min_cb_move_5s_bps &&
        direction * lead >= cfg.min_cb_lead_bps;
    if (!cfg.model_value_enabled && !coinbase_impulse)
        return reject("model_value_disabled");
    // The fast model can also trigger when it sees conservative value at the
    // current ask. Coinbase lead is an additional early trigger, not a
    // requirement that suppresses every market while spot and Coinbase track.
    const int trade_direction = coinbase_impulse ? direction : model_direction;
    if (trade_direction == 0) return reject("no_model_direction_or_coinbase_impulse");
    if (coinbase_impulse && model_direction != 0 &&
        model_direction != direction)
        return reject("model_direction_disagrees");
    if (!coinbase_impulse && direction != 0 &&
        direction * in.cb_move_2s_bps >= cfg.min_cb_move_bps &&
        direction != model_direction)
        return reject("model_direction_disagrees_with_coinbase_impulse");
    if (trade_direction * in.oracle_move_2s_bps < -cfg.max_oracle_counter_bps)
        return reject("chainlink_moved_against_signal");
    if (in.binance_fresh &&
        std::abs(in.binance_move_2s_bps) >= cfg.min_cb_move_bps &&
        trade_direction * in.binance_move_2s_bps < 0.0) return reject("binance_direction_disagrees");

    const ShareSide side = trade_direction > 0 ? ShareSide::UP : ShareSide::DOWN;
    const double ask = side == ShareSide::UP ? in.up_ask : in.down_ask;
    const double spread = side == ShareSide::UP ? in.up_spread_pct : in.down_spread_pct;
    const int book_age_ms = side == ShareSide::UP
        ? in.up_book_age_ms : in.down_book_age_ms;
    const double p_side = side == ShareSide::UP
        ? in.conservative_up_probability
        : in.conservative_down_probability;
    if (!(ask >= cfg.min_price && ask <= cfg.max_price)) return reject("share_price_outside_band");
    if (book_age_ms < 0 || book_age_ms > cfg.max_book_age_ms) return reject("book_stale");
    if (!(spread >= 0.0 && spread <= cfg.max_spread_pct)) return reject("spread_too_wide");
    if (!(p_side >= 0.0 && p_side <= 1.0)) return reject("invalid_conservative_probability");

    // Entry value is measured against the current ask. The configured take-profit
    // is an execution target, not a binary-settlement fair value; comparing a
    // settlement probability to that exit quote makes almost every valid scalp
    // signal impossible to admit.
    const double target_price = ask * (1.0 + cfg.profit_target_pct / 100.0);
    if (!(target_price > ask && target_price < 1.0)) return reject("take_profit_price_unattainable");
    const double edge = checkpoint_net_edge(
        p_side, ask, 0.0, in.fee_exponent);
    if (!std::isfinite(edge) || edge < cfg.min_net_edge_pct / 100.0)
        return reject("entry_net_edge_too_small");

    const double confidence = std::clamp(
        0.55 + std::min(0.20, edge * 1.5) +
        std::min(0.15, std::abs(lead) / 40.0), 0.55, 0.92);
    if (confidence < cfg.min_signal_confidence)
        return reject("signal_confidence");

    state.armed = false;
    state.left_rearm_zone = std::abs(lead) > cfg.rearm_bps;
    state.last_direction = trade_direction;
    state.last_signal_ms = in.now_ms;
    state.entries_at_signal = entries_this_window;
    return WindowBotSignal{
        side, confidence, edge, ask,
        fmt::format("{} {} move2={:+.2f}bp move5={:+.2f}bp lead={:+.2f}bp oracle2={:+.2f}bp ens={:.3f}/{:.2f} p={:.3f} ask={:.3f} tp={:.3f} entry_edge={:+.2f}pt",
                    coinbase_impulse ? "CB_LEAD" : "MODEL_VALUE",
                    trade_direction > 0 ? "UP" : "DOWN", in.cb_move_2s_bps,
                    in.cb_move_5s_bps, lead, in.oracle_move_2s_bps,
                    in.ensemble_up_probability, in.ensemble_confidence,
                    p_side, ask, target_price, edge * 100.0)};
}

std::optional<WindowBotSignal> WindowBots::late_sniper_entry(
    const LateSniperInput& in, const LateSniperConfig& cfg,
    int entries_this_window, std::string* reject_reason) {
    if (reject_reason) reject_reason->clear();
    if (!cfg.enabled) {
        if (reject_reason) *reject_reason = "disabled";
        return std::nullopt;
    }
    if (!supported_window_bot_asset(in.asset)) {
        if (reject_reason) *reject_reason = "asset_mismatch";
        return std::nullopt;
    }
    if (in.horizon_s != 300 && in.horizon_s != 900) {
        if (reject_reason) *reject_reason = "horizon_mismatch";
        return std::nullopt;
    }
    const int min_secs = in.horizon_s == 300
        ? cfg.min_secs_5m : cfg.min_secs_15m;
    const int max_secs = in.horizon_s == 300
        ? cfg.max_secs_5m : cfg.max_secs_15m;
    if (in.secs_left < min_secs || in.secs_left > max_secs) {
        if (reject_reason) *reject_reason = "outside_entry_window";
        return std::nullopt;
    }
    if (!in.forecast_ready || !in.cb_fresh || !in.oracle_fresh) {
        if (reject_reason) *reject_reason = "forecast_or_reference_stale";
        return std::nullopt;
    }
    if (!std::isfinite(in.ensemble_up_probability) ||
        in.ensemble_up_probability < 0.0 || in.ensemble_up_probability > 1.0 ||
        !std::isfinite(in.conditional_up_low) ||
        !std::isfinite(in.conditional_up_high) ||
        in.conditional_up_low < 0.0 || in.conditional_up_low > 1.0 ||
        in.conditional_up_high < 0.0 || in.conditional_up_high > 1.0 ||
        in.conditional_up_low > in.conditional_up_high) {
        if (reject_reason) *reject_reason = "invalid_forecast";
        return std::nullopt;
    }
    auto& state = late_sniper_state_[in.condition_id];
    if (state.window_close_ms != in.window_close_ms) {
        state = LateSniperState{};
        state.window_close_ms = in.window_close_ms;
    }
    if (state.last_signal_ms > 0 && in.now_ms - state.last_signal_ms < 5000) {
        if (reject_reason) *reject_reason = "entry_cooldown";
        return std::nullopt;
    }
    if (entries_this_window >= cfg.max_entries_per_window) {
        if (reject_reason) *reject_reason = "entry_limit";
        return std::nullopt;
    }

    const double tick = std::isfinite(in.tick_size) && in.tick_size > 0.0
        ? in.tick_size : 0.01;
    const int improve_ticks = std::max(0, cfg.maker_improve_ticks);
    const auto passive_limit = [&](double bid, double ask) {
        if (!std::isfinite(ask) || ask <= 0.0 || ask >= 1.0)
            return 0.0;
        double price = bid > 0.0
            ? bid + improve_ticks * tick
            : ask - tick;
        // Match the BUY maker pricing rule used by both executors: improve the
        // bid where possible, but stay at least one tick below the ask.
        price = std::min(price, ask - tick);
        price = std::min(price, cfg.max_price);
        price = std::floor(price / tick + 1e-9) * tick;
        if (!std::isfinite(price) || price + 1e-9 < cfg.min_price ||
            price > cfg.max_price + 1e-9 || price >= ask - 1e-9)
            return 0.0;
        return price;
    };
    const double up_limit = passive_limit(in.up_bid, in.up_ask);
    const double down_limit = passive_limit(in.down_bid, in.down_ask);
    const bool up_in_band = up_limit > 0.0;
    const bool down_in_band = down_limit > 0.0;
    if (!up_in_band && !down_in_band) {
        if (reject_reason) *reject_reason = "share_price_outside_band";
        return std::nullopt;
    }

    // Score both passive quotes against the weaker of the independent
    // settlement forecast and the fast ensemble. A late quote is eligible
    // only when this conservative probability clears its price by the
    // configured edge; a high share price alone is not evidence of value.
    const double conservative_up = std::min(
        in.conditional_up_low, in.ensemble_up_probability);
    const double conservative_down = std::min(
        1.0 - in.conditional_up_high, 1.0 - in.ensemble_up_probability);
    const double min_edge = std::max(0.0, cfg.min_net_edge_pct) / 100.0;
    const double up_edge = up_in_band
        ? checkpoint_net_edge(conservative_up, up_limit, 0.0, in.fee_exponent)
        : -std::numeric_limits<double>::infinity();
    const double down_edge = down_in_band
        ? checkpoint_net_edge(conservative_down, down_limit, 0.0, in.fee_exponent)
        : -std::numeric_limits<double>::infinity();
    const bool up_eligible = up_in_band && up_edge >= min_edge;
    const bool down_eligible = down_in_band && down_edge >= min_edge;
    if (!up_eligible && !down_eligible) {
        if (reject_reason) *reject_reason = "conservative_net_edge_too_small";
        return std::nullopt;
    }

    const bool choose_up = up_eligible &&
        (!down_eligible || up_edge >= down_edge);
    const double limit = choose_up ? up_limit : down_limit;
    const double edge = choose_up ? up_edge : down_edge;
    const ShareSide side = choose_up ? ShareSide::UP : ShareSide::DOWN;
    // Rate-limit re-evaluation after a signal. Accepted entries consume the
    // configured window count in the shared submit path; the short cooldown
    // permits another qualified clip after a fill or a rejected submission.
    state.last_signal_ms = in.now_ms;
    return WindowBotSignal{
        side, 0.55 + edge, edge, limit,
        fmt::format("LATE_SETTLEMENT_MAKER_ONLY {} t={}s limit={:.3f} "
                    "conservative_edge={:.2f}pt band=[{:.2f},{:.2f}]",
                    choose_up ? "UP" : "DOWN", in.secs_left, limit,
                    edge * 100.0,
                    cfg.min_price, cfg.max_price)};
}

} // namespace vp::engine
