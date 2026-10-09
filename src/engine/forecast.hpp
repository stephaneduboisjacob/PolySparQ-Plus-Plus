#pragma once
// Shared, allocation-free probability/execution kernel for bot and Cockpit.
// Experimental physical-price model; accuracy must be measured out of sample.
#include <algorithm>
#include <cmath>
#include <span>

namespace vp::engine {
inline constexpr const char* kForecastVersion = "reference-v3";
struct VolatilityEstimate { double sigma_minute = 0; int samples = 0; };
inline VolatilityEstimate estimate_volatility(std::span<const double> closes) {
    // EWMA variance reacts to recent moves; slower variance prevents a short
    // quiet patch from producing artificially certain forecasts.
    double fast = 0, slow = 0;
    int n = 0;
    const size_t start = closes.size() > 1441 ? closes.size() - 1441 : 1;
    for (size_t i = start; i < closes.size(); ++i) {
        if (!(closes[i] > 0 && closes[i-1] > 0) ||
            !std::isfinite(closes[i]) || !std::isfinite(closes[i-1])) return {};
        const double r = std::log(closes[i] / closes[i-1]);
        if (n++ == 0) fast = slow = r*r;
        else { fast = 0.94*fast + 0.06*r*r; slow = 0.995*slow + 0.005*r*r; }
    }
    return {n >= 30 ? std::max(0.00005, std::sqrt(std::max(fast, 0.5*slow))) : 0.0, n};
}
struct DriftEstimate { double log_drift_minute = 0; int samples = 0; };
inline DriftEstimate estimate_drift(std::span<const double> closes,
                                    double sigma_minute) {
    if (closes.size() < 31 || !(sigma_minute > 0) ||
        !std::isfinite(sigma_minute)) return {};
    // A deliberately conservative short-horizon trend estimate. Recent log
    // returns receive more weight, individual shocks are winsorised, and the
    // result is shrunk and capped relative to volatility. This is an outlook
    // for a not-yet-open window, not a substitute for its future official
    // opening reference.
    const size_t start = closes.size() > 61 ? closes.size() - 61 : 1;
    double ewma = 0;
    int n = 0;
    constexpr double alpha = 0.10;
    const double shock_cap = 3.0 * sigma_minute;
    for (size_t i = start; i < closes.size(); ++i) {
        if (!(closes[i] > 0 && closes[i-1] > 0) ||
            !std::isfinite(closes[i]) || !std::isfinite(closes[i-1])) return {};
        const double r = std::clamp(std::log(closes[i] / closes[i-1]),
                                    -shock_cap, shock_cap);
        ewma = n++ == 0 ? r : alpha*r + (1.0-alpha)*ewma;
    }
    const double shrink = 0.35 * n / (n + 60.0);
    return {std::clamp(ewma * shrink,
                       -0.10*sigma_minute, 0.10*sigma_minute), n};
}
struct ForecastInput {
    double spot = 0, strike = 0, seconds_left = 0, sigma_minute = 0;
    double reference_age_s = -1, candle_age_s = -1;
    int samples = 0, twap_window_s = 0;
    double log_drift_minute = 0;
};
struct Forecast {
    double probability = 0.5, low = 0.5, high = 0.5, quality = 0;
    bool ready = false;
    const char* status = "warming_up";
};
inline Forecast forecast(const ForecastInput& in) {
    Forecast out;
    if (!(std::isfinite(in.spot) && std::isfinite(in.strike) &&
          std::isfinite(in.seconds_left) && std::isfinite(in.sigma_minute) &&
          std::isfinite(in.reference_age_s) && std::isfinite(in.candle_age_s) &&
          std::isfinite(in.log_drift_minute))) {
        out.status = "invalid_input"; return out;
    }
    if (in.seconds_left <= 0) { out.status = "closed"; return out; }
    if (in.twap_window_s != 0 && in.twap_window_s != 30 && in.twap_window_s != 60) {
        out.status = "unsupported_reference"; return out;
    }
    if (in.spot <= 0 || in.strike <= 0) { out.status = "missing_reference"; return out; }
    if (in.reference_age_s < -1 || in.reference_age_s > 10 || in.reference_age_s == -1 ||
        in.candle_age_s < 0 || in.candle_age_s > 120) {
        out.status = "stale_data"; return out;
    }
    if (in.samples < 30 || in.sigma_minute <= 0) return out;
    if (in.twap_window_s > 0 && in.seconds_left <= in.twap_window_s) {
        out.status = "averaging_window"; return out;
    }
    // Arithmetic diffusion approximation for a future trailing average:
    // Var(TWAP_T / S_now) = sigma_second² * (T - 2W/3), T >= W.
    // Spot settlement is the W=0 special case. Quote prices never enter this
    // probability: they are independent execution-cost observations.
    const double variance_time = (in.seconds_left - 2.0*in.twap_window_s/3.0) / 60.0;
    const double mean_time = (in.seconds_left - 0.5*in.twap_window_s) / 60.0;
    const double move = std::log(in.spot / in.strike) +
                        in.log_drift_minute * mean_time;
    auto prob = [&](double scale) {
        const double sd = in.sigma_minute * scale * std::sqrt(variance_time);
        return std::clamp(0.5 * std::erfc(-move / (sd * std::sqrt(2.0))), 0.01, 0.99);
    };
    out.probability = prob(1.0);
    // A sensitivity envelope, NOT a calibrated statistical confidence interval.
    out.low = std::min(prob(0.75), prob(1.5));
    out.high = std::max(prob(0.75), prob(1.5));
    out.quality = std::clamp(in.samples / 240.0, 0.0, 1.0);
    out.ready = true; out.status = "ready";
    return out;
}
struct ExecutionEdge { double net = 0, conservative = 0, spread = 0; bool executable = false; };
inline ExecutionEdge execution_edge(double probability, double conservative_probability,
                                     double bid, double ask, double fee_rate,
                                     int exponent, double slippage = 0.005) {
    ExecutionEdge out;
    if (!std::isfinite(probability) || !std::isfinite(conservative_probability) ||
        !std::isfinite(bid) || !std::isfinite(ask) || !std::isfinite(fee_rate) ||
        !std::isfinite(slippage) || probability < 0 || probability > 1 ||
        conservative_probability < 0 || conservative_probability > 1 ||
        bid <= 0 || ask >= 1 || ask < bid || fee_rate < 0 || slippage < 0 ||
        exponent < 0 || exponent > 4) return out;
    const double cost = ask + fee_rate * std::pow(ask*(1-ask), exponent) + slippage;
    out.net = probability - cost;
    out.conservative = conservative_probability - cost;
    out.spread = ask-bid;
    out.executable = true;
    return out;
}
} // namespace vp::engine
