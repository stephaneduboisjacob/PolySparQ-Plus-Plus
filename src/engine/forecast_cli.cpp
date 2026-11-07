#include "forecast.hpp"
#include <simdjson.h>
#include <fmt/format.h>
#include <algorithm>
#include <iostream>
#include <iterator>
#include <utility>
#include <vector>

namespace {
struct CachedCloseFeatures {
    std::vector<double> closes;
    vp::engine::VolatilityEstimate volatility;
    vp::engine::DriftEstimate drift;
    bool has_drift = false;
};
}

int main() {
    try {
        const std::string raw{std::istreambuf_iterator<char>(std::cin), {}};
        if (raw.size() > 4*1024*1024) return 2;
        simdjson::dom::parser parser;
        const auto doc = parser.parse(raw);
        std::vector<CachedCloseFeatures> close_cache;
        std::string result = "[";
        bool first = true;
        for (auto row : doc.get_array()) {
            auto number = [&](const char* key, double fallback=0) {
                double v; return row[key].get(v) == simdjson::SUCCESS ? v : fallback;
            };
            std::vector<double> closes;
            for (auto c : row["closes"].get_array()) closes.push_back(double(c));
            auto cached = std::find_if(close_cache.begin(), close_cache.end(),
                [&](const auto& entry) { return entry.closes == closes; });
            if (cached == close_cache.end()) {
                close_cache.push_back({std::move(closes), {}, {}, false});
                cached = std::prev(close_cache.end());
                cached->volatility = vp::engine::estimate_volatility(cached->closes);
            }
            const auto& vol = cached->volatility;
            const bool use_drift = number("use_drift") > 0;
            if (use_drift && !cached->has_drift) {
                cached->drift = vp::engine::estimate_drift(
                    cached->closes, vol.sigma_minute);
                cached->has_drift = true;
            }
            const auto drift = use_drift ? cached->drift : vp::engine::DriftEstimate{};
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
