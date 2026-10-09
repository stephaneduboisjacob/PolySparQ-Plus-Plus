#include "engine/predictor.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <vector>

int main() {
    using vp::engine::checkpoint_decision;

    {
        const auto d = checkpoint_decision(0.60, 0.0, 0.0, 0.005, 2.0);
        assert(d.buy_up);
        assert(d.active_votes == 1);
        assert(d.size_multiplier == 1.0);
    }
    {
        const auto d = checkpoint_decision(0.60, -0.03, -0.02, 0.005, 2.0);
        assert(!d.buy_up);
        assert(d.up_votes == 1 && d.down_votes == 2);
        assert(std::abs(d.consensus - 2.0 / 3.0) < 1e-12);
        assert(std::abs(d.size_multiplier - 1.5) < 1e-12);
    }
    {
        const auto d = checkpoint_decision(0.70, 0.03, 0.02, 0.005, 2.0);
        assert(d.buy_up);
        assert(d.up_votes == 3 && d.down_votes == 0);
        assert(d.consensus == 1.0);
        assert(d.size_multiplier == 2.0);
        assert(vp::engine::checkpoint_votes_unanimous(d));
    }
    {
        // Split evidence falls back to the model and never scales below base.
        const auto d = checkpoint_decision(0.55, -0.03, 0.001, 0.005, 3.0);
        assert(d.buy_up);
        assert(d.up_votes == 1 && d.down_votes == 1);
        assert(d.size_multiplier == 1.0);
        assert(!vp::engine::checkpoint_votes_unanimous(d));
    }

    {
        const std::vector<std::string> pairs{"SOL:300", "XRP:900"};
        assert(vp::engine::checkpoint_pair_allowed(pairs, "SOL", 300));
        assert(vp::engine::checkpoint_pair_allowed(pairs, "XRP", 900));
        assert(!vp::engine::checkpoint_pair_allowed(pairs, "SOL", 900));
        assert(!vp::engine::checkpoint_pair_allowed(pairs, "ETH", 300));
        assert(vp::engine::checkpoint_pair_allowed({}, "ETH", 300));
    }

    {
        // At an 80-cent ask, the configured fee is 1.152 cents/share and
        // the winning return after that entry fee is about 23.23%.
        const double r = vp::engine::checkpoint_win_return_pct(0.80, 0.072, 1);
        assert(std::abs(r - 23.2256) < 0.001);
        assert(!std::isfinite(
            vp::engine::checkpoint_win_return_pct(1.0, 0.072, 1)));
    }

    {
        // Edge is measured against fee-adjusted cost, not displayed ask.
        const double positive =
            vp::engine::checkpoint_net_edge(0.75, 0.70, 0.07, 1);
        assert(std::abs(positive - 0.0353) < 1e-12);
        assert(vp::engine::checkpoint_net_edge(0.70, 0.70, 0.07, 1) < 0.0);
        assert(!std::isfinite(
            vp::engine::checkpoint_net_edge(1.2, 0.70, 0.07, 1)));
    }

    {
        vp::engine::PredictorInput in;
        in.asset = "ETH";
        in.horizon_s = 300;
        in.secs_left = 120;
        in.asset_price = 100.0;
        in.strike = 100.0;
        in.up_mid = 0.5;
        in.down_mid = 0.5;
        in.up_bid = 0.49;
        in.up_ask = 0.51;
        in.down_bid = 0.49;
        in.down_ask = 0.51;
        const auto out = vp::engine::predict(in);
        assert(std::abs(out.prediction - 0.5) < 1e-12);
        assert(std::abs(out.confidence) < 1e-12);
    }

    // ── WpredCalibration ────────────────────────────────────────────────
    {
        using vp::engine::WpredCalibration;
        WpredCalibration cal;
        const std::string key = "SOL:300:0.60:0.65";

        // No history -> allowed, verdict reports zeros.
        auto v0 = cal.gate(key, 30, 0.02);
        assert(v0.allow && v0.n == 0);

        // One filled entry, settled: below min_samples -> still allowed,
        // but the bucket stats are visible.
        cal.open(key, "pos0|UP", 0.66);
        assert(cal.open_count() == 1);
        cal.settle("pos0|UP", true);
        assert(cal.open_count() == 0);
        auto v1 = cal.gate(key, 30, 0.02);
        assert(v1.allow && v1.n == 1);
        assert(std::abs(v1.realized - 1.0) < 1e-12);
        assert(std::abs(v1.predicted - 0.66) < 1e-12);

        // Fill the bucket with an underperforming history: predicted 0.66,
        // realized 15/30 = 0.5 -> gated at min_samples.
        for (int i = 0; i < 29; ++i) {
            const std::string pk = "pos" + std::to_string(i + 1) + "|UP";
            cal.open(key, pk, 0.66);
            cal.settle(pk, i < 14);  // 14 more wins -> 15/30
        }
        auto v2 = cal.gate(key, 30, 0.02);
        assert(!v2.allow && v2.n == 30);
        assert(std::abs(v2.realized - 0.5) < 1e-12);
        assert(std::abs(v2.predicted - 0.66) < 1e-12);

        // A wide margin lets the bucket trade again.
        assert(cal.gate(key, 30, 0.20).allow);
        // min_samples = 0 disables the gate entirely.
        assert(cal.gate(key, 0, 0.02).allow);
        // Untracked positions settle as no-ops.
        cal.settle("missing|DOWN", true);
        assert(cal.open_count() == 0);
        // Different buckets stay independent.
        assert(cal.gate("BTC:300:0.60:0.55", 30, 0.02).allow);
    }
}
