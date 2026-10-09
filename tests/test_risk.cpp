#include "risk/risk_manager.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <string>

using vp::risk::RiskConfig;
using vp::risk::RiskInput;
using vp::risk::aggregate_failures;
using vp::risk::run_all_checks;

namespace {
RiskInput healthy() {
    RiskInput in;
    in.wallet_baseline_usd = 250.0;
    in.net_pnl_usd = 0.0;
    in.deployed_usd = 5.0;
    in.open_positions_count = 1;
    in.order_notional_usd = 5.0;
    in.now_ms = 1'800'000'000'000;
    return in;
}
} // namespace

int main() {
    // Healthy input passes everything.
    assert(aggregate_failures(run_all_checks(healthy())) == "all_passed");

    // daily_loss_pct = 0 disables the daily-loss checks even when down on
    // the session — a zero limit must not read as "any loss breaches".
    {
        auto in = healthy();
        in.net_pnl_usd = -20.0;
        in.cfg.global_daily_loss_pct = 0.0;
        in.cfg.trader_daily_loss_pct = 0.0;
        assert(aggregate_failures(run_all_checks(in)) == "all_passed");
        // Same loss with the kill enabled breaches.
        in.cfg.global_daily_loss_pct = 5.0;
        in.cfg.trader_daily_loss_pct = 5.0;
        const std::string fails = aggregate_failures(run_all_checks(in));
        assert(fails.find("global_daily_loss_realized") != std::string::npos);
    }

    // consecutive_loss_halt = 0 disables the streak halt.
    {
        auto in = healthy();
        in.consecutive_losses = 9;
        in.cfg.consecutive_loss_halt = 0;
        assert(aggregate_failures(run_all_checks(in)) == "all_passed");
        in.cfg.consecutive_loss_halt = 5;
        assert(aggregate_failures(run_all_checks(in)) !=
               "all_passed");
    }

    // A five-minute loss pause must expire without needing a winning trade
    // that the same halt prevents us from placing.
    {
        auto in = healthy();
        in.consecutive_losses = 3;
        in.cfg.consecutive_loss_halt = 3;
        in.loss_pause_until_ms = in.now_ms + 300'000;
        assert(aggregate_failures(run_all_checks(in)).find("consecutive_loss_halt") != std::string::npos);
        in.now_ms += 299'999;
        assert(aggregate_failures(run_all_checks(in)).find("consecutive_loss_halt") != std::string::npos);
        ++in.now_ms;
        assert(aggregate_failures(run_all_checks(in)) == "all_passed");
    }

    // Reproduce the production CB block: negative paper equity incorrectly
    // made a positive loss limit. The configured budget is $40 * 200% = $80.
    {
        auto in = healthy();
        in.wallet_baseline_usd = 22.39;
        in.net_pnl_usd = -23.77;
        in.daily_loss_basis_usd = 40.0;
        in.cfg.global_daily_loss_pct = in.cfg.trader_daily_loss_pct = 200;
        assert(aggregate_failures(run_all_checks(in)) == "all_passed");
        in.net_pnl_usd = -80.01;
        assert(aggregate_failures(run_all_checks(in)).find("global_daily_loss_realized") != std::string::npos);
        in.loss_period_start_net_pnl_usd = -60;
        assert(aggregate_failures(run_all_checks(in)) == "all_passed");
        in.unrealized_pnl_usd = -60;
        assert(aggregate_failures(run_all_checks(in)).find("global_daily_loss_total") != std::string::npos);
    }

    // Global and trader switches are independent, and the budget cannot
    // shrink merely because losses have reduced equity.
    {
        auto in = healthy();
        in.net_pnl_usd = -12;
        in.cfg.global_daily_loss_pct = 0;
        in.cfg.trader_daily_loss_pct = 5;
        assert(aggregate_failures(run_all_checks(in)) == "all_passed"); // $12.50 budget
        in.net_pnl_usd = -13;
        auto failures = aggregate_failures(run_all_checks(in));
        assert(failures.find("trader_daily_loss_realized") != std::string::npos);
        assert(failures.find("global_daily_loss_realized") == std::string::npos);
    }

    // Gross exposure and notional caps still bite.
    {
        auto in = healthy();
        in.deployed_usd = 25.0;
        in.cfg.max_gross_exposure_usd = 20.0;
        in.cfg.max_deployed_usd = 20.0;
        assert(aggregate_failures(run_all_checks(in)) !=
               "all_passed");
        auto in2 = healthy();
        in2.order_notional_usd = 10.0;
        in2.cfg.max_notional_per_order_usd = 5.0;
        assert(aggregate_failures(run_all_checks(in2)) !=
               "all_passed");
    }

    // Orders-per-cycle cap disabled at <= 0.
    {
        auto in = healthy();
        in.orders_this_cycle = 99;
        in.cfg.max_orders_per_cycle = 0;
        assert(aggregate_failures(run_all_checks(in)) == "all_passed");
    }
}
