#pragma once
// Multi-signal prediction engine.
//
// Given a snapshot (asset price history, oracle delta, target/strike,
// time-to-close, current UP/DOWN PM mid prices, microstructure), produces:
//
//   • fair_value_up  ∈ [0,1]   — model probability that asset_close ≥ strike
//   • prediction     ∈ [0,1]   — same, ensemble-blended with technical signals
//   • edge_up        signed    — prediction − up_mid  (positive ⇒ buy UP)
//   • edge_down      signed    — (1 − prediction) − down_mid
//   • confidence     ∈ [0,1]   — combined trade conviction
//   • tech_score     signed    — −1..+1 (sum of 5 technical signals, weighted)
//   • parts.*                  — individual contributions for diagnostics/UI
//
// Pure / stateless: receives history snapshots from CandleStore. Safe to
// call from the refresh thread.
#include "indicators.hpp"
#include "forecast.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace vp::engine {

struct PredictorInput {
    std::string asset;            // "BTC" / "ETH"
    int         horizon_s = 0;    // 300/900/3600
    int64_t     secs_left = 0;    // seconds until window close
    double      asset_price = 0;  // latest spot (Binance)
    double      strike      = 0;  // window-open price (oracle/Binance)
    double      reference_price = 0; // Chainlink spot, matched to TWAP/spot strike
    double      reference_age_s = -1;
    double      candle_age_s = -1;
    int         settlement_window_s = -1;
    double      fee_rate = 0.07;
    int         fee_exponent = 1;
    double      coinbase_price = 0; // optional cross-source confirmation
    // Polymarket prices (mid)
    double      up_mid = 0, down_mid = 0;
    // Order-book microstructure (top-of-book)
    double      up_bid = 0, up_ask = 0, up_bid_sz = 0, up_ask_sz = 0;
    // Down-side book microstructure (symmetric with up-side)
    double      down_bid = 0, down_ask = 0, down_bid_sz = 0, down_ask_sz = 0;
    // Oracle velocity (pre-computed by caller from OracleBuffer)
    double      oracle_vel_1m_pct = 0;   // %/min over last 60s
    double      oracle_vel_5m_pct = 0;   // %/min over last 300s
    double      oracle_accel_pct  = 0;   // change in velocity/min
    // Trade flow bias (from TradeFlowBuffer, computed in main.cpp)
    double      trade_flow_bias = 0;     // -1..+1, +1 = all buy aggression
    // Recent close history (1-min bars), most recent at back. Pre-fetched
    // by caller from CandleStore. Intraday indicators slice this internally;
    // callers should pass enough for 7d realized vol without copying the full
    // multi-year 1m history in the hot path.
    std::vector<double> closes_1m;
    // 5-minute closes (aggregated from 1-minute bars).
    std::vector<double> closes_5m;
    // 15-minute closes.
    std::vector<double> closes_15m;
    // 60-minute closes. Callers should pass the long window (up to 5 years)
    // so the predictor can compute 30d/90d/1y volatility and regime trend
    // cheaply from hourly data.
    std::vector<double> closes_60m;
    // Recent OHLCV bars (1-min), most recent at back. Used for ATR, VWAP,
    // 24h range. Caller may pass a shorter window (e.g. last 7 days) since
    // these features don't need a year of OHLC.
    std::vector<Candle> bars_1m;
    // 5-minute bars.
    std::vector<Candle> bars_5m;
    // 15-minute bars.
    std::vector<Candle> bars_15m;
    // 60-minute bars.
    std::vector<Candle> bars_60m;
    // Sub-second tick velocity from TickBuffer ($/s, 0 = no data).
    double tick_velocity_1s = 0;
    double tick_velocity_3s = 0;
    // Polymarket token trade-flow imbalance (PmFlowBuffer, tick-rule
    // classified): -1..+1, +1 = flow supports UP. Enabled flag gates the
    // contribution inside predict().
    double pm_flow_bias = 0;
    bool   pm_flow_enabled = false;
    // Cross-asset 5-minute momentum for the correlated reference asset
    // (ETH for a BTC window and vice versa; SOL/XRP reference BTC):
    // % move over the last 5 closed 1m bars. 0 = no data.
    double other_mom_5m_pct = 0;
};

struct PredictorParts {
    double fair_value_up = 0.5;   // market-implied anchor probability
    double rsi14         = 50;
    double macd_hist     = 0;
    double bb_z          = 0;     // (close-mid)/sigma → BB z-score
    double slope_5       = 0;     // last-5 close linreg slope (price/bar)
    double atr_pct       = 0;     // ATR(14)/price (volatility regime)
    double vwap_dev_pct  = 0;     // (close-vwap)/vwap*100
    double oracle_delta_pct = 0;  // (now-strike)/strike*100
    double micro_imbalance  = 0;  // (bid_sz-ask_sz)/(bid_sz+ask_sz)
    double cross_src_dev_pct = 0; // (binance-coinbase)/binance*100
    double tech_score    = 0;     // weighted sum of normalised signals

    // ── Multi-horizon volatility (annualised) ─────────────────────
    // Computed from log returns over the corresponding window. Short windows
    // drive intraday behavior; long windows add regime context from the full
    // imported candle set.
    double rv_1h        = 0;       // last 60 min
    double rv_4h        = 0;       // last 240 min
    double rv_1d        = 0;       // last 1440 min
    double rv_7d        = 0;       // last 10080 min
    double rv_30d       = 0;       // last 30 days, hourly history
    double rv_90d       = 0;       // last 90 days, hourly history
    double rv_1y        = 0;       // last 365 days, hourly history
    double rv_used      = 0;       // blended volatility regime used by predictor

    // ── Multi-timeframe trend (slope as % per minute) ─────────────
    double slope_30m_pct = 0;      // linreg slope over last 30 closes / price
    double slope_4h_pct  = 0;      // last 240 closes
    double slope_1d_pct  = 0;      // last 1440 closes
    double slope_7d_pct  = 0;      // total fitted % move over 7d
    double slope_30d_pct = 0;      // total fitted % move over 30d
    double slope_1y_pct  = 0;      // total fitted % move over 1y

    // ── 24h hi-lo range position ──────────────────────────────────
    // 0.0 = at 24h low, 1.0 = at 24h high. Used for mean-reversion
    // tilt: extremes (>0.85 or <0.15) reduce the trend weight and add
    // a contrarian bias.
    double range_pos_24h = 0.5;
    double range_24h_pct = 0;      // (high-low)/low * 100
    double range_pos_30d = 0.5;
    double range_pos_1y  = 0.5;
    double history_regime_score = 0; // -1..+1 long-history directional bias

    // ── Oracle dynamics ────────────────────────────────────────────
    double oracle_vel_1m_pct      = 0;  // %/min over last 60s
    double oracle_vel_5m_pct      = 0;  // %/min over last 300s last 300s
    double oracle_accel_pct       = 0;  // change in velocity/min

    // ── Down-side microstructure ───────────────────────────────────
    double micro_imbalance_down   = 0;  // (bid_sz-ask_sz)/(bid_sz+ask_sz)

    // ── Volume analysis ────────────────────────────────────────────
    double volume_surge_ratio     = 1.0;  // current 1m vol / 14-bar avg
    double candle_body_conviction = 0;    // |close-open|/(high-low), 0..1

    // ── Trade flow (from Binance) ──────────────────────────────────
    double trade_flow_bias        = 0;   // -1..+1, +1 = all buy aggression

    // ── Sub-second tick momentum ────────────────────────────────────
    double tick_momentum_score   = 0;   // from TickBuffer velocity, -1..+1

    // ── Polymarket token trade flow (tick-rule classified) ──────────
    double pm_flow_bias          = 0;   // -1..+1, +1 = flow supports UP

    // ── Forward-looking leads ───────────────────────────────────────
    double cb_lead_pct           = 0;   // (coinbase-binance)/binance*100, + = CB above
    double other_mom_5m_pct      = 0;   // reference asset 5m momentum

    // ── Signal strength tracking (for gauges and calibration) ──────
    double signal_strength_rsi   = 0;   // normalized |rsi_score|
    double signal_strength_macd  = 0;   // normalized |macd_score|
    double signal_strength_bb    = 0;   // normalized |bb_score|
    double signal_strength_slope = 0;   // normalized |slope_score|
    double signal_strength_micro = 0;   // normalized |micro_score|
    double signal_strength_oracle= 0;   // normalized |oracle_vel_score|
    double signal_strength_trade = 0;   // normalized |trade_flow_score|
    double signal_strength_tick  = 0;   // normalized |tick_momentum_score|
    double signal_strength_pmflow= 0;   // normalized |pm_flow_score|
    double signal_strength_cb    = 0;   // normalized |cb_lead_score|
    double signal_strength_xmom  = 0;   // normalized |other_mom_score|
    double signal_strength_vol   = 0;   // normalized |volume_surge_score|
    double signal_strength_body  = 0;   // normalized |candle_body_score|
    double signal_strength_regime= 0;   // normalized |regime_score|
    int    signals_agreeing      = 0;   // count of signals agreeing with prediction direction
    int    signals_total         = 0;   // total active signals

    // ── Additional indicators (Phase 2.0) ──────────────────────────
    double stoch_k = 50;         // Stochastic %K (0..100)
    double stoch_d = 50;         // Stochastic %D
    double adx     = 0;          // ADX (0..100, >25 = trending)
    double pdi     = 0;          // +DI
    double mdi     = 0;          // -DI
    double cci     = 0;          // CCI (-100..+100 typical)
};

struct PredictorOutput {
    double fair_value_up = 0.5;
    double prediction    = 0.5;   // ensemble-blended P(up wins)
    double edge_up       = 0;     // prediction - up_mid
    double edge_down     = 0;     // (1 - prediction) - down_mid
    double confidence    = 0;     // 0..1
    double rec_size_usd  = 0;     // honours per_click_size_usd / max_order_usd
    Forecast forecast;
    PredictorParts parts;
    std::string    summary;       // one-line human readable
    // ── Confidence trajectory (for ADAPTIVE mode) ─────────────────────
    // Filled by the caller (main.cpp) when adaptive tracking is enabled.
    // Each sample is {timestamp_ms, confidence}.
    std::vector<std::pair<int64_t, double>> conf_trajectory;
    // Recommended entry timing hint: seconds_left when confidence crossed
    // adaptive_target_conf on the latest rising slope. -1 = not yet crossed.
    int64_t adaptive_entry_countdown_s = -1;
    // Current confidence slope (conf/sec). Positive = rising confidence.
    double adaptive_conf_slope = 0.0;
    // Whether the current trajectory satisfies the adaptive entry gate.
    bool adaptive_entry_ready = false;
};

// Always-on checkpoint decision policy. It preserves the configured checkpoint
// cadence, but chooses direction by majority vote across the model, the
// Chainlink window move, and the Coinbase window move. The configured amount is
// always the sizing floor; agreement can only scale it upward.
struct CheckpointDecision {
    bool   buy_up = true;
    int    model_vote = 1;      // +1 UP, -1 DOWN
    int    oracle_vote = 0;     // 0 = below noise floor / unavailable
    int    coinbase_vote = 0;
    int    up_votes = 0;
    int    down_votes = 0;
    int    active_votes = 0;
    double consensus = 0.5;     // winning votes / active votes
    double size_multiplier = 1.0;
};

CheckpointDecision checkpoint_decision(double model_p_up,
                                       double oracle_delta_pct,
                                       double coinbase_delta_pct,
                                       double min_delta_pct,
                                       double max_size_multiplier);

// Selective-entry helpers shared by the runtime and unit tests.
bool checkpoint_votes_unanimous(const CheckpointDecision& decision);
bool checkpoint_pair_allowed(const std::vector<std::string>& pairs,
                             const std::string& asset,
                             int horizon_s);
double checkpoint_win_return_pct(double ask,
                                 double fee_rate,
                                 int fee_exponent);
double checkpoint_net_edge(double fair_probability,
                           double ask,
                           double fee_rate,
                           int fee_exponent);

// ── WPRED calibration ledger ────────────────────────────────────────────
// Tracks, per (pair, fraction, predicted-probability bucket), the realized
// settlement win rate against the model's predicted side probability.
// In-sample gates like "15/17 unanimous wins" overfit tiny samples; this
// ledger is the running out-of-sample correction: once a bucket holds
// enough settled samples, entries are gated off while the bucket's
// realized win rate undershoots its mean predicted probability by more
// than the margin.
// Single-threaded by design: the autopilot sweep (open) and the settlement
// sweep (settle) run on the same refresh thread.
struct WpredCalBucket {
    std::uint32_t n = 0;      // settled samples
    std::uint32_t wins = 0;   // settled winners
    double p_sum = 0.0;       // sum of predicted side probabilities
};

struct WpredCalVerdict {
    bool     allow = true;
    std::uint32_t n = 0;      // settled samples in the bucket
    double   realized = 0.0;  // realized win rate
    double   predicted = 0.0; // mean predicted side probability
};

class WpredCalibration {
public:
    // Register a filled checkpoint entry. pos_key identifies the position
    // (condition_id + side) so the outcome can be consumed at settlement.
    void open(const std::string& bucket_key, const std::string& pos_key,
              double p_side);
    // Consume the oldest open sample for pos_key (no-op if none).
    void settle(const std::string& pos_key, bool won);
    // Evaluate the gate for a candidate entry. Buckets without history
    // always pass; min_samples <= 0 disables the gate.
    WpredCalVerdict gate(const std::string& bucket_key, int min_samples,
                         double margin) const;
    std::size_t open_count() const { return open_.size(); }

private:
    struct OpenSample {
        std::string bucket_key;
        double p_side = 0.0;
    };
    std::unordered_map<std::string, WpredCalBucket> buckets_;
    std::unordered_map<std::string, std::vector<OpenSample>> open_;
};

// Cumulative normal distribution helper (free function, exported for tests).
double norm_cdf(double x);

// Black-Scholes binary option fair value. Returns P(spot_at_T >= strike).
// vol_annual ∈ [0.05, 2.5]; secs_to_expiry > 0; spot, strike > 0.
double bs_binary_call_prob(double spot, double strike,
                            double secs_to_expiry, double vol_annual);

// ── Digital fair value, zero-drift GBM (Phase 5.1) ──────────────────────
// P(spot_T >= strike) under a driftless geometric Brownian motion with
// per-second volatility σ:
//   ln(S_T/S_0) ~ N(-σ²T/2, σ²T)  (Itô correction)
//   P = Φ((ln(S_0/K) − ½σ²T) / (σ√T))
// Principled fair value for 5m/15m crypto binaries: the oracle price series
// IS the underlying, per-second realized vol IS the σ. Returns 0.5 on
// degenerate inputs.
double digital_fv_up_prob(double spot, double strike,
                          double secs_left, double vol_per_sec);

// Run the full ensemble; safe under any input (degrades gracefully).
PredictorOutput predict(const PredictorInput& in);

} // namespace vp::engine
