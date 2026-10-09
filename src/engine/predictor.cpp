// Settlement-reference probability with technical diagnostics.
// Quote prices are execution costs, never a self-referential model anchor.
#include "predictor.hpp"
#include "indicators.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <fmt/format.h>

namespace vp::engine {

double norm_cdf(double x) {
    return 0.5 * (1.0 + std::erf(x / std::sqrt(2.0)));
}

// Retained for reference but NO LONGER used for the anchor.
// BS is only useful for long-dated options (>1 day), not 5–15 min windows.
double bs_binary_call_prob(double spot, double strike,
                            double secs_to_expiry, double vol_annual) {
    if (secs_to_expiry <= 0) return spot >= strike ? 1.0 : 0.0;
    if (spot <= 0 || strike <= 0) return 0.5;
    double T = secs_to_expiry / (365.0 * 24 * 3600);
    double sigma = std::clamp(vol_annual, 0.05, 2.5);
    double d2 = (std::log(spot / strike) - 0.5 * sigma * sigma * T)
              / (sigma * std::sqrt(T));
    return norm_cdf(d2);
}

// ── Digital fair value, zero-drift GBM (Phase 5.1) ──────────────────────
// σ is per-second realized vol measured from the oracle tick series
// itself (OracleBuffer::realized_vol_per_sec). T in seconds.
double digital_fv_up_prob(double spot, double strike,
                          double secs_left, double vol_per_sec) {
    if (secs_left <= 0) return spot >= strike ? 1.0 : 0.0;
    if (spot <= 0 || strike <= 0 || vol_per_sec <= 0) return 0.5;
    const double T = secs_left;
    const double s = vol_per_sec;
    const double d = (std::log(spot / strike) - 0.5 * s * s * T)
                   / (s * std::sqrt(T));
    return norm_cdf(d);
}

CheckpointDecision checkpoint_decision(double model_p_up,
                                       double oracle_delta_pct,
                                       double coinbase_delta_pct,
                                       double min_delta_pct,
                                       double max_size_multiplier) {
    CheckpointDecision out;
    const double floor = std::max(0.0, min_delta_pct);
    out.model_vote = (std::isfinite(model_p_up) && model_p_up < 0.5) ? -1 : 1;
    if (std::isfinite(oracle_delta_pct) && std::abs(oracle_delta_pct) >= floor &&
        std::abs(oracle_delta_pct) > 0.0)
        out.oracle_vote = oracle_delta_pct > 0.0 ? 1 : -1;
    if (std::isfinite(coinbase_delta_pct) && std::abs(coinbase_delta_pct) >= floor &&
        std::abs(coinbase_delta_pct) > 0.0)
        out.coinbase_vote = coinbase_delta_pct > 0.0 ? 1 : -1;

    const int votes[] = {out.model_vote, out.oracle_vote, out.coinbase_vote};
    for (int vote : votes) {
        if (vote > 0) ++out.up_votes;
        else if (vote < 0) ++out.down_votes;
    }
    out.active_votes = out.up_votes + out.down_votes;
    // A tie falls back to the model, so the checkpoint always buys a side.
    out.buy_up = out.up_votes == out.down_votes
        ? out.model_vote > 0
        : out.up_votes > out.down_votes;
    const int winning_votes = std::max(out.up_votes, out.down_votes);
    out.consensus = out.active_votes > 0
        ? winning_votes / static_cast<double>(out.active_votes) : 0.5;

    const double max_mult = std::max(1.0, max_size_multiplier);
    if (out.active_votes > 1 && out.up_votes != out.down_votes) {
        // Two agreeing sources earn half of the available size bonus; three
        // unanimous sources earn all of it. A split/tie never reduces base size.
        const double agreement = std::clamp(
            (winning_votes - 1) / static_cast<double>(out.active_votes - 1),
            0.0, 1.0);
        out.size_multiplier = 1.0 + (max_mult - 1.0) * agreement;
    }
    return out;
}

bool checkpoint_votes_unanimous(const CheckpointDecision& decision) {
    return decision.active_votes == 3 &&
           (decision.up_votes == 3 || decision.down_votes == 3);
}

bool checkpoint_pair_allowed(const std::vector<std::string>& pairs,
                             const std::string& asset,
                             int horizon_s) {
    if (pairs.empty()) return true;
    const std::string key = asset + ":" + std::to_string(horizon_s);
    return std::find(pairs.begin(), pairs.end(), key) != pairs.end();
}

double checkpoint_win_return_pct(double ask,
                                 double fee_rate,
                                 int fee_exponent) {
    if (!std::isfinite(ask) || ask <= 0.0 || ask >= 1.0 ||
        !std::isfinite(fee_rate) || fee_rate < 0.0)
        return -std::numeric_limits<double>::infinity();

    double p_pow = 1.0;
    double q_pow = 1.0;
    for (int i = 0; i < fee_exponent; ++i) {
        p_pow *= ask;
        q_pow *= (1.0 - ask);
    }
    const double all_in_per_share = ask + fee_rate * p_pow * q_pow;
    if (!(all_in_per_share > 0.0))
        return -std::numeric_limits<double>::infinity();
    return 100.0 * (1.0 - all_in_per_share) / all_in_per_share;
}

double checkpoint_net_edge(double fair_probability,
                           double ask,
                           double fee_rate,
                           int fee_exponent) {
    if (!std::isfinite(fair_probability) || fair_probability < 0.0 ||
        fair_probability > 1.0 || !std::isfinite(ask) || ask <= 0.0 ||
        ask >= 1.0 || !std::isfinite(fee_rate) || fee_rate < 0.0)
        return -std::numeric_limits<double>::infinity();

    double p_pow = 1.0;
    double q_pow = 1.0;
    for (int i = 0; i < fee_exponent; ++i) {
        p_pow *= ask;
        q_pow *= (1.0 - ask);
    }
    return fair_probability - (ask + fee_rate * p_pow * q_pow);
}

namespace {

// Bound a signed score to [-1,+1].
inline double clip(double x) { return std::clamp(x, -1.0, 1.0); }

double realized_vol_annualised_periods(const std::vector<double>& closes,
                                       int window,
                                       double periods_per_year) {
    if ((int)closes.size() < std::max(window, 2)) return 0.50;
    int n = std::min(window, (int)closes.size());
    int start = (int)closes.size() - n;
    std::vector<double> logret;
    logret.reserve(n - 1);
    for (int i = start + 1; i < (int)closes.size(); ++i) {
        if (closes[i] > 0 && closes[i - 1] > 0)
            logret.push_back(std::log(closes[i] / closes[i - 1]));
    }
    if (logret.size() < 2) return 0.50;
    double mean = 0.0;
    for (double r : logret) mean += r;
    mean /= (double)logret.size();
    double var = 0.0;
    for (double r : logret) {
        double d = r - mean;
        var += d * d;
    }
    var /= (double)(logret.size() - 1);
    return std::clamp(std::sqrt(var) * std::sqrt(periods_per_year), 0.05, 2.50);
}

} // namespace

PredictorOutput predict(const PredictorInput& in) {
    PredictorOutput o;
    PredictorParts& p = o.parts;

    // ── Multi-horizon realised volatility (annualised) ──────────
    p.rv_1h = ind::realized_vol_annualised(in.closes_1m, 60);
    p.rv_4h = ind::realized_vol_annualised(in.closes_1m, 240);
    p.rv_1d = ind::realized_vol_annualised(in.closes_1m, 1440);
    p.rv_7d = ind::realized_vol_annualised(in.closes_1m, 10080);
    p.rv_30d = realized_vol_annualised_periods(in.closes_60m, 24 * 30, 24.0 * 365.0);
    p.rv_90d = realized_vol_annualised_periods(in.closes_60m, 24 * 90, 24.0 * 365.0);
    p.rv_1y  = realized_vol_annualised_periods(in.closes_60m, 24 * 365, 24.0 * 365.0);

    // Time-weighted vol blend
    double w1h, w4h, w1d, w7d;
    if (in.secs_left <= 600) {
        w1h = 0.65; w4h = 0.25; w1d = 0.08; w7d = 0.02;
    } else if (in.secs_left <= 1800) {
        w1h = 0.45; w4h = 0.35; w1d = 0.15; w7d = 0.05;
    } else if (in.secs_left <= 3600) {
        w1h = 0.30; w4h = 0.35; w1d = 0.25; w7d = 0.10;
    } else {
        w1h = 0.20; w4h = 0.30; w1d = 0.30; w7d = 0.20;
    }
    double rv = w1h * p.rv_1h + w4h * p.rv_4h + w1d * p.rv_1d + w7d * p.rv_7d;
    rv = 0.88 * rv + 0.08 * p.rv_30d + 0.04 * p.rv_90d;
    rv = std::clamp(rv, 0.05, 2.50);
    p.rv_used = rv;

    const auto vol = estimate_volatility(in.closes_1m);
    o.forecast = forecast({in.reference_price, in.strike, double(in.secs_left),
        vol.sigma_minute, in.reference_age_s, in.candle_age_s,
        vol.samples, in.settlement_window_s});
    const double fv = o.forecast.probability;
    p.fair_value_up = o.fair_value_up = fv;

    // ── Oracle delta % ──────────────────────────────────────────
    if (in.strike > 0)
        p.oracle_delta_pct = 100.0 * (in.asset_price - in.strike) / in.strike;

    // ── Cross-source deviation (Binance vs Coinbase) ─────────────
    if (in.coinbase_price > 0 && in.asset_price > 0) {
        p.cross_src_dev_pct = 100.0 * (in.asset_price - in.coinbase_price) / in.asset_price;
        // CB-lead: Coinbase prints reach the settlement oracle ~2-4s before
        // the market fully reprices (the oracle-lag bot's proven edge, median
        // 3.9s lead). CB above Binance ⇒ upside pressure that the mid hasn't
        // priced yet. Full score at 0.05% divergence (typical arb band).
        p.cb_lead_pct = 100.0 * (in.coinbase_price - in.asset_price) / in.asset_price;
    }
    double cb_lead_score = clip(p.cb_lead_pct / 0.05);

    // ── Technical signals ────────────────────────────────────────
    double rsi_score = 0, macd_score = 0, bb_score = 0;
    double slope_score = 0, micro_score = 0;

    if (auto r = ind::rsi(in.closes_1m, 14)) {
        p.rsi14 = *r;
        if (*r < 30)      rsi_score = (30 - *r) / 30.0;
        else if (*r > 70) rsi_score = -(*r - 70) / 30.0;
        rsi_score = clip(rsi_score);
    }

    if (auto m = ind::macd(in.closes_1m, 12, 26, 9)) {
        p.macd_hist = m->hist;
        double norm = (in.asset_price > 0) ? m->hist / in.asset_price : 0.0;
        macd_score = clip(norm * 5000.0);
    }

    if (auto b = ind::bbands(in.closes_1m, 20, 2.0); b && in.asset_price > 0) {
        double band = b->up - b->mid;
        if (band > 0) {
            double z = (in.asset_price - b->mid) / band;
            p.bb_z = z;
            bb_score = -clip(z * 0.5);
        }
    }

    if (auto s = ind::linreg_slope(in.closes_1m, 5)) {
        p.slope_5 = *s;
        double norm = (in.asset_price > 0) ? *s / in.asset_price : 0.0;
        slope_score = clip(norm * 2000.0);
    }

    // ── Multi-timeframe slopes (% per minute) ───────────────────
    double mt_score = 0;
    int    mt_n     = 0;
    auto add_slope = [&](int win, double& field, double scale) {
        if (auto s = ind::linreg_slope(in.closes_1m, win); s && in.asset_price > 0) {
            field = 100.0 * (*s) / in.asset_price;
            mt_score += clip(field * scale);
            ++mt_n;
        }
    };
    add_slope(30,   p.slope_30m_pct, 50.0);
    if (in.closes_15m.size() >= 16) {
        if (auto s = ind::linreg_slope(in.closes_15m, 16); s && in.asset_price > 0) {
            p.slope_4h_pct = 100.0 * (*s) / in.asset_price;
            mt_score += clip((p.slope_4h_pct / 15.0) * 20.0);
            ++mt_n;
        }
    }
    if (in.closes_60m.size() >= 24) {
        if (auto s = ind::linreg_slope(in.closes_60m, 24); s && in.asset_price > 0) {
            p.slope_1d_pct = 100.0 * (*s) / in.asset_price;
            mt_score += clip((p.slope_1d_pct / 60.0) * 8.0);
            ++mt_n;
        }
    }
    double multi_trend_score = (mt_n > 0) ? clip(mt_score / mt_n) : 0.0;

    // ── Long-history regime from the imported candle set ─────────
    // Use hourly data for long windows so the 5-year import influences the
    // predictor without copying millions of 1-minute bars every sweep.
    double regime_trend = 0;
    int regime_n = 0;
    auto add_long_slope = [&](int hours, double& field, double full_move_scale_pct) {
        if (auto s = ind::linreg_slope(in.closes_60m, hours);
            s && in.asset_price > 0) {
            field = 100.0 * (*s) * hours / in.asset_price;
            regime_trend += clip(field / full_move_scale_pct);
            ++regime_n;
        }
    };
    add_long_slope(24 * 7,   p.slope_7d_pct,   8.0);
    add_long_slope(24 * 30,  p.slope_30d_pct, 20.0);
    add_long_slope(24 * 365, p.slope_1y_pct,  80.0);
    regime_trend = (regime_n > 0) ? clip(regime_trend / regime_n) : 0.0;

    double long_range_score = 0;
    int long_range_n = 0;
    auto add_close_range = [&](int hours, double& pos_field) {
        if ((int)in.closes_60m.size() < std::max(hours, 2) || in.asset_price <= 0)
            return;
        int start = (int)in.closes_60m.size() - hours;
        double hi = -1e18, lo = 1e18;
        for (int i = start; i < (int)in.closes_60m.size(); ++i) {
            hi = std::max(hi, in.closes_60m[i]);
            lo = std::min(lo, in.closes_60m[i]);
        }
        if (hi <= lo) return;
        pos_field = std::clamp((in.asset_price - lo) / (hi - lo), 0.0, 1.0);
        double score = 0.0;
        if (pos_field > 0.88) score = -(pos_field - 0.88) / 0.12;
        else if (pos_field < 0.12) score = (0.12 - pos_field) / 0.12;
        long_range_score += clip(score);
        ++long_range_n;
    };
    add_close_range(24 * 30,  p.range_pos_30d);
    add_close_range(24 * 365, p.range_pos_1y);
    long_range_score = (long_range_n > 0) ? clip(long_range_score / long_range_n) : 0.0;
    p.history_regime_score = clip(0.75 * regime_trend + 0.25 * long_range_score);

    // ── 24h hi-lo range position ────────────────────────────────
    double range_score = 0;
    if (in.bars_1m.size() >= 60) {
        int    n   = std::min<int>(1440, (int)in.bars_1m.size());
        size_t st  = in.bars_1m.size() - n;
        double hi  = -1e18, lo = 1e18;
        for (size_t i = st; i < in.bars_1m.size(); ++i) {
            if (in.bars_1m[i].high > hi) hi = in.bars_1m[i].high;
            if (in.bars_1m[i].low  < lo) lo = in.bars_1m[i].low;
        }
        if (hi > lo && in.asset_price > 0) {
            p.range_pos_24h = std::clamp((in.asset_price - lo) / (hi - lo), 0.0, 1.0);
            p.range_24h_pct = 100.0 * (hi - lo) / lo;
            if      (p.range_pos_24h > 0.85) range_score = -(p.range_pos_24h - 0.85) / 0.15;
            else if (p.range_pos_24h < 0.15) range_score =  (0.15 - p.range_pos_24h) / 0.15;
            range_score = clip(range_score);
        }
    }

    if (auto a = ind::atr(in.bars_1m, 14); a && in.asset_price > 0)
        p.atr_pct = 100.0 * (*a) / in.asset_price;

    if (auto v = ind::vwap(in.bars_1m, 30); v && *v > 0)
        p.vwap_dev_pct = 100.0 * (in.asset_price - *v) / *v;

    // ── Additional indicators (Phase 2.0) ────────────────────────
    if (auto s = ind::stoch(in.bars_1m, 14, 3)) {
        p.stoch_k = s->k;
        p.stoch_d = s->d;
    }
    if (auto a = ind::adx(in.bars_1m, 14)) {
        p.adx = a->adx;
        p.pdi = a->pdi;
        p.mdi = a->mdi;
    }
    if (auto c = ind::cci(in.bars_1m, 20)) {
        p.cci = *c;
    }

    // Microstructure: order book imbalance — UP book bid-heaviness supports
    // UP; DOWN book bid-heaviness (buyers accumulating DOWN) supports DOWN.
    // Combined so both books inform one score.
    {
        double sum = in.up_bid_sz + in.up_ask_sz;
        if (sum > 0)
            p.micro_imbalance = (in.up_bid_sz - in.up_ask_sz) / sum;
        double dsum = in.down_bid_sz + in.down_ask_sz;
        if (dsum > 0)
            p.micro_imbalance_down = (in.down_bid_sz - in.down_ask_sz) / dsum;
        micro_score = clip(0.6 * p.micro_imbalance - 0.4 * p.micro_imbalance_down);
    }

    // ── Oracle velocity / acceleration ──────────────────────────
    p.oracle_vel_1m_pct = in.oracle_vel_1m_pct;
    p.oracle_vel_5m_pct = in.oracle_vel_5m_pct;
    p.oracle_accel_pct  = in.oracle_accel_pct;
    double oracle_vel_score = clip(
        (in.oracle_vel_1m_pct * 0.7 + in.oracle_vel_5m_pct * 0.3) * 2.0
        + std::clamp(in.oracle_accel_pct, -0.3, 0.3));

    // ── Volume surge ────────────────────────────────────────────
    if (in.bars_1m.size() >= 15) {
        size_t ref = in.bars_1m.size() - 2;
        double cur_vol = in.bars_1m[ref].buy_volume + in.bars_1m[ref].sell_volume;
        double avg_vol = 0;
        for (size_t i = ref - 14; i < ref; ++i)
            avg_vol += in.bars_1m[i].buy_volume + in.bars_1m[i].sell_volume;
        avg_vol /= 14.0;
        if (avg_vol > 0) p.volume_surge_ratio = cur_vol / avg_vol;
    }

    // ── Candle body conviction ──────────────────────────────────
    if (in.bars_1m.size() >= 2) {
        const auto& last = in.bars_1m[in.bars_1m.size() - 2];
        double hl = last.high - last.low;
        if (hl > 0)
            p.candle_body_conviction = std::abs(last.close - last.open) / hl;
    }

    // Volume-surge directional score
    double volume_surge_score = 0;
    if (p.volume_surge_ratio > 1.5 && in.bars_1m.size() >= 2) {
        const auto& last = in.bars_1m[in.bars_1m.size() - 2];
        bool candle_up = last.close > last.open;
        bool fv_up    = fv > 0.5;
        double dir    = (candle_up == fv_up) ? 1.0 : -1.0;
        volume_surge_score = clip(dir * std::min(p.volume_surge_ratio / 5.0, 0.5));
    }

    // Candle body conviction score
    double candle_body_score = 0;
    if (p.candle_body_conviction > 0.6 && in.bars_1m.size() >= 2) {
        const auto& last = in.bars_1m[in.bars_1m.size() - 2];
        bool candle_up = last.close > last.open;
        bool fv_up    = fv > 0.5;
        double dir    = (candle_up == fv_up) ? 1.0 : -1.0;
        candle_body_score = clip(dir * std::min(p.candle_body_conviction * 0.5, 0.4));
    }

    // ── Trade flow bias ──────────────────────────────────────────
    p.trade_flow_bias = in.trade_flow_bias;
    double trade_flow_score = clip(in.trade_flow_bias * 2.0);

    // ── Tick momentum (sub-second velocity) ──────────────────────
    double tick_mom = 0.0;
    if (std::abs(in.tick_velocity_1s) > 1e-9) {
        tick_mom = std::clamp(in.tick_velocity_1s * 100.0, -1.0, 1.0);
        double v3 = std::clamp(in.tick_velocity_3s * 100.0, -1.0, 1.0);
        if (tick_mom * v3 > 0) tick_mom = 0.6 * tick_mom + 0.4 * v3;
        else                   tick_mom = 0.7 * tick_mom + 0.3 * v3;
    }
    p.tick_momentum_score = clip(tick_mom);
    double tick_momentum_score = p.tick_momentum_score;

    // ── PM token trade flow (tick-rule classified, optional) ─────
    double pm_flow_score = 0;
    if (in.pm_flow_enabled) {
        p.pm_flow_bias = in.pm_flow_bias;
        pm_flow_score = clip(in.pm_flow_bias);
    }

    // ── Cross-asset momentum (leading reference asset) ───────────
    // ETH follows BTC (and BTC drifts with ETH) on seconds-to-minutes
    // timescales, so the reference asset's 5m move bleeds into this
    // window. Full score at a 0.3% 5m move.
    p.other_mom_5m_pct = in.other_mom_5m_pct;
    double other_mom_score = clip(in.other_mom_5m_pct / 0.30);

    // ── Tech weighted sum ────────────────────────────────────────
    // Oracle velocity is the fastest forward indicator. RSI/MACD provide
    // mean-reversion context. cb_lead and other_mom join as a carve-out
    // (0.14/0.08) on top of the 1.0-weighted core — they are the only
    // signals here the market mid cannot already see — and the sum is
    // rescaled to stay normalized. pm_flow joins with 0.06 when enabled.
    p.tech_score =
          0.13 * rsi_score
        + 0.13 * macd_score
        + 0.10 * bb_score
        + 0.08 * slope_score
        + 0.12 * multi_trend_score
        + 0.04 * range_score
        + 0.06 * micro_score
        + 0.10 * oracle_vel_score
        + 0.06 * trade_flow_score
        + 0.04 * volume_surge_score
        + 0.04 * candle_body_score
        + 0.04 * tick_momentum_score
        + 0.06 * p.history_regime_score;
    p.tech_score = (p.tech_score + 0.14 * cb_lead_score
                              + 0.08 * other_mom_score) / 1.22;
    if (in.pm_flow_enabled) {
        p.tech_score = (p.tech_score + 0.06 * pm_flow_score) / 1.06;
    }
    p.tech_score = clip(p.tech_score);

    // ── Final prediction = market-implied fv + tech tilt ─────────
    // Compute this before agreement tracking. Previously agreement compared
    // signals against PredictorOutput's default 0.5, so confidence was measured
    // against the wrong direction whenever fair value was not exactly neutral.
    o.prediction = fv; // correlated technical indicators remain diagnostics until validated

    // ── Signal strength tracking (for gauges and calibration) ──────
    p.signal_strength_rsi    = std::abs(rsi_score);
    p.signal_strength_macd   = std::abs(macd_score);
    p.signal_strength_bb     = std::abs(bb_score);
    p.signal_strength_slope  = std::abs(slope_score);
    p.signal_strength_micro  = std::abs(micro_score);
    p.signal_strength_oracle = std::abs(oracle_vel_score);
    p.signal_strength_trade  = std::abs(trade_flow_score);
    p.signal_strength_tick   = std::abs(tick_momentum_score);
    p.signal_strength_pmflow = std::abs(pm_flow_score);
    p.signal_strength_cb     = std::abs(cb_lead_score);
    p.signal_strength_xmom   = std::abs(other_mom_score);
    p.signal_strength_vol    = std::abs(volume_surge_score);
    p.signal_strength_body   = std::abs(candle_body_score);
    p.signal_strength_regime = std::abs(p.history_regime_score);
    // Count only active signals agreeing with the predicted outcome direction.
    // Zero-valued/unavailable features are not evidence and must not dilute the
    // agreement ratio.
    auto agrees_with_pred = [&](double score) {
        return (o.prediction >= 0.5 && score > 0) ||
               (o.prediction < 0.5 && score < 0);
    };
    p.signals_total = 0;
    p.signals_agreeing = 0;
    #define TRACK_SIG(score) do { \
        if (std::abs(score) >= 0.02) { \
            ++p.signals_total; \
            if (agrees_with_pred(score)) ++p.signals_agreeing; \
        } \
    } while(0)
    TRACK_SIG(rsi_score);
    TRACK_SIG(macd_score);
    TRACK_SIG(bb_score);
    TRACK_SIG(slope_score);
    TRACK_SIG(multi_trend_score);
    TRACK_SIG(range_score);
    TRACK_SIG(micro_score);
    TRACK_SIG(oracle_vel_score);
    TRACK_SIG(trade_flow_score);
    TRACK_SIG(volume_surge_score);
    TRACK_SIG(candle_body_score);
    TRACK_SIG(tick_momentum_score);
    TRACK_SIG(p.history_regime_score);
    TRACK_SIG(cb_lead_score);
    TRACK_SIG(other_mom_score);
    if (in.pm_flow_enabled) TRACK_SIG(pm_flow_score);
    #undef TRACK_SIG

    // Both sides use the executable ask, configured fees and a half-cent
    // slippage allowance. Missing/crossed books cannot create an entry edge.
    const auto up = execution_edge(fv, o.forecast.low, in.up_bid, in.up_ask, in.fee_rate, in.fee_exponent);
    const auto dn = execution_edge(1-fv, 1-o.forecast.high, in.down_bid, in.down_ask, in.fee_rate, in.fee_exponent);
    o.edge_up = o.forecast.ready && up.executable ? up.conservative : -1.0;
    o.edge_down = o.forecast.ready && dn.executable ? dn.conservative : -1.0;
    o.confidence = o.forecast.ready ? o.forecast.quality *
        std::clamp(2.0 * std::abs(fv - 0.5), 0.0, 1.0) : 0.0;

    // ── Recommended size ─────────────────────────────────────────
    o.rec_size_usd = o.confidence;

    // Summary
    o.summary = fmt::format(
        "fv={:.2f} pred={:.2f} e_up={:+.3f} e_dn={:+.3f} conf={:.0f}% "
        "rsi={:.0f} macd={:+.4f} bbz={:+.2f} slp={:+.3f} "
        "micro={:+.2f}/{:+.2f} "
        "ovel={:+.2f} acl={:+.2f} tf={:+.2f} "
        "cb={:+.3f}% xm={:+.3f}% "
        "surg={:.1f}x body={:.2f} "
        "rv1h={:.2f} rv1d={:.2f} rv30d={:.2f} rv1y={:.2f} "
        "mt30m={:+.3f}%/m mt4h={:+.3f}%/m hist7d={:+.2f}% hist30d={:+.2f}% hist1y={:+.2f}% "
        "rng24={:.2f} (Δ24h={:.2f}%) rng30d={:.2f} rng1y={:.2f} hist_score={:+.2f} "
        "sig={}/{} agree={:.0f}%",
        fv, o.prediction, o.edge_up, o.edge_down, o.confidence * 100,
        p.rsi14, p.macd_hist, p.bb_z, p.slope_5,
        p.micro_imbalance, p.micro_imbalance_down,
        p.oracle_vel_1m_pct, p.oracle_accel_pct, p.trade_flow_bias,
        p.cb_lead_pct, p.other_mom_5m_pct,
        p.volume_surge_ratio, p.candle_body_conviction,
        p.rv_1h, p.rv_1d, p.rv_30d, p.rv_1y,
        p.slope_30m_pct, p.slope_4h_pct,
        p.slope_7d_pct, p.slope_30d_pct, p.slope_1y_pct,
        p.range_pos_24h, p.range_24h_pct,
        p.range_pos_30d, p.range_pos_1y, p.history_regime_score,
        p.signals_agreeing, p.signals_total,
        p.signals_total > 0 ? (100.0 * p.signals_agreeing / p.signals_total) : 0.0);

    return o;
}

// ── WPRED calibration ledger ────────────────────────────────────────────

void WpredCalibration::open(const std::string& bucket_key,
                            const std::string& pos_key, double p_side) {
    if (!std::isfinite(p_side)) return;
    open_[pos_key].push_back(
        {bucket_key, std::clamp(p_side, 0.0, 1.0)});
}

void WpredCalibration::settle(const std::string& pos_key, bool won) {
    auto it = open_.find(pos_key);
    if (it == open_.end() || it->second.empty()) return;
    OpenSample s = std::move(it->second.front());
    it->second.erase(it->second.begin());
    if (it->second.empty()) open_.erase(it);
    auto& b = buckets_[s.bucket_key];
    b.n += 1;
    b.wins += won ? 1u : 0u;
    b.p_sum += s.p_side;
}

WpredCalVerdict WpredCalibration::gate(const std::string& bucket_key,
                                       int min_samples,
                                       double margin) const {
    WpredCalVerdict v;
    auto it = buckets_.find(bucket_key);
    if (it == buckets_.end()) return v;  // no history: allow
    const auto& b = it->second;
    v.n = b.n;
    v.realized = static_cast<double>(b.wins) / static_cast<double>(b.n);
    v.predicted = b.p_sum / static_cast<double>(b.n);
    const bool enough = min_samples > 0 &&
        b.n >= static_cast<std::uint32_t>(min_samples);
    v.allow = !(enough && v.realized < v.predicted - std::max(0.0, margin));
    return v;
}

} // namespace vp::engine
