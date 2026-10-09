#pragma once
// Technical indicators — pure functions over price/candle vectors.
// All functions are stateless and thread-safe (no shared state).
//
// Inputs are typically the most-recent N closes from the candle store;
// callers slice their own windows. Returns nullopt when there isn't
// enough data to produce a meaningful value.
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace vp::engine {

struct Candle {
    int64_t ts_ms = 0;   // bar open
    double  open  = 0;
    double  high  = 0;
    double  low   = 0;
    double  close = 0;
    double  volume= 0;
    double  buy_volume  = 0;  // volume from buy-aggressor trades
    double  sell_volume = 0;  // volume from sell-aggressor trades
};

namespace ind {

// ── Moving averages ───────────────────────────────────────────────
double sma(const std::vector<double>& v, int period);
double ema(const std::vector<double>& v, int period);

// ── Oscillators ───────────────────────────────────────────────────
// RSI(period). Returns 0..100. Wilder's smoothing.
std::optional<double> rsi(const std::vector<double>& closes, int period = 14);

// MACD(12,26,9): returns {macd, signal, histogram}.
struct MacdOut { double macd; double signal; double hist; };
std::optional<MacdOut> macd(const std::vector<double>& closes,
                            int fast = 12, int slow = 26, int sig = 9);

// ── Volatility ────────────────────────────────────────────────────
// Average True Range — needs OHLC.
std::optional<double> atr(const std::vector<Candle>& bars, int period = 14);

// Bollinger Bands(period=20, k=2). Returns {middle, upper, lower}.
struct BbOut { double mid; double up; double dn; };
std::optional<BbOut> bbands(const std::vector<double>& closes,
                            int period = 20, double k = 2.0);

// Annualised realized volatility from log returns of 1-min closes.
// Annualisation factor sqrt(525600). Returns clamped 0.05..2.50.
double realized_vol_annualised(const std::vector<double>& closes_1m,
                               int window = 60);

// ── Trend / regression ────────────────────────────────────────────
// Slope of linear regression over the last `period` closes (price/bar).
// Positive = uptrend, negative = downtrend. nullopt if insufficient data.
std::optional<double> linreg_slope(const std::vector<double>& closes, int period);

// Z-score of latest close vs window mean / stddev.
std::optional<double> zscore(const std::vector<double>& closes, int period);

// ── Volume ─────────────────────────────────────────────────────────
// VWAP from rolling window of (price, volume) candles.
std::optional<double> vwap(const std::vector<Candle>& bars, int period);

// On-Balance Volume — cumulative; pass last N bars, returns delta over window.
double obv_delta(const std::vector<Candle>& bars);

// ── Momentum oscillators ────────────────────────────────────────
// Stochastic oscillator %K (fast) and %D (slow). Returns {k, d}.
struct StochOut { double k; double d; };
std::optional<StochOut> stoch(const std::vector<Candle>& bars, int k_period = 14, int d_period = 3);

// Average Directional Index — trend strength. Returns ADX, +DI, -DI.
struct AdxOut { double adx; double pdi; double mdi; };
std::optional<AdxOut> adx(const std::vector<Candle>& bars, int period = 14);

// Commodity Channel Index — overbought/oversold. Returns CCI.
std::optional<double> cci(const std::vector<Candle>& bars, int period = 20);

} // namespace ind
} // namespace vp::engine
