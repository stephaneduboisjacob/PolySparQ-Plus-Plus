#include "scalp_manager.hpp"
#include "tick_buffer.hpp"
#include "exec/positions.hpp"  // Position
#include "feeds/polymarket_rest.hpp"  // PmMarket
#include <algorithm>
#include <cmath>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

namespace vp::engine {

ScalpManager::ScalpManager(TickBuffer& tick_buf)
    : ticks_(tick_buf) {}

// ── Entry ─────────────────────────────────────────────────────────────

std::optional<ScalpEntrySignal> ScalpManager::check_entry(
    const PmMarket& market,
    double up_mid, double down_mid,
    double up_vel, double down_vel,
    int64_t now_us, int secs_left, int entries_so_far) {

    if (!cfg.enabled) return std::nullopt;
    if (secs_left < cfg.min_secs || secs_left > cfg.max_secs) return std::nullopt;
    if (entries_so_far >= cfg.max_entries_per_window) return std::nullopt;

    {
        std::lock_guard<std::mutex> lk(mu_);
        int64_t latest_action_us = 0;
        for (ShareSide s : {ShareSide::UP, ShareSide::DOWN}) {
            auto it = positions_.find(mk_key(market.condition_id, s));
            if (it == positions_.end()) continue;
            const auto& ps = it->second;
            if (!ps.exited) return std::nullopt;
            latest_action_us = std::max(latest_action_us,
                                        std::max(ps.entry_us, ps.exit_us));
        }
        if (cfg.cooldown_us > 0 && latest_action_us > 0 &&
            now_us - latest_action_us < cfg.cooldown_us) {
            return std::nullopt;
        }
    }

    // Compute accelerations to use as a tiebreaker
    double up_accel   = ticks_.acceleration_us(market.condition_id, "UP");
    double down_accel = ticks_.acceleration_us(market.condition_id, "DOWN");

    auto build_signal = [&](ShareSide side, const std::string& side_name,
                            const std::string& token_id, double price,
                            double vel, double other_vel, double accel)
        -> std::optional<ScalpEntrySignal> {
        if (price < cfg.min_price || price > cfg.max_price) return std::nullopt;
        if (vel < cfg.velocity_thresh) return std::nullopt;
        if (accel < cfg.accel_min) return std::nullopt;
        const double dominance = vel - other_vel;
        if (dominance < cfg.dominance_thresh) return std::nullopt;

        const double vel_ratio = cfg.velocity_thresh > 0
            ? std::clamp(vel / cfg.velocity_thresh, 0.0, 4.0) : 0.0;
        const double dom_ratio = cfg.dominance_thresh > 0
            ? std::clamp(dominance / cfg.dominance_thresh, 0.0, 4.0) : 1.0;
        double conf = 0.44 + 0.12 * vel_ratio + 0.045 * dom_ratio;
        if (accel > 0) conf += std::min(accel * 65.0, 0.12);
        conf = std::clamp(conf, 0.0, 0.97);
        if (conf < cfg.min_confidence) return std::nullopt;

        return ScalpEntrySignal{
            market.condition_id,
            token_id,
            side,
            conf,
            cfg.per_entry_usd,
            price,
            fmt::format("SCALP-V3 {} vel={:.4f}/s dom={:.4f}/s accel={:.4f} price={:.3f} t={:.0f}s",
                        side_name, vel, dominance, accel, price, (double)secs_left)
        };
    };

    // Evaluate UP side
    auto up_signal = build_signal(ShareSide::UP, "UP", market.yes_token_id,
                                  up_mid, up_vel, down_vel, up_accel);

    // Evaluate DOWN side
    auto dn_signal = build_signal(ShareSide::DOWN, "DN", market.no_token_id,
                                  down_mid, down_vel, up_vel, down_accel);

    // If both sides qualify, prefer the one with higher acceleration
    // (momentum is building, not fading). If neither is accelerating,
    // prefer the one with higher velocity.
    if (up_signal && dn_signal) {
        if (up_accel > down_accel) return up_signal;
        if (down_accel > up_accel) return dn_signal;
        return (up_vel >= down_vel) ? up_signal : dn_signal;
    }

    if (up_signal) return up_signal;
    if (dn_signal) return dn_signal;

    return std::nullopt;
}

// ── Exits ─────────────────────────────────────────────────────────────

std::vector<ScalpExitSignal> ScalpManager::check_exits(
    const std::vector<Position>& open_positions,
    const std::function<double(const std::string&, ShareSide)>& get_bid,
    int64_t now_us) {

    std::vector<ScalpExitSignal> exits;
    if (!cfg.enabled) return exits;

    for (const auto& pos : open_positions) {
        double bid = get_bid(pos.condition_id, pos.side);
        if (bid <= 0) continue;

        // Get current velocity for this position's side (from TickBuffer)
        std::string side_str = (pos.side == ShareSide::UP) ? "UP" : "DOWN";
        double vel = ticks_.velocity_us(pos.condition_id, side_str, cfg.reversal_window_us);

        std::optional<ScalpExitSignal> exit;
        double entry = 0.0;
        double peak = 0.0;
        double stop = 0.0;
        double profit = 0.0;
        bool promoted_runner = false;
        bool skip_for_runner = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = positions_.find(mk_key(pos.condition_id, pos.side));
            if (it == positions_.end() || it->second.exited) continue;
            PositionState& ps = it->second;

            if (ps.entry_us > 0 && now_us - ps.entry_us < cfg.min_hold_us)
                continue;

            ps.peak_price = std::max(ps.peak_price, bid);
            entry = ps.entry_price;
            peak = ps.peak_price;
            profit = bid - ps.entry_price;

            const int64_t secs_left = (pos.window_close_ms > 0)
                ? (pos.window_close_ms * 1000LL - now_us) / 1'000'000LL
                : INT64_MAX;
            const bool runner_now =
                cfg.runner_enabled &&
                secs_left >= 0 &&
                secs_left <= cfg.runner_max_secs_left &&
                bid >= cfg.runner_min_bid &&
                profit >= cfg.runner_min_profit &&
                vel >= cfg.runner_velocity_floor;
            if (runner_now) {
                promoted_runner = !ps.runner;
                ps.runner = true;
                skip_for_runner = true;
            } else {
                ps.runner = false;
            }

            // 1) Momentum reversal. Both UP and DOWN are long tokens, so negative
            // velocity on the held token means its bid is losing value.
            // Profit-gated: a reversal exit only LOCKS IN gains — it never sells
            // at a loss (losses are the trailing stop / hard stop's job).
            if (!skip_for_runner && cfg.momentum_reversal_exit &&
                vel < cfg.reversal_vel_thresh &&
                profit >= cfg.reversal_min_profit) {
                exit = ScalpExitSignal{pos.condition_id, pos.side, "MOMENTUM_REVERSAL"};
            }

            // 2) Profit target. Percentage-based (profit_target_pct) fires when the
            // position gain % >= threshold; absolute (profit_target) fires on raw cents.
            // pct takes priority when set; either can trigger PROFIT_TAKEN.
            if (!exit && cfg.profit_take) {
                const bool pct_hit = cfg.profit_target_pct > 0 && entry > 0 &&
                                     (profit / entry * 100.0) >= cfg.profit_target_pct;
                const bool abs_hit = cfg.profit_target_pct <= 0 && profit >= cfg.profit_target;
                if (pct_hit || abs_hit)
                    exit = ScalpExitSignal{pos.condition_id, pos.side, "PROFIT_TAKEN"};
            }

            // 3) Trailing stop. Same math for UP and DOWN because both positions
            // are long shares: rising bid is good, falling bid is bad.
            if (!exit && cfg.trailing_stop) {
                update_trailing_stop(ps, bid);
                stop = ps.trail_stop;
                if (bid <= stop)
                    exit = ScalpExitSignal{pos.condition_id, pos.side, "TRAILING_STOP"};
            }
        }

        if (promoted_runner) {
            spdlog::info("scalp runner: {} {} entry={:.3f} bid={:.3f} profit={:.3f} vel={:.4f}/s",
                         pos.asset, side_str, entry, bid, profit, vel);
        }
        if (skip_for_runner) continue;
        if (exit) {
            exits.push_back(*exit);
            if (exit->reason == "TRAILING_STOP") {
                spdlog::info("scalp exit: {} {} TRAILING_STOP entry={:.3f} bid={:.3f} stop={:.3f} peak={:.3f}",
                             pos.asset, side_str, entry, bid, stop, peak);
            } else if (exit->reason == "PROFIT_TAKEN") {
                spdlog::info("scalp exit: {} {} PROFIT_TAKEN profit={:.3f} ({:.1f}%) entry={:.3f} bid={:.3f}",
                             pos.asset, side_str, profit,
                             entry > 0 ? profit / entry * 100.0 : 0.0, entry, bid);
            } else {
                spdlog::info("scalp exit: {} {} MOMENTUM_REVERSAL vel={:.4f}/s thresh={:.4f} entry={:.3f} bid={:.3f}",
                             pos.asset, side_str, vel, cfg.reversal_vel_thresh, entry, bid);
            }
        }
    }

    return exits;
}

// ── Trailing stop update ─────────────────────────────────────────────

void ScalpManager::update_trailing_stop(PositionState& ps, double current_bid) {
    // UP and DOWN positions are both long tokens. The stop ratchets up as the
    // held token's own bid rises, then exits when that bid falls back through it.
    (void)current_bid;
    const double target_stop = ps.peak_price - cfg.trail_start_loss;
    const double max_allowed = ps.peak_price - cfg.trail_min_distance;
    const double ratchet = std::clamp(cfg.trail_ratchet_pct, 0.0, 1.0);

    if (ps.trail_stop <= 0) {
        ps.trail_stop = target_stop;
    } else if (target_stop > ps.trail_stop) {
        ps.trail_stop += (target_stop - ps.trail_stop) * ratchet;
    }

    ps.trail_stop = std::min(ps.trail_stop, max_allowed);
}

// ── Lifecycle ─────────────────────────────────────────────────────────

void ScalpManager::on_entry(const std::string& cond_id, ShareSide side,
                            double entry_price, int64_t now_us) {
    std::lock_guard<std::mutex> lk(mu_);
    auto& ps = positions_[mk_key(cond_id, side)];
    ps.entry_price = entry_price;
    ps.peak_price  = entry_price;
    ps.trail_stop = (cfg.trail_stop_pct > 0.0 && entry_price > 0.0)
        ? entry_price * (1.0 - cfg.trail_stop_pct / 100.0)
        : entry_price - cfg.trail_start_loss;
    ps.entry_us    = now_us;
    ps.exit_us     = 0;
    ps.exited      = false;
    ps.runner      = false;
    spdlog::info("scalp track: {} entry={:.3f} stop={:.3f} ({})",
                 side == ShareSide::UP ? "UP" : "DN", entry_price, ps.trail_stop,
                 cfg.trail_stop_pct > 0.0 ? fmt::format("{:.0f}% floor", cfg.trail_stop_pct)
                                           : fmt::format("abs -{:.3f}", cfg.trail_start_loss));
}

void ScalpManager::on_exit(const std::string& cond_id, ShareSide side, int64_t now_us) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = positions_.find(mk_key(cond_id, side));
    if (it != positions_.end()) {
        it->second.exited = true;
        it->second.exit_us = now_us;
        it->second.runner = false;
    }
}

bool ScalpManager::is_tracking(const std::string& cond_id, ShareSide side) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = positions_.find(mk_key(cond_id, side));
    return it != positions_.end() && !it->second.exited;
}

} // namespace vp::engine
