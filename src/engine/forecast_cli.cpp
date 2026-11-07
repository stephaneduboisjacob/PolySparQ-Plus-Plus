#include "forecast.hpp"
#include <simdjson.h>
#include <fmt/format.h>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <iterator>
#include <utility>
#include <vector>

int main() {
    try {
        const std::string raw{std::istreambuf_iterator<char>(std::cin), {}};
        if (raw.size() > 4*1024*1024) return 2;
        simdjson::dom::parser parser;
        const auto doc = parser.parse(raw);
        std::vector<std::vector<double>> close_series;
        for (auto series : doc["close_series"].get_array()) {
            std::vector<double> closes;
            for (auto c : series.get_array()) closes.push_back(double(c));
            close_series.push_back(std::move(closes));
        }
        std::vector<vp::engine::VolatilityEstimate> volatilities;
        volatilities.reserve(close_series.size());
        for (const auto& closes : close_series)
            volatilities.push_back(vp::engine::estimate_volatility(closes));
        std::vector<vp::engine::DriftEstimate> drifts(close_series.size());
        std::vector<bool> has_drift(close_series.size(), false);
        std::string result = "[";
        bool first = true;
        for (auto row : doc["requests"].get_array()) {
            auto number = [&](const char* key, double fallback=0) {
                double v; return row[key].get(v) == simdjson::SUCCESS ? v : fallback;
            };
            const double raw_series_index = number("series_index", -1);
            if (!std::isfinite(raw_series_index) || raw_series_index < 0 ||
                raw_series_index >= static_cast<double>(close_series.size()) ||
                std::floor(raw_series_index) != raw_series_index) return 2;
            const auto series_index = static_cast<std::size_t>(raw_series_index);
            const auto& vol = volatilities[series_index];
            const bool use_drift = number("use_drift") > 0;
            if (use_drift && !has_drift[series_index]) {
                drifts[series_index] = vp::engine::estimate_drift(
                    close_series[series_index], vol.sigma_minute);
                has_drift[series_index] = true;
            }
            const vp::engine::DriftEstimate empty_drift{};
            const auto& drift = use_drift ? drifts[series_index] : empty_drift;
            vp::engine::ForecastInput in{number("spot"), number("strike"), number("seconds_left"),
                vol.sigma_minute, number("reference_age_s",-1), number("candle_age_s",-1),
                vol.samples, int(number("twap_window_s",-1)), drift.log_drift_minute};
            const auto f = vp::engine::forecast(in);
            const auto up = vp::engine::execution_edge(f.probability, f.low,
                number("up_bid"), number("up_ask"), number("fee_rate",-1), int(number("fee_exponent",1)));
            const auto dn = vp::engine::execution_edge(1-f.probability, 1-f.high,
                number("down_bid"), number("down_ask"), number("fee_rate",-1), int(number("fee_exponent",1)));
            if (!first) result += ',';
            first = false;
            result += fmt::format(R"({{"probability":{},"low":{},"high":{},"quality":{},"ready":{},"status":"{}","sigma_minute":{},"samples":{},"drift_minute":{},"drift_samples":{},"up_edge":{},"down_edge":{},"up_conservative":{},"down_conservative":{},"up_executable":{},"down_executable":{}}})",
                f.probability,f.low,f.high,f.quality,f.ready,f.status,vol.sigma_minute,vol.samples,
                drift.log_drift_minute,drift.samples,
                up.net,dn.net,up.conservative,dn.conservative,up.executable,dn.executable);
        }
        std::cout << result << "]\n";
    } catch (const std::exception& e) { std::cerr << "Invalid forecast batch\n"; return 2; }
}
