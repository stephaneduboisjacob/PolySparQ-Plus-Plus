#pragma once
// ScalpManager — consolidated scalping state machine for sub-second
// momentum trading.
//
// Produces both entry and exit signals. Owns per-position trailing stop
// state, peak price tracking, and momentum reversal detection.
//
// Entry logic: compute velocity for both UP/DOWN sides from TickBuffer;
// if any side's velocity exceeds threshold and price is in range, emit
// an entry signal for that side. Prefer accelerating velocity.
//
// Exit logic (checked every 200ms, three triggers, first wins):
//   1. Momentum reversal — velocity dropped below threshold opposite entry
//   2. Trailing stop     — bid hit the trailing stop level
//   3. Profit target     — bid passed profit target above entry
//
// All parameters are hot-reloadable via ScalpManager::Config.

#include "core/types.hpp"  // ShareSide
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace vp {
struct PmMarket;   // forward decl from feeds/polymarket_rest.hpp
struct Position;   // forward decl from exec/positions.hpp
}

namespace vp::engine {

class TickBuffer;

struct ScalpEntrySignal {
    std::string condition_id;
    std::string token_id;
    ShareSide   side;
    double      confidence;       // 0..1
    double      rec_size_usd;
    double      rec_limit_price;
    std::string reason;
};

struct ScalpExitSignal {
    std::string condition_id;
    ShareSide   side;
    std::string reason;   // "MOMENTUM_REVERSAL", "TRAILING_STOP", "PROFIT_TAKEN"
};

class ScalpManager {
public:
    explicit ScalpManager(TickBuffer& tick_buf);

    // ── Hot-reloadable configuration ──────────────────────────────
    struct Config {
        // Entry
        bool    enabled                    = false;
        double  velocity_thresh            = 0.0003;  // $/s (was 0.0005 — lower = more entries)
        double  accel_min                  = -1.0;    // $/s/s floor; v2 keeps this permissive
        double  dominance_thresh           = 0.0;     // chosen side velocity must beat opposite by this
        double  min_confidence             = 0.0;
        double  min_price                  = 0.08;
        double  max_price                  = 0.75;
        int     min_secs                   = 15;
        int     max_secs                   = 3600;    // all timeframes
        double  per_entry_usd              = 3.00;
        int     max_entries_per_window     = 5;
        int64_t cooldown_us                = 0;

        // Exit — Momentum Reversal — RE-ENABLED 2026-05-31.
        // Entry velocity threshold is now 0.0015 (vs 0.0005 before) so only real
        // momentum triggers entries. A reversal signal is now meaningful. Gated by
        // cfg.momentum_reversal_exit (hot-reloadable) in case spread noise picks up again.
        bool    momentum_reversal_exit     = false;
        double  reversal_vel_thresh        = -0.0010;  // $/s (tightened to align with entry thresh 0.0015)
        int     reversal_window_us         = 1000000;  // 1s lookback (tightened from 1.5s)
        double  reversal_min_profit        = 0.0;      // reversal only LOCKS profit ≥ this; losses go to trail/hard stop
        int64_t min_hold_us                = 0;        // main.cpp syncs this from TOML before use

        // Exit — Trailing Stop
        bool    trailing_stop              = false;
        double  trail_stop_pct             = 0.0;      // % loss from entry for initial stop (0 = use absolute trail_start_loss)
        double  trail_start_loss           = 0.015;    // initial stop below entry; also ratchet distance from peak
        double  trail_ratchet_pct          = 0.50;     // ratchet aggressiveness
        double  trail_min_distance         = 0.008;    // min distance from peak (was 0.01)

        // Exit — Tight Profit Target
        bool    profit_take                = false;
        double  profit_target              = 0.02;     // absolute cents above entry; 0 = disabled
        double  profit_target_pct          = 0.0;      // % gain on entry; overrides absolute when > 0

        // Runner promotion. A strong late-window scalp stops using tactical
        // exits and is allowed to settle, unless velocity drops under the floor.
        bool    runner_enabled             = false;
        double  runner_min_bid             = 0.82;
        double  runner_min_profit          = 0.12;
        int     runner_max_secs_left       = 75;
        double  runner_velocity_floor      = -0.0003;
    };
    Config cfg;

    // ── Entry ─────────────────────────────────────────────────────

    // Evaluate one market for a scalp entry signal.
    // velocities come from TickBuffer (1s window), accel from 2s window.
    std::optional<ScalpEntrySignal> check_entry(
        const PmMarket& market,
        double up_mid, double down_mid,
        double up_vel, double down_vel,
        int64_t now_us, int secs_left, int entries_so_far);

    // ── Exits ─────────────────────────────────────────────────────

    // Check all open positions for scalp exit signals.
    // get_bid(cond_id, side) returns the current best bid for that side.
    // Returns exits ordered by urgency (reversal first, then trailing, then profit).
    std::vector<ScalpExitSignal> check_exits(
        const std::vector<Position>& open_positions,
        const std::function<double(const std::string&, ShareSide)>& get_bid,
        int64_t now_us);

    // ── Lifecycle ─────────────────────────────────────────────────

    // Call when an entry signal is executed to start position tracking.
    void on_entry(const std::string& cond_id, ShareSide side,
                  double entry_price, int64_t now_us);

    // Call when an exit is executed to stop tracking a position.
    void on_exit(const std::string& cond_id, ShareSide side, int64_t now_us = 0);

    // True if the ScalpManager is currently tracking this position.
    bool is_tracking(const std::string& cond_id, ShareSide side) const;

private:
    struct PositionState {
        double  entry_price;
        double  peak_price;       // best price since entry
        double  trail_stop;       // current stop level (ratchets up)
        int64_t entry_us;
        int64_t exit_us = 0;
        bool    exited = false;
        bool    runner = false;
    };

    TickBuffer& ticks_;
    mutable std::mutex mu_;
    // Key format: cond_id + "|U" or cond_id + "|D"
    std::unordered_map<std::string, PositionState> positions_;

    static std::string mk_key(const std::string& cid, ShareSide s) {
        return cid + (s == ShareSide::UP ? "|U" : "|D");
    }

    void update_trailing_stop(PositionState& ps, double current_bid);
};

} // namespace vp::engine
