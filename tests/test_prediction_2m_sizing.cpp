#include "config/config.hpp"
#include "engine/prediction_2m_sizing.hpp"

#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

int main() {
    using namespace vp;
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
        ("polysparq-prediction-2m-sizing-" + std::to_string(suffix) + ".toml");
    struct RemoveFile {
        std::filesystem::path path;
        ~RemoveFile() { std::error_code ec; std::filesystem::remove(path, ec); }
    } cleanup{path};

    auto parse = [&](const std::string& strategy) {
        std::ofstream file(path);
        if (!file) throw std::runtime_error("cannot create temporary TOML config");
        file << "[runtime]\nmode='paper'\n"
                "[wallet]\ncredentials_file=''\n"
                "[strategy.hot]\nprediction_2m_enabled=true\n" << strategy;
        file.close();
        return Config::load(path.string());
    };

    const auto usd = parse("prediction_2m_order_usd=25.0\n");
    if (std::abs(usd.strategy.prediction_2m_order_usd - 25.0) > 1e-9)
        throw std::runtime_error("USD clip did not load");
    for (const double price : {0.55, 0.82, 0.93}) {
        const double shares = engine::prediction_2m_shares_for_limit(usd.strategy, price);
        const double notional = shares * price;
        if (shares <= 0.0 || notional > 25.0 + 1e-9 || 25.0 - notional >= price / 100.0 + 1e-9)
            throw std::runtime_error("USD clip did not round down within one cent-share");
    }
    if (engine::prediction_2m_shares_for_limit(usd.strategy, 0.0) != 0.0)
        throw std::runtime_error("invalid price did not fail closed");

    const auto legacy = parse("prediction_2m_order_shares=15.0\n");
    if (legacy.strategy.prediction_2m_order_usd != 0.0 ||
        engine::prediction_2m_shares_for_limit(legacy.strategy, 0.67) != 15.0)
        throw std::runtime_error("legacy fixed-share profile compatibility changed");

    // The value gate and passive offset default to a one-tick quote and a 3%
    // model-over-ask edge requirement; both are operator tunable. The price
    // ceiling defaults below the generic 0.97 hard cap so this path cannot
    // risk a dollar to win a few cents.
    if (legacy.strategy.prediction_2m_maker_offset_ticks != 1 ||
        std::abs(legacy.strategy.prediction_2m_min_edge_pct - 3.0) > 1e-9 ||
        std::abs(legacy.strategy.prediction_2m_max_price - 0.85) > 1e-9 ||
        legacy.strategy.prediction_2m_fade_enabled ||
        std::abs(legacy.strategy.prediction_2m_fade_max_price - 0.40) > 1e-9)
        throw std::runtime_error("prediction_2m gate defaults changed unexpectedly");
    const auto gated = parse(
        "prediction_2m_maker_offset_ticks=2\nprediction_2m_min_edge_pct=4.5\n"
        "prediction_2m_max_price=0.80\n");
    if (gated.strategy.prediction_2m_maker_offset_ticks != 2 ||
        std::abs(gated.strategy.prediction_2m_min_edge_pct - 4.5) > 1e-9 ||
        std::abs(gated.strategy.prediction_2m_max_price - 0.80) > 1e-9)
        throw std::runtime_error("prediction_2m gate overrides did not load");

    const auto fade = parse(
        "prediction_2m_fade_enabled=true\nprediction_2m_fade_order_usd=4.0\n"
        "prediction_2m_fade_max_price=0.30\n");
    if (!fade.strategy.prediction_2m_fade_enabled ||
        std::abs(fade.strategy.prediction_2m_fade_order_usd - 4.0) > 1e-9 ||
        std::abs(fade.strategy.prediction_2m_fade_max_price - 0.30) > 1e-9)
        throw std::runtime_error("prediction_2m fade overrides did not load");

    bool rejected = false;
    try { (void)parse("prediction_2m_order_usd=-1.0\n"); }
    catch (const std::runtime_error&) { rejected = true; }
    if (!rejected) throw std::runtime_error("negative USD clip was accepted");

    rejected = false;
    try { (void)parse("prediction_2m_maker_offset_ticks=99\n"); }
    catch (const std::runtime_error&) { rejected = true; }
    if (!rejected) throw std::runtime_error("out-of-range maker offset was accepted");

    rejected = false;
    try { (void)parse("prediction_2m_min_edge_pct=51.0\n"); }
    catch (const std::runtime_error&) { rejected = true; }
    if (!rejected) throw std::runtime_error("out-of-range edge gate was accepted");

    rejected = false;
    try { (void)parse("prediction_2m_max_price=0.50\n"); }
    catch (const std::runtime_error&) { rejected = true; }
    if (!rejected) throw std::runtime_error("max price below min price was accepted");

    rejected = false;
    try { (void)parse("prediction_2m_max_price=1.0\n"); }
    catch (const std::runtime_error&) { rejected = true; }
    if (!rejected) throw std::runtime_error("max price at 1.0 was accepted");

    rejected = false;
    try { (void)parse("prediction_2m_fade_enabled=true\n"); }
    catch (const std::runtime_error&) { rejected = true; }
    if (!rejected) throw std::runtime_error("enabled fade without a clip was accepted");

    rejected = false;
    try { (void)parse(
        "prediction_2m_fade_enabled=true\nprediction_2m_fade_order_usd=6.0\n"); }
    catch (const std::runtime_error&) { rejected = true; }
    if (!rejected) throw std::runtime_error("fade clip above max_order_usd was accepted");
}
