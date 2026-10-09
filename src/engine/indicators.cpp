// Technical indicators implementation. See indicators.hpp for contracts.
#include "indicators.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace vp::engine::ind {

// ──────────────────────────────────────────────────────────────────
// Moving averages
// ──────────────────────────────────────────────────────────────────
double sma(const std::vector<double>& v, int period) {
    if ((int)v.size() < period || period <= 0) return 0.0;
    double s = 0;
    for (int i = (int)v.size() - period; i < (int)v.size(); ++i) s += v[i];
    return s / period;
}

double ema(const std::vector<double>& v, int period) {
    if (v.empty() || period <= 0) return 0.0;
    const double k = 2.0 / (period + 1);
    double e = v.front();
    for (size_t i = 1; i < v.size(); ++i) e = v[i] * k + e * (1 - k);
    return e;
}

// ──────────────────────────────────────────────────────────────────
// RSI — Wilder smoothing
// ──────────────────────────────────────────────────────────────────
std::optional<double> rsi(const std::vector<double>& closes, int period) {
    if ((int)closes.size() < period + 1) return std::nullopt;
    double gain = 0, loss = 0;
    // First seed window
    for (int i = 1; i <= period; ++i) {
        double d = closes[i] - closes[i - 1];
        if (d >= 0) gain += d; else loss -= d;
    }
    double avg_g = gain / period;
    double avg_l = loss / period;
    // Smoothed remainder
    for (int i = period + 1; i < (int)closes.size(); ++i) {
        double d = closes[i] - closes[i - 1];
        double g = d > 0 ? d : 0.0;
        double l = d < 0 ? -d : 0.0;
        avg_g = (avg_g * (period - 1) + g) / period;
        avg_l = (avg_l * (period - 1) + l) / period;
    }
    if (avg_l == 0) return 100.0;
    double rs = avg_g / avg_l;
    return 100.0 - 100.0 / (1.0 + rs);
}

// ──────────────────────────────────────────────────────────────────
// MACD
// ──────────────────────────────────────────────────────────────────
static std::vector<double> ema_series(const std::vector<double>& v, int period) {
    std::vector<double> out;
    out.reserve(v.size());
    if (v.empty()) return out;
    const double k = 2.0 / (period + 1);
    double e = v.front();
    out.push_back(e);
    for (size_t i = 1; i < v.size(); ++i) {
        e = v[i] * k + e * (1 - k);
        out.push_back(e);
    }
    return out;
}

std::optional<MacdOut> macd(const std::vector<double>& closes,
                            int fast, int slow, int sig) {
    if ((int)closes.size() < slow + sig) return std::nullopt;
    auto ef = ema_series(closes, fast);
    auto es = ema_series(closes, slow);
    std::vector<double> macd_line(closes.size());
    for (size_t i = 0; i < closes.size(); ++i)
        macd_line[i] = ef[i] - es[i];
    auto sig_line = ema_series(macd_line, sig);
    MacdOut o;
    o.macd   = macd_line.back();
    o.signal = sig_line.back();
    o.hist   = o.macd - o.signal;
    return o;
}

// ──────────────────────────────────────────────────────────────────
// ATR (Wilder)
// ──────────────────────────────────────────────────────────────────
std::optional<double> atr(const std::vector<Candle>& bars, int period) {
    if ((int)bars.size() < period + 1) return std::nullopt;
    auto tr = [](const Candle& c, double prev_close) {
        double a = c.high - c.low;
        double b = std::abs(c.high - prev_close);
        double d = std::abs(c.low  - prev_close);
        return std::max({a, b, d});
    };
    double atr_v = 0;
    for (int i = 1; i <= period; ++i) atr_v += tr(bars[i], bars[i - 1].close);
    atr_v /= period;
    for (int i = period + 1; i < (int)bars.size(); ++i)
        atr_v = (atr_v * (period - 1) + tr(bars[i], bars[i - 1].close)) / period;
    return atr_v;
}

// ──────────────────────────────────────────────────────────────────
// Bollinger Bands
// ──────────────────────────────────────────────────────────────────
std::optional<BbOut> bbands(const std::vector<double>& closes,
                            int period, double k) {
    if ((int)closes.size() < period) return std::nullopt;
    double mean = 0;
    for (int i = (int)closes.size() - period; i < (int)closes.size(); ++i)
        mean += closes[i];
    mean /= period;
    double var = 0;
    for (int i = (int)closes.size() - period; i < (int)closes.size(); ++i) {
        double d = closes[i] - mean;
        var += d * d;
    }
    double sd = std::sqrt(var / period);
    return BbOut{mean, mean + k * sd, mean - k * sd};
}

// ──────────────────────────────────────────────────────────────────
// Realized vol — annualised from 1-min log returns
// ──────────────────────────────────────────────────────────────────
double realized_vol_annualised(const std::vector<double>& closes_1m, int window) {
    if ((int)closes_1m.size() < std::max(window, 2)) return 0.50;
    int n = std::min(window, (int)closes_1m.size());
    int start = (int)closes_1m.size() - n;
    std::vector<double> logret;
    logret.reserve(n - 1);
    for (int i = start + 1; i < (int)closes_1m.size(); ++i) {
        if (closes_1m[i] > 0 && closes_1m[i - 1] > 0)
            logret.push_back(std::log(closes_1m[i] / closes_1m[i - 1]));
    }
    if (logret.size() < 2) return 0.50;
    double mean = std::accumulate(logret.begin(), logret.end(), 0.0) / logret.size();
    double var  = 0;
    for (double r : logret) { double d = r - mean; var += d * d; }
    var /= (logret.size() - 1);
    double sd  = std::sqrt(var);
    double vol = sd * std::sqrt(525600.0); // minutes per year
    return std::clamp(vol, 0.05, 2.50);
}

// ──────────────────────────────────────────────────────────────────
// Linear regression slope (closes per bar)
// ──────────────────────────────────────────────────────────────────
std::optional<double> linreg_slope(const std::vector<double>& closes, int period) {
    if ((int)closes.size() < period || period < 2) return std::nullopt;
    int start = (int)closes.size() - period;
    double sx = 0, sy = 0, sxy = 0, sxx = 0;
    for (int i = 0; i < period; ++i) {
        double x = i;
        double y = closes[start + i];
        sx += x; sy += y; sxy += x * y; sxx += x * x;
    }
    double denom = period * sxx - sx * sx;
    if (denom == 0) return 0.0;
    return (period * sxy - sx * sy) / denom;
}

// ──────────────────────────────────────────────────────────────────
// Z-score
// ──────────────────────────────────────────────────────────────────
std::optional<double> zscore(const std::vector<double>& closes, int period) {
    if ((int)closes.size() < period) return std::nullopt;
    double mean = 0;
    for (int i = (int)closes.size() - period; i < (int)closes.size(); ++i)
        mean += closes[i];
    mean /= period;
    double var = 0;
    for (int i = (int)closes.size() - period; i < (int)closes.size(); ++i) {
        double d = closes[i] - mean;
        var += d * d;
    }
    double sd = std::sqrt(var / period);
    if (sd == 0) return 0.0;
    return (closes.back() - mean) / sd;
}

// ──────────────────────────────────────────────────────────────────
// VWAP
// ──────────────────────────────────────────────────────────────────
std::optional<double> vwap(const std::vector<Candle>& bars, int period) {
    if ((int)bars.size() < period || period <= 0) return std::nullopt;
    double pv = 0, vv = 0;
    for (int i = (int)bars.size() - period; i < (int)bars.size(); ++i) {
        double tp = (bars[i].high + bars[i].low + bars[i].close) / 3.0;
        pv += tp * bars[i].volume;
        vv += bars[i].volume;
    }
    if (vv == 0) return bars.back().close;
    return pv / vv;
}

// ──────────────────────────────────────────────────────────────────
// OBV delta
// ──────────────────────────────────────────────────────────────────
double obv_delta(const std::vector<Candle>& bars) {
    if (bars.size() < 2) return 0.0;
    double obv = 0;
    for (size_t i = 1; i < bars.size(); ++i) {
        if (bars[i].close > bars[i - 1].close) obv += bars[i].volume;
        else if (bars[i].close < bars[i - 1].close) obv -= bars[i].volume;
    }
    return obv;
}

// ──────────────────────────────────────────────────────────────────
// Stochastic oscillator
// ──────────────────────────────────────────────────────────────────
std::optional<StochOut> stoch(const std::vector<Candle>& bars, int k_period, int d_period) {
    if ((int)bars.size() < k_period) return std::nullopt;
    double hi = -1e18, lo = 1e18;
    int start = (int)bars.size() - k_period;
    for (int i = start; i < (int)bars.size(); ++i) {
        hi = std::max(hi, bars[i].high);
        lo = std::min(lo, bars[i].low);
    }
    if (hi <= lo) return std::nullopt;
    double k = 100.0 * (bars.back().close - lo) / (hi - lo);
    // Simple %D = SMA of %K over d_period (use closes as proxy if not enough history)
    double d = k;
    if ((int)bars.size() >= k_period + d_period - 1) {
        double dsum = 0;
        int dstart = (int)bars.size() - d_period;
        for (int i = dstart; i < (int)bars.size(); ++i) {
            double dhi = -1e18, dlo = 1e18;
            int kstart = std::max(0, i - k_period + 1);
            for (int j = kstart; j <= i; ++j) {
                dhi = std::max(dhi, bars[j].high);
                dlo = std::min(dlo, bars[j].low);
            }
            if (dhi > dlo)
                dsum += 100.0 * (bars[i].close - dlo) / (dhi - dlo);
        }
        d = dsum / d_period;
    }
    return StochOut{k, d};
}

// ──────────────────────────────────────────────────────────────────
// ADX (Average Directional Index)
// ──────────────────────────────────────────────────────────────────
std::optional<AdxOut> adx(const std::vector<Candle>& bars, int period) {
    if ((int)bars.size() < period + 1) return std::nullopt;
    std::vector<double> tr(bars.size()), pdi(bars.size()), mdi(bars.size());
    for (size_t i = 1; i < bars.size(); ++i) {
        double a = bars[i].high - bars[i].low;
        double b = std::abs(bars[i].high - bars[i - 1].close);
        double d = std::abs(bars[i].low - bars[i - 1].close);
        tr[i] = std::max({a, b, d});
        double up = bars[i].high - bars[i - 1].high;
        double dn = bars[i - 1].low - bars[i].low;
        pdi[i] = (up > dn && up > 0) ? up : 0.0;
        mdi[i] = (dn > up && dn > 0) ? dn : 0.0;
    }
    // Smooth with Wilder
    double atr_sum = 0, pdi_sum = 0, mdi_sum = 0;
    for (int i = 1; i <= period; ++i) {
        atr_sum += tr[i];
        pdi_sum += pdi[i];
        mdi_sum += mdi[i];
    }
    double atr = atr_sum / period;
    double p = pdi_sum / period;
    double m = mdi_sum / period;
    std::vector<double> dx_vals;
    for (size_t i = period + 1; i < bars.size(); ++i) {
        atr = (atr * (period - 1) + tr[i]) / period;
        p   = (p * (period - 1) + pdi[i]) / period;
        m   = (m * (period - 1) + mdi[i]) / period;
        if (atr > 0) {
            double dx = 100.0 * std::abs(p - m) / (p + m);
            dx_vals.push_back(dx);
        }
    }
    if (dx_vals.empty()) return std::nullopt;
    double adx = 0;
    int n = (int)dx_vals.size();
    if (n >= period) {
        for (int i = 0; i < period; ++i) adx += dx_vals[i];
        adx /= period;
        for (int i = period; i < n; ++i)
            adx = (adx * (period - 1) + dx_vals[i]) / period;
    } else {
        for (double v : dx_vals) adx += v;
        adx /= n;
    }
    return AdxOut{adx, p, m};
}

// ──────────────────────────────────────────────────────────────────
// CCI (Commodity Channel Index)
// ──────────────────────────────────────────────────────────────────
std::optional<double> cci(const std::vector<Candle>& bars, int period) {
    if ((int)bars.size() < period) return std::nullopt;
    std::vector<double> tp(period);
    for (int i = 0; i < period; ++i)
        tp[i] = (bars[bars.size() - period + i].high + bars[bars.size() - period + i].low + bars[bars.size() - period + i].close) / 3.0;
    double mean_tp = std::accumulate(tp.begin(), tp.end(), 0.0) / period;
    double mad = 0;
    for (double v : tp) mad += std::abs(v - mean_tp);
    mad /= period;
    if (mad == 0) return 0.0;
    double cur_tp = (bars.back().high + bars.back().low + bars.back().close) / 3.0;
    return (cur_tp - mean_tp) / (0.015 * mad);
}

} // namespace vp::engine::ind
