#include "risk/risk_manager.hpp"
#include <fmt/format.h>
#include <algorithm>

namespace vp::risk {

std::vector<RiskCheck> run_all_checks(const RiskInput& in) {
    std::vector<RiskCheck> results;
    results.reserve(12);

    const double basis = in.daily_loss_basis_usd > 0.0
        ? in.daily_loss_basis_usd : in.wallet_baseline_usd;
    const double period_pnl = in.net_pnl_usd - in.loss_period_start_net_pnl_usd;

    // pct <= 0 disables the daily-loss kill (the executor-level daily kill in
    // main.cpp shares the knob) — record a passed sentinel instead of treating
    // a zero limit as "any loss breaches".
    const bool daily_loss_enabled = in.cfg.global_daily_loss_pct > 0.0;

    // 1. Global daily loss — realized only
    {
        RiskCheck rc;
        rc.key = "global_daily_loss_realized";
        const double limit = -basis * (in.cfg.global_daily_loss_pct / 100.0);
        rc.score = (daily_loss_enabled && basis > 0) ? (period_pnl / limit) : 0.0;
        rc.passed = !daily_loss_enabled || period_pnl >= limit;
        if (!rc.passed || rc.score > 0.8) {
            rc.detail = fmt::format("period realized {:+.2f} exceeds {:.2f} limit ({:.0f}% of ${:.2f} risk basis)",
                                    period_pnl, limit, in.cfg.global_daily_loss_pct, basis);
        }
        results.push_back(std::move(rc));
    }

    // 2. Global daily loss — realized + unrealized (mark-to-market)
    {
        RiskCheck rc;
        rc.key = "global_daily_loss_total";
        const double limit = -basis * (in.cfg.global_daily_loss_pct / 100.0);
        const double total_pnl = period_pnl + in.unrealized_pnl_usd;
        rc.score = (daily_loss_enabled && basis > 0) ? (total_pnl / limit) : 0.0;
        rc.passed = !daily_loss_enabled || total_pnl >= limit;
        if (!rc.passed || rc.score > 0.8) {
            rc.detail = fmt::format("total (realized+unreal) {:+.2f} exceeds {:.2f} limit",
                                    total_pnl, limit);
        }
        results.push_back(std::move(rc));
    }

    // 3. Trader daily loss — realized only (same as global for single-trader bot)
    {
        RiskCheck rc;
        rc.key = "trader_daily_loss_realized";
        const bool trader_enabled = in.cfg.trader_daily_loss_pct > 0.0;
        const double limit = -basis * (in.cfg.trader_daily_loss_pct / 100.0);
        rc.score = (trader_enabled && basis > 0) ? (period_pnl / limit) : 0.0;
        rc.passed = !trader_enabled || period_pnl >= limit;
        if (!rc.passed || rc.score > 0.8) {
            rc.detail = fmt::format("trader realized {:+.2f} exceeds {:.2f} limit",
                                    period_pnl, limit);
        }
        results.push_back(std::move(rc));
    }

    // 4. Trader daily loss — realized + unrealized
    {
        RiskCheck rc;
        rc.key = "trader_daily_loss_total";
        const bool trader_enabled = in.cfg.trader_daily_loss_pct > 0.0;
        const double limit = -basis * (in.cfg.trader_daily_loss_pct / 100.0);
        const double total_pnl = period_pnl + in.unrealized_pnl_usd;
        rc.score = (trader_enabled && basis > 0) ? (total_pnl / limit) : 0.0;
        rc.passed = !trader_enabled || total_pnl >= limit;
        if (!rc.passed || rc.score > 0.8) {
            rc.detail = fmt::format("trader total (realized+unreal) {:+.2f} exceeds {:.2f} limit",
                                    total_pnl, limit);
        }
        results.push_back(std::move(rc));
    }

    // 5. Consecutive loss halt
    {
        RiskCheck rc;
        rc.key = "consecutive_loss_halt";
        rc.score = in.cfg.consecutive_loss_halt > 0
            ? static_cast<double>(in.consecutive_losses) / in.cfg.consecutive_loss_halt
            : 0.0;
        // halt <= 0 disables the streak halt (main.cpp shares the knob).
        const bool pause_finished = in.loss_pause_until_ms > 0 &&
                                    in.now_ms >= in.loss_pause_until_ms;
        rc.passed = in.cfg.consecutive_loss_halt <= 0 || pause_finished ||
                    in.consecutive_losses < in.cfg.consecutive_loss_halt;
        if (!rc.passed || rc.score > 0.8) {
            rc.detail = fmt::format("consecutive losses {} >= halt threshold {}",
                                    in.consecutive_losses, in.cfg.consecutive_loss_halt);
        }
        results.push_back(std::move(rc));
    }

    // 6. Consecutive loss cooldown
    {
        RiskCheck rc;
        rc.key = "consecutive_loss_cooldown";
        const bool in_cooldown = (in.last_loss_ts_ms > 0) &&
                                 (in.now_ms - in.last_loss_ts_ms <
                                  static_cast<int64_t>(in.cfg.consecutive_loss_cooldown_s) * 1000LL);
        rc.score = in_cooldown ? 1.0 : 0.0;
        rc.passed = !in_cooldown;
        if (!rc.passed) {
            const int64_t elapsed_s = (in.now_ms - in.last_loss_ts_ms) / 1000;
            const int64_t remaining_s = in.cfg.consecutive_loss_cooldown_s - elapsed_s;
            rc.detail = fmt::format("in cooldown: {}s elapsed, {}s remaining",
                                    elapsed_s, remaining_s);
        }
        results.push_back(std::move(rc));
    }

    // 7. Orders per cycle (per market window)
    {
        RiskCheck rc;
        rc.key = "orders_per_cycle";
        rc.score = in.cfg.max_orders_per_cycle > 0
            ? static_cast<double>(in.orders_this_cycle) / in.cfg.max_orders_per_cycle
            : 0.0;
        // max <= 0 disables the per-cycle cap.
        rc.passed = in.cfg.max_orders_per_cycle <= 0 ||
                    in.orders_this_cycle < in.cfg.max_orders_per_cycle;
        if (!rc.passed || rc.score > 0.8) {
            rc.detail = fmt::format("orders this cycle {} >= max {}",
                                    in.orders_this_cycle, in.cfg.max_orders_per_cycle);
        }
        results.push_back(std::move(rc));
    }

    // 8. Notional per order
    {
        RiskCheck rc;
        rc.key = "notional_per_order";
        rc.score = in.cfg.max_notional_per_order_usd > 0
            ? in.order_notional_usd / in.cfg.max_notional_per_order_usd
            : 0.0;
        rc.passed = in.order_notional_usd <= in.cfg.max_notional_per_order_usd;
        if (!rc.passed || rc.score > 0.8) {
            rc.detail = fmt::format("order notional ${:.2f} >= max ${:.2f}",
                                    in.order_notional_usd, in.cfg.max_notional_per_order_usd);
        }
        results.push_back(std::move(rc));
    }

    // 9. Gross exposure (sum of absolute position values across all markets)
    {
        RiskCheck rc;
        rc.key = "gross_exposure";
        rc.score = in.cfg.max_gross_exposure_usd > 0
            ? in.deployed_usd / in.cfg.max_gross_exposure_usd
            : 0.0;
        rc.passed = in.deployed_usd <= in.cfg.max_gross_exposure_usd;
        if (!rc.passed || rc.score > 0.8) {
            rc.detail = fmt::format("gross exposure ${:.2f} >= max ${:.2f}",
                                    in.deployed_usd, in.cfg.max_gross_exposure_usd);
        }
        results.push_back(std::move(rc));
    }

    // 10. Deployed capital (sum of shares * avg_price)
    {
        RiskCheck rc;
        rc.key = "deployed_capital";
        rc.score = in.cfg.max_deployed_usd > 0
            ? in.deployed_usd / in.cfg.max_deployed_usd
            : 0.0;
        rc.passed = in.deployed_usd <= in.cfg.max_deployed_usd;
        if (!rc.passed || rc.score > 0.8) {
            rc.detail = fmt::format("deployed ${:.2f} >= max ${:.2f}",
                                    in.deployed_usd, in.cfg.max_deployed_usd);
        }
        results.push_back(std::move(rc));
    }

    // 11. Open orders count
    {
        RiskCheck rc;
        rc.key = "open_orders_count";
        rc.score = in.cfg.max_open_orders > 0
            ? static_cast<double>(in.open_orders_count) / in.cfg.max_open_orders
            : 0.0;
        rc.passed = in.open_orders_count < in.cfg.max_open_orders;
        if (!rc.passed || rc.score > 0.8) {
            rc.detail = fmt::format("open orders {} >= max {}",
                                    in.open_orders_count, in.cfg.max_open_orders);
        }
        results.push_back(std::move(rc));
    }

    // 12. Open positions count
    {
        RiskCheck rc;
        rc.key = "open_positions_count";
        rc.score = in.cfg.max_open_positions > 0
            ? static_cast<double>(in.open_positions_count) / in.cfg.max_open_positions
            : 0.0;
        rc.passed = in.open_positions_count < in.cfg.max_open_positions;
        if (!rc.passed || rc.score > 0.8) {
            rc.detail = fmt::format("open positions {} >= max {}",
                                    in.open_positions_count, in.cfg.max_open_positions);
        }
        results.push_back(std::move(rc));
    }

    // 13. Positions per market (UP+DOWN)
    {
        RiskCheck rc;
        rc.key = "positions_per_market";
        rc.score = in.cfg.max_positions_per_market > 0
            ? static_cast<double>(in.positions_in_this_market) / in.cfg.max_positions_per_market
            : 0.0;
        rc.passed = in.positions_in_this_market < in.cfg.max_positions_per_market;
        if (!rc.passed || rc.score > 0.8) {
            rc.detail = fmt::format("positions in this market {} >= max {}",
                                    in.positions_in_this_market, in.cfg.max_positions_per_market);
        }
        results.push_back(std::move(rc));
    }

    // 14. Per-market exposure
    {
        RiskCheck rc;
        rc.key = "exposure_per_market";
        const double new_exposure = in.exposure_this_market_usd + in.order_notional_usd;
        rc.score = in.cfg.max_exposure_per_market_usd > 0
            ? new_exposure / in.cfg.max_exposure_per_market_usd
            : 0.0;
        rc.passed = new_exposure <= in.cfg.max_exposure_per_market_usd;
        if (!rc.passed || rc.score > 0.8) {
            rc.detail = fmt::format("exposure this market ${:.2f} (+${:.2f} order) >= max ${:.2f}",
                                    in.exposure_this_market_usd, in.order_notional_usd,
                                    in.cfg.max_exposure_per_market_usd);
        }
        results.push_back(std::move(rc));
    }

    return results;
}

std::string aggregate_failures(const std::vector<RiskCheck>& checks) {
    std::string out;
    for (const auto& rc : checks) {
        if (!rc.passed) {
            if (!out.empty()) out += "; ";
            out += rc.key + ": " + rc.detail;
        }
    }
    return out.empty() ? "all_passed" : out;
}

} // namespace vp::risk
