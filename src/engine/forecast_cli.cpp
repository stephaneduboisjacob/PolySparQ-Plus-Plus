#include "forecast.hpp"
#include <simdjson.h>
#include <fmt/format.h>
#include <iostream>
#include <iterator>
#include <vector>
int main() {
    try {
        const std::string raw{std::istreambuf_iterator<char>(std::cin), {}};
        if (raw.size() > 4*1024*1024) return 2;
        simdjson::dom::parser parser;
        const auto doc = parser.parse(raw);
        std::string result = "[";
        bool first = true;
        for (auto row : doc.get_array()) {
            auto number = [&](const char* key, double fallback=0) {
                double v; return row[key].get(v) == simdjson::SUCCESS ? v : fallback;
            };
            std::vector<double> closes;
            for (auto c : row["closes"].get_array()) closes.push_back(double(c));
            auto vol = vp::engine::estimate_volatility(closes);
            const auto drift = number("use_drift") > 0
                ? vp::engine::estimate_drift(closes, vol.sigma_minute)
                : vp::engine::DriftEstimate{};
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
