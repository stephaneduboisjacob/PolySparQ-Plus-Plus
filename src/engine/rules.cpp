#include "rules.hpp"
#include "tui/tui.hpp"  // WatchlistRow
#include "engine/predictor.hpp"
#include <cmath>
#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <string>
#include <unordered_map>

namespace vp {
namespace rules {

// -------------------------------------------------------------------
// Evaluate one market row against the active strategy config.
//
// Design (after cleanup):
//   ONE active strategy: ORACLE-LAG ENDGAME SNIPER
//   Evaluated in priority order within the combined oracle-lag + endgame block.
//
//   Other strategies (momentum scalp, predictor edge, legacy scalp_v2)
//   are disabled via config + #if 0 in this file. Do not re-enable
//   without explicit review — they compete for the same entry slots.
// -------------------------------------------------------------------
std::optional<SignalOut> evaluate(const WatchlistRow& row,
                                  const StrategyHot&  cfg,
                                  int64_t             now_ms,
                                  EvaluationDiagnostics* diagnostics) {
    if (diagnostics) *diagnostics = {};
    auto reject = [&](const char* code, std::string detail = {}) -> std::optional<SignalOut> {
        if (diagnostics) *diagnostics = {code, std::move(detail)};
        return std::nullopt;
    };
    int64_t secs_left = (row.window_close_ms - now_ms) / 1000;

    // Near-zone trace (once per market+window, BEFORE any early return): the
    // real runtime secs_left / endgame_secs_left / endgame_mode plus the
    // forecast status at the moment the window approaches the endgame zone.
    if (secs_left > 0 && secs_left <= 140) {
        static std::unordered_map<std::string, std::pair<int64_t, int>> nz;
        const std::string nk = fmt::format("{}|{}", row.condition_id, row.window_close_ms);
        auto& e = nz[nk];
        if (e.first != row.window_close_ms) e = {row.window_close_ms, 0};
        if (e.second == 0) {
            e.second = 1;
            spdlog::info(
                "ENDGAME near-zone t={:.0f}s cap={} mode={} fc_ready={} fc_status={} "
                "oracle={:+.3f}% up_ask={:.3f} dn_ask={:.3f} -> {}",
                (double)secs_left, cfg.endgame_secs_left,
                cfg.endgame_mode ? 1 : 0,
                row.reference_forecast.ready ? 1 : 0,
                row.reference_forecast.status,
                row.oracle_delta_pct, row.up_ask, row.down_ask,
                (cfg.endgame_mode && secs_left <= cfg.endgame_secs_left) ? "ENTER" : "outside");
        }
    }

    if (!row.reference_forecast.ready) {
        // First not-ready reason per (market, window) — the forecast kernel's
        // status string is the answer to "why does this bot never trade?".
        static std::unordered_map<std::string, std::pair<int64_t, int>> nf_seen;
        const std::string nf_key =
            fmt::format("{}|{}", row.condition_id, row.window_close_ms);
        auto& e = nf_seen[nf_key];
        if (e.first != row.window_close_ms) e = {row.window_close_ms, 0};
        if (e.second == 0) {
            e.second = 1;
            spdlog::info("gate {}: forecast not ready ({}) t={:.0f}s",
                         row.condition_id.substr(0, 10),
                         row.reference_forecast.status, (double)secs_left);
        }
        return reject("forecast", row.reference_forecast.status);
    }
    if (secs_left <= 0) return reject("closed");

    // Need executable book data for any entry path.
    if (row.up_ask <= 0 && row.down_ask <= 0) {
        static std::unordered_map<std::string, std::pair<int64_t, int>> nb_seen;
        const std::string nb_key =
            fmt::format("{}|{}", row.condition_id, row.window_close_ms);
        auto& e = nb_seen[nb_key];
        if (e.first != row.window_close_ms) e = {row.window_close_ms, 0};
        if (e.second == 0) {
            e.second = 1;
            spdlog::info("gate {}: no executable book (up_ask {:.3f} down_ask {:.3f})",
                         row.condition_id.substr(0, 10), row.up_ask, row.down_ask);
        }
        return reject("no_book");
    }

    // Asset blocklist — skip entries for assets that the settlement backtest
    // shows as net losers (e.g. SOL). Exits/settlement of existing positions
    // are unaffected; this only gates NEW entries.
    for (const auto& blocked : cfg.blocked_assets)
        if (row.asset == blocked) return reject("blocked_asset");

    // ---------------------------------------------------------------
    // 1) ENDGAME SNIPE
    //    Last `endgame_secs_left` seconds of the window. Share price
    //    is nearly the true probability. If a side trades at e.g. 0.85
    //    and oracle confirms direction, evaluate the remaining payout risk.
    // ---------------------------------------------------------------
    // Endgame zone: only endgame may fire; predictor/oracle-lag don't enter last seconds.
    if (secs_left > 0 && secs_left <= cfg.endgame_secs_left) {
        // Zone diagnostic (once per market+window), deliberately OUTSIDE the
        // mode gate: logs the real runtime values plus the enter/skip verdict,
        // so a mode/config mismatch can't hide behind silent skips.
        {
            static std::unordered_map<std::string, std::pair<int64_t, int>> zseen;
            const std::string zk =
                fmt::format("{}|{}", row.condition_id, row.window_close_ms);
            auto& z = zseen[zk];
            if (z.first != row.window_close_ms) z = {row.window_close_ms, 0};
            if (z.second == 0) {
                z.second = 1;
                spdlog::info(
                    "ENDGAME zone t={:.0f}s mode={} thresh={:.3f}% band=[{:.2f},{:.2f}] "
                    "oracle={:+.3f}% cb={:+.3f}% up_ask={:.3f} dn_ask={:.3f} -> {}",
                    (double)secs_left, cfg.endgame_mode ? 1 : 0,
                    cfg.endgame_min_oracle_pct, cfg.endgame_min_price,
                    cfg.endgame_max_price, row.oracle_delta_pct,
                    row.cb_delta_pct, row.up_ask, row.down_ask,
                    cfg.endgame_mode ? "ENTER" : "mode-off");
            }
        }
        if (cfg.endgame_mode) {
            // ENDGAME veto logging: the entry path used to reject silently,
            // making "why is this bot not trading?" unanswerable from logs.
            // Log the FIRST rejection reason per (market, window) only.
            const std::string eg_key =
                fmt::format("{}|{}", row.condition_id, row.window_close_ms);
            auto veto = [&](const std::string& reason) {
                static std::unordered_map<std::string, std::pair<int64_t, std::string>> seen;
                auto& e = seen[eg_key];
                if (e.first != row.window_close_ms) e = {row.window_close_ms, ""};
                if (e.second.empty()) {
                    e.second = reason;
                    spdlog::info("ENDGAME veto t={:.0f}s: {}", (double)secs_left, reason);
                }
                return reject("endgame", reason);
            };

            const double oracle_thresh = cfg.endgame_min_oracle_pct;
            bool eg_oracle_up   = row.oracle_delta_pct >= +oracle_thresh;
            bool eg_oracle_down = row.oracle_delta_pct <= -oracle_thresh;

            auto try_side = [&](ShareSide side, double ask, bool oracle_agrees)
                -> std::optional<SignalOut> {
                const char* sname = side == ShareSide::UP ? "UP" : "DN";
                if (ask <= 0) return veto(fmt::format("{} no ask", sname));
                if (ask < cfg.endgame_min_price)
                    return veto(fmt::format("{} ask {:.3f} < band min {:.2f} (market not confident)",
                                            sname, ask, cfg.endgame_min_price));
                if (ask > cfg.endgame_max_price)
                    return veto(fmt::format("{} ask {:.3f} > band max {:.2f}",
                                            sname, ask, cfg.endgame_max_price));
                if (ask > cfg.hard_share_price_ceil)
                    return veto(fmt::format("{} ask {:.3f} > hard ceil {:.2f}",
                                            sname, ask, cfg.hard_share_price_ceil));
                if (!oracle_agrees)
                    return veto(fmt::format("{} oracle {:+.3f}% vs floor {:.3f}% (no directional move)",
                                            sname, row.oracle_delta_pct, oracle_thresh));

                const double direction = side == ShareSide::UP ? 1.0 : -1.0;
                const double cb_thr = cfg.coinbase_lead_min_delta_pct > 0
                    ? cfg.coinbase_lead_min_delta_pct : oracle_thresh;

                // The late favorite is only useful when the faster feeds have
                // not reversed. Honor the same CB/Binance protections as the
                // oracle-lag path instead of letting ENDGAME bypass them.
                if (cfg.coinbase_veto_mode) {
                    if (!row.cb_data_ready)
                        return veto(fmt::format("{} cb feed not ready", sname));
                    if (row.cb_delta_pct * direction < 0.0)
                        return veto(fmt::format("{} cb reversed ({:+.3f}%)", sname, row.cb_delta_pct));
                    if (std::abs(row.cb_delta_pct) < cb_thr)
                        return veto(fmt::format("{} cb flat ({:+.3f}% < {:.3f}%)", sname, row.cb_delta_pct, cb_thr));
                }
                if (cfg.binance_book_confirm) {
                    const bool has_bt = row.bt_mid_delta_pct != 0.0 ||
                                        row.bt_imbalance != 0.0;
                    if (has_bt) {
                        const bool mid_ok = row.bt_mid_delta_pct * direction > 0.0;
                        const bool imb_ok = row.bt_imbalance * direction >=
                                            cfg.binance_book_imb_min;
                        if (!mid_ok && !imb_ok)
                            return veto(fmt::format("{} binance disagrees (mid {:+.3f}% imb {:+.2f})",
                                                    sname, row.bt_mid_delta_pct, row.bt_imbalance));
                    }
                }
                if (cfg.multi_source_vote) {
                    int voters = 0, agrees = 0;
                    if (std::abs(row.cb_delta_pct) >= cb_thr) {
                        ++voters;
                        if (row.cb_delta_pct * direction > 0.0) ++agrees;
                    }
                    if (row.bt_mid_delta_pct != 0.0 || row.bt_imbalance != 0.0) {
                        ++voters;
                        const bool bt_agrees = row.bt_mid_delta_pct * direction > 0.0 ||
                            row.bt_imbalance * direction >= cfg.binance_book_imb_min;
                        if (bt_agrees) ++agrees;
                    }
                    if (row.liq_pressure_usd != 0.0) {
                        ++voters;
                        if (row.liq_pressure_usd * direction > 0.0) ++agrees;
                    }
                    // For an end-game snipe, missing confirmations are a veto,
                    // not a pass-through: one tail loss erases many small wins.
                    if (voters < cfg.multi_source_quorum ||
                        agrees < cfg.multi_source_quorum)
                        return veto(fmt::format("{} quorum {}/{} votes, {}/{} agree",
                                                sname, voters, cfg.multi_source_quorum,
                                                agrees, cfg.multi_source_quorum));
                }

                double pe = 1.0, qe = 1.0;
                for (int i = 0; i < cfg.fee_exponent; ++i) {
                    pe *= ask;
                    qe *= (1.0 - ask);
                }
                const double fee_per_share = cfg.taker_fee_rate * pe * qe;
                const double net_profit_pct = 100.0 *
                    (1.0 - ask - fee_per_share) / (ask + fee_per_share);
                if (net_profit_pct < cfg.endgame_min_profit_pct)
                    return veto(fmt::format("{} net return {:.2f}% < {:.2f}% (fees not covered)",
                                            sname, net_profit_pct, cfg.endgame_min_profit_pct));
                const auto& f = row.reference_forecast;
                const auto edge = vp::engine::execution_edge(
                    side==ShareSide::UP ? f.probability : 1-f.probability,
                    side==ShareSide::UP ? f.low : 1-f.high,
                    side==ShareSide::UP ? row.up_bid : row.down_bid,ask,
                    cfg.taker_fee_rate,cfg.fee_exponent);
                if (!edge.executable || edge.conservative < cfg.min_predictor_edge)
                    return veto(fmt::format("{} stressed edge {:.3f} < {:.3f}",
                                            sname, edge.conservative, cfg.min_predictor_edge));
                SignalOut s;
                s.side            = side;
                s.dir             = OrderDir::Buy;
                double profit_score = std::min(net_profit_pct / 20.0, 1.0);
                s.confidence      = side==ShareSide::UP ? f.probability : 1-f.probability;
                s.rec_limit_price = ask;
                s.rec_size_usd    = cfg.per_click_size_usd;
                s.glyph           = (side == ShareSide::UP) ? '+' : 'v';
                s.reason = fmt::format(
                    "ENDGAME {} ask={:.3f} net=+{:.1f}% t={:.0f}s "
                    "oracle={:+.3f}% cb={:+.3f}% bt={:+.3f}%",
                    side == ShareSide::UP ? "UP" : "DN",
                    ask, net_profit_pct, (double)secs_left,
                    row.oracle_delta_pct, row.cb_delta_pct,
                    row.bt_mid_delta_pct);
                return s;
            };

            // Preserve the relevant side's veto (previously every DOWN market
            // logged the losing UP side's low ask as its first rejection).
            if (row.oracle_delta_pct < 0) {
                auto s = try_side(ShareSide::DOWN, row.down_ask, eg_oracle_down);
                if (s || !eg_oracle_up) return s;
                return try_side(ShareSide::UP, row.up_ask, eg_oracle_up);
            }
            auto s = try_side(ShareSide::UP, row.up_ask, eg_oracle_up);
            if (s || !eg_oracle_down) return s;
            return try_side(ShareSide::DOWN, row.down_ask, eg_oracle_down);
        }
        return reject("endgame_disabled");  // endgame zone: no other entries
    }

    // All regular entries honor the configured time band, including latency.
    {
        int min_secs = cfg.min_secs_remaining_5m;
        int max_secs = cfg.max_secs_remaining_5m;
        if (row.horizon_s >= 3600) { min_secs=cfg.min_secs_remaining_60m; max_secs=cfg.max_secs_remaining_60m; }
        else if (row.horizon_s >= 900) { min_secs=cfg.min_secs_remaining_15m; max_secs=cfg.max_secs_remaining_15m; }
        if (secs_left < min_secs || secs_left > max_secs)
            return reject("entry_time", fmt::format("allowed=[{},{}]s", min_secs, max_secs));
    }

    // Latency fair value uses the same reference-aware model as the board.
    // A capped price is not itself evidence of an edge. Require actual fees,
    // slippage and stressed volatility to leave a positive margin.
    if (cfg.digital_fv_mode) {
        const auto& f = cfg.digital_fv_use_coinbase ? row.fast_forecast : row.reference_forecast;
        if (!f.ready || (cfg.digital_fv_use_coinbase && !row.cb_data_ready))
            return reject("digital_reference", f.status);
        if (cfg.digital_fv_use_coinbase &&
            std::abs(row.cb_delta_pct) < cfg.coinbase_lead_min_delta_pct)
            return reject("digital_cb_delta", fmt::format("cb={:+.4f}% need={:.4f}%",
                row.cb_delta_pct, cfg.coinbase_lead_min_delta_pct));
        // Jump/regime guards — the knobs existed in config but were never
        // consulted until Sep 06. A z-score jump in the oracle reference or
        // realized vol outside the sanity band breaks the forecast kernel's
        // diffusion assumption; stand down for this sweep. Missing vol data
        // (0/non-finite) does not veto.
        if (cfg.digital_fv_jump_z > 0 && std::isfinite(row.oracle_jump_z) &&
            std::abs(row.oracle_jump_z) >= cfg.digital_fv_jump_z)
            return reject("digital_jump", fmt::format("z={:.3f}", row.oracle_jump_z));
        if (std::isfinite(row.oracle_vol_per_sec) && row.oracle_vol_per_sec > 0) {
            if (cfg.digital_fv_vol_floor > 0 &&
                row.oracle_vol_per_sec < cfg.digital_fv_vol_floor)
                return reject("digital_vol_floor", fmt::format("sigma={:.8f} floor={:.8f}",
                    row.oracle_vol_per_sec, cfg.digital_fv_vol_floor));
            if (cfg.digital_fv_vol_ceil > 0 &&
                row.oracle_vol_per_sec > cfg.digital_fv_vol_ceil)
                return reject("digital_vol_ceil", fmt::format("sigma={:.8f} ceil={:.8f}",
                    row.oracle_vol_per_sec, cfg.digital_fv_vol_ceil));
        }
        const auto up = vp::engine::execution_edge(f.probability,f.low,row.up_bid,row.up_ask,
            cfg.taker_fee_rate,cfg.fee_exponent);
        const auto down = vp::engine::execution_edge(1-f.probability,1-f.high,row.down_bid,row.down_ask,
            cfg.taker_fee_rate,cfg.fee_exponent);
        auto try_side = [&](ShareSide side, const vp::engine::ExecutionEdge& edge, double ask)
            -> std::optional<SignalOut> {
            const double direction = side == ShareSide::UP ? 1 : -1;
            if (!edge.executable || ask < cfg.min_entry_share_price ||
                ask > std::min(cfg.hard_share_price_ceil,cfg.max_entry_share_price))
                return reject("digital_price_band", fmt::format("{} ask={:.3f} band=[{:.3f},{:.3f}] executable={}",
                    side==ShareSide::UP ? "UP" : "DOWN", ask, cfg.min_entry_share_price,
                    std::min(cfg.hard_share_price_ceil,cfg.max_entry_share_price), edge.executable));
            if (cfg.coinbase_veto_mode && (!row.cb_data_ready || row.cb_delta_pct*direction <= 0))
                return reject("digital_cb_veto", fmt::format("cb={:+.4f}%", row.cb_delta_pct));
            const double reserve = (cfg.digital_fv_fee_buffer +
                (side == ShareSide::DOWN ? cfg.digital_fv_down_slip_pct : 0))/100.0;
            if (edge.net < cfg.digital_fv_min_edge_pct/100.0+reserve ||
                edge.conservative < cfg.min_predictor_edge)
                return reject("digital_edge", fmt::format("{} net={:+.4f} need={:.4f} stress={:+.4f} need={:.4f}",
                    side==ShareSide::UP ? "UP" : "DOWN", edge.net,
                    cfg.digital_fv_min_edge_pct/100.0+reserve, edge.conservative, cfg.min_predictor_edge));
            SignalOut sig;
            sig.side=side; sig.dir=OrderDir::Buy;
            sig.confidence=side==ShareSide::UP ? f.probability : 1-f.probability;
            sig.rec_limit_price=ask; sig.rec_size_usd=cfg.per_click_size_usd;
            sig.glyph=side==ShareSide::UP ? '+' : 'v';
            sig.reason=fmt::format("DIGITALFV{} {} fair={:.3f} ask={:.3f} net={:+.3f} stress={:+.3f} t={}s",
                cfg.digital_fv_use_coinbase ? "-CB" : "", side==ShareSide::UP ? "UP" : "DN",
                sig.confidence,ask,edge.net,edge.conservative,secs_left);
            return sig;
        };
        if (down.conservative > up.conservative) {
            if (auto sig=try_side(ShareSide::DOWN,down,row.down_ask)) return sig;
            const auto best_veto = diagnostics ? *diagnostics : EvaluationDiagnostics{};
            if (auto sig=try_side(ShareSide::UP,up,row.up_ask)) return sig;
            if (diagnostics) *diagnostics = best_veto;
            return std::nullopt;
        }
        if (auto sig=try_side(ShareSide::UP,up,row.up_ask)) return sig;
        const auto best_veto = diagnostics ? *diagnostics : EvaluationDiagnostics{};
        if (auto sig=try_side(ShareSide::DOWN,down,row.down_ask)) return sig;
        if (diagnostics) *diagnostics = best_veto;
        return std::nullopt;
    }

    // ---------------------------------------------------------------
#if 0
    // 2) MOMENTUM SCALP — buy the side that's moving in our favour.
    //
    //    UP velocity > threshold -> UP shares rising -> buy UP
    //    DOWN velocity > threshold -> DOWN shares rising -> buy DOWN
    //
    //    Price must be in a sensible range (avoid noise < 15c, avoid
    //    expiry near $1). Works on all window sizes (5m/15m/60m).
    // ---------------------------------------------------------------
    if (cfg.momentum_scalp_enabled) {
        int64_t min_secs = cfg.momentum_scalp_min_secs;
        int64_t max_secs = cfg.momentum_scalp_max_secs;

        if (secs_left >= min_secs && secs_left <= max_secs) {
            // Compute confidence from velocity strength.
            // Goal: fire on real moves, not micro-ticks.
            auto build_signal = [&](ShareSide side, double price, double vel,
                                    double max_vel) -> std::optional<SignalOut> {
                if (price <= 0) return std::nullopt;
                if (price < cfg.momentum_scalp_min_price) return std::nullopt;
                if (price > cfg.momentum_scalp_max_price) return std::nullopt;
                // Velocity must exceed threshold
                if (std::abs(vel) < cfg.momentum_scalp_velocity_thresh) return std::nullopt;
                // Confidence: 0.50 at threshold, 0.95 at 5x threshold
                double vel_ratio = std::abs(vel) / cfg.momentum_scalp_velocity_thresh;
                double conf = std::clamp(0.50 + 0.09 * vel_ratio, 0.0, 0.95);
                // Price discount bonus: cheaper shares = more runway
                double price_discount = 1.0 - price;
                conf += 0.05 * price_discount;  // +5c max at $0 -> +~2.5c at $0.50
                conf = std::clamp(conf, 0.0, 0.95);

                SignalOut s;
                s.side = side;
                s.dir = OrderDir::Buy;
                s.confidence = conf;
                s.rec_limit_price = price;
                s.rec_size_usd = cfg.per_click_size_usd;
                s.glyph = (side == ShareSide::UP) ? '+' : 'v';
                s.reason = fmt::format(
                    "MOMENTUM {} vel={:.4f}/s price={:.3f} t={:.0f}s",
                    side == ShareSide::UP ? "UP" : "DN",
                    vel, price, (double)secs_left);
                return s;
            };

            // UP side: buy when UP price is rising
            if (row.up_velocity >= cfg.momentum_scalp_velocity_thresh) {
                if (auto s = build_signal(ShareSide::UP, row.up_price,
                                          row.up_velocity,
                                          cfg.momentum_scalp_velocity_thresh * 5))
                    return s;
            }
            // DOWN side: buy when DOWN price is rising
            if (row.down_velocity >= cfg.momentum_scalp_velocity_thresh) {
                if (auto s = build_signal(ShareSide::DOWN, row.down_price,
                                          row.down_velocity,
                                          cfg.momentum_scalp_velocity_thresh * 5))
                    return s;
            }
        }
    }
#endif

    // ---------------------------------------------------------------
#if 1   // P1: predictor edge entries ENABLED (was #if 0 — output was wasted)
    // 3) PREDICTOR EDGE — ensemble model (Black-Scholes + technicals +
    //    microstructure + tick momentum) predicts a meaningful edge
    //    vs the current market mid-price. Buy the undervalued side.
    //    This is the bridge between the expensive predictor computation
    //    and actual trade execution — previously the predictor ran but
    //    its output was never used for signal generation.
    // ---------------------------------------------------------------
    if (cfg.predictor_mode) {
        bool oracle_flat = std::abs(row.oracle_delta_pct) < cfg.min_oracle_delta_pct;
        double conf_req = oracle_flat
            ? cfg.predictor_no_oracle_conf
            : cfg.min_predictor_conf;

        auto try_predictor = [&](ShareSide side, double edge, double price)
            -> std::optional<SignalOut> {
            if (edge < cfg.min_predictor_edge) return std::nullopt;
            if (price <= 0 || price > cfg.predictor_max_token_price) return std::nullopt;
            if (price < cfg.min_entry_share_price) return std::nullopt;  // P3 floor
            if (price > cfg.hard_share_price_ceil) return std::nullopt;
            if (row.pred_conf < conf_req) return std::nullopt;

            SignalOut s;
            s.side            = side;
            s.dir             = OrderDir::Buy;
            s.confidence      = std::clamp(row.pred_conf, 0.0, 0.95);
            s.rec_limit_price = price;
            s.rec_size_usd    = cfg.per_click_size_usd;
            s.glyph           = (side == ShareSide::UP) ? '+' : 'v';
            s.reason = fmt::format(
                "PREDICTOR {} edge={:+.3f} conf={:.0f}% fv={:.2f} t={:.0f}s",
                side == ShareSide::UP ? "UP" : "DN",
                edge, row.pred_conf * 100, row.pred_fair_up, (double)secs_left);
            return s;
        };

        if (auto s = try_predictor(ShareSide::UP, row.pred_edge_up, row.up_ask))
            return s;
        if (auto s = try_predictor(ShareSide::DOWN, row.pred_edge_down, row.down_ask))
            return s;
    }
#endif

    // ---------------------------------------------------------------
    // 3.5) ADAPTIVE CONFIDENCE-TIMED ENTRY
    //
    //    The user specifies a target confidence (e.g. 90%). The bot tracks
    //    the predictor's confidence trajectory and enters when:
    //      - confidence >= target_conf
    //      - confidence slope > threshold (rising)
    //      - confidence has risen enough from trajectory low
    //      - enough samples collected
    //      - predictor agrees with oracle direction (if enabled)
    //    This lets the user say "buy when 90% confident" and the bot figures
    //    out the optimal time in the window.
    // ---------------------------------------------------------------
    if (cfg.adaptive_mode && row.pred_conf_trajectory_ready) {
        bool want_up = row.pred_up >= 0.5;
        double price = want_up ? row.up_ask : row.down_ask;
        if (price <= 0 || price > cfg.predictor_max_token_price) {
            // Fall through to oracle-lag if adaptive price band not met
        } else if (price >= cfg.min_entry_share_price && price <= cfg.hard_share_price_ceil) {
            // Oracle agreement gate
            if (cfg.adaptive_require_predictor_agree) {
                double m = cfg.adaptive_oracle_agree_margin;
                bool pred_agrees = want_up ? (row.pred_up >= 0.5 + m)
                                           : (row.pred_up <= 0.5 - m);
                if (!pred_agrees) {
                    // Predictor disagrees with oracle; fall through
                } else {
                    SignalOut s;
                    s.side            = want_up ? ShareSide::UP : ShareSide::DOWN;
                    s.dir             = OrderDir::Buy;
                    s.confidence      = std::clamp(row.pred_conf, 0.0, 0.95);
                    s.rec_limit_price = price;
                    s.rec_size_usd    = cfg.per_click_size_usd;
                    s.glyph           = (want_up ? '+' : 'v');
                    s.reason = fmt::format(
                        "ADAPTIVE {} pred={:.2f} conf={:.0f}% slope={:+.3f}/s "
                        "countdown={}s ask={:.3f} t={:.0f}s",
                        want_up ? "UP" : "DN",
                        row.pred_up, row.pred_conf * 100,
                        row.pred_conf_trajectory_slope,
                        row.pred_adaptive_countdown_s >= 0
                            ? row.pred_adaptive_countdown_s : -1,
                        price, (double)secs_left);
                    return s;
                }
            } else {
                SignalOut s;
                s.side            = want_up ? ShareSide::UP : ShareSide::DOWN;
                s.dir             = OrderDir::Buy;
                s.confidence      = std::clamp(row.pred_conf, 0.0, 0.95);
                s.rec_limit_price = price;
                s.rec_size_usd    = cfg.per_click_size_usd;
                s.glyph           = (want_up ? '+' : 'v');
                s.reason = fmt::format(
                    "ADAPTIVE {} pred={:.2f} conf={:.0f}% slope={:+.3f}/s "
                    "countdown={}s ask={:.3f} t={:.0f}s",
                    want_up ? "UP" : "DN",
                    row.pred_up, row.pred_conf * 100,
                    row.pred_conf_trajectory_slope,
                    row.pred_adaptive_countdown_s >= 0
                        ? row.pred_adaptive_countdown_s : -1,
                    price, (double)secs_left);
                return s;
            }
        }
    }

    // ---------------------------------------------------------------
    // 4) ORACLE LAG — front-run the Chainlink oracle repricing.
    //
    //    Oracle delta is the % the price has moved since window open.
    //    The CLOB lags by ~55s. Buy the side that hasn't repriced yet.
    //
    //    delta > 0 -> oracle went up -> buy UP (still cheap)
    //    delta < 0 -> oracle went down -> buy DOWN (still cheap)
    // ---------------------------------------------------------------
    if (cfg.oracle_lag_mode) {
        double delta = row.oracle_delta_pct;
        bool   cb_led = false;

        // ── Coinbase lead (2026-06-10): Coinbase moves ~1.5-3s BEFORE the
        // oracle prints the same move (Chainlink aggregates USD venues,
        // Coinbase dominant). If Coinbase has crossed the threshold and the
        // oracle does not contradict it, adopt the Coinbase delta NOW — we
        // trade the oracle's next print before oracle-watchers and MMs react.
        const double cb_thr = oracle_trigger_threshold(cfg, true);
        if (cfg.coinbase_lead_mode && row.cb_data_ready &&
            std::abs(delta) < cfg.min_oracle_delta_pct &&
            std::abs(row.cb_delta_pct) >= cb_thr &&
            row.cb_delta_pct * delta >= 0.0) {   // oracle agrees or is flat
            delta = row.cb_delta_pct;
            cb_led = true;
        }

        const double trigger_threshold = oracle_trigger_threshold(cfg, cb_led);
        if (std::abs(delta) < trigger_threshold)
            return reject("oracle_delta", fmt::format("oracle={:+.4f}% cb={:+.4f}% need={:.4f}% cb_ready={}",
                row.oracle_delta_pct, row.cb_delta_pct, trigger_threshold, row.cb_data_ready));

        // ── Binance bookTicker confirmation (Phase 6.3) ──────────────────
        // Book-level confirmation of the oracle move: the sub-100ms Binance
        // top-of-book mid must agree with the direction over the last 2s,
        // OR top-of-book size imbalance must lean toward our side. A blip
        // the real market already rejected gets killed here before we pay.
        // No bookTicker data yet (both fields 0) → pass-through, never a veto.
        if (cfg.binance_book_confirm) {
            const bool has_bt_data = row.bt_mid_delta_pct != 0.0 ||
                                     row.bt_imbalance != 0.0;
            if (has_bt_data) {
                const bool mid_agrees = row.bt_mid_delta_pct * delta > 0.0;
                const bool side_up    = delta > 0.0;
                const bool imb_agrees = (side_up ? row.bt_imbalance : -row.bt_imbalance)
                                            >= cfg.binance_book_imb_min;
                if (!mid_agrees && !imb_agrees) return std::nullopt;
            }
        }

        // ── Multi-source directional vote (Phase 4.1) ──────────────────
        // Require >= quorum independent confirmers to agree with the oracle
        // direction. Coinbase leads the oracle by ~1.5-3s; Binance bookTicker
        // mid confirms the top-of-book; liquidation pressure votes on the
        // forced-unwind side. Fewer confirmers than quorum → skip.
        if (cfg.multi_source_vote) {
            int agrees = 0, voters = 0;
            if (row.cb_delta_pct != 0.0) {
                ++voters; if (row.cb_delta_pct * delta > 0.0) ++agrees;
            }
            if (row.bt_mid_delta_pct != 0.0) {
                ++voters; if (row.bt_mid_delta_pct * delta > 0.0) ++agrees;
            }
            if (row.liq_pressure_usd != 0.0) {
                ++voters; if (row.liq_pressure_usd * delta > 0.0) ++agrees;
            }
            if (voters < cfg.multi_source_quorum || agrees < cfg.multi_source_quorum)
                return std::nullopt;
        }

        // ── Midcycle Continuation (Homerun crypto_5m_midcycle) ─────────────
        // At exactly 150s into a 300s cycle: Chainlink distance ≥15bps from
        // strike + winning-side share ask ≤ 0.70 → continuation bet.
        // Only fires on the configured window size (default 5m).
        if (cfg.midcycle_mode) {
            if (row.horizon_s == cfg.midcycle_window_s &&
                secs_left == cfg.midcycle_trigger_secs &&
                std::abs(row.oracle_delta_pct) * 100.0 >= cfg.midcycle_min_distance_bps) {
                bool want_up = row.oracle_delta_pct > 0;
                double price = want_up ? row.up_ask : row.down_ask;
                if (price > 0 && price <= cfg.midcycle_max_share_price &&
                    price < cfg.hard_share_price_ceil &&
                    price >= cfg.min_entry_share_price) {
                    SignalOut s;
                    s.side            = want_up ? ShareSide::UP : ShareSide::DOWN;
                    s.dir             = OrderDir::Buy;
                    s.confidence      = 0.65;
                    s.rec_limit_price = price;
                    s.rec_size_usd    = cfg.per_click_size_usd;
                    s.glyph           = want_up ? '+' : 'v';
                    s.reason = fmt::format(
                        "MIDCYCLE {} oracle_delta={:+.3f}% ask={:.3f} t={:.0f}s",
                        want_up ? "UP" : "DN",
                        row.oracle_delta_pct, price, (double)secs_left);
                    return s;
                }
            }
        }

        // ── Coinbase veto: the oracle crossed the threshold, but Coinbase —
        // which already shows the oracle's NEXT print — has snapped back or
        // reversed. That oracle move is a blip about to revert; skip it.
        if (cfg.coinbase_veto_mode && !row.cb_data_ready) return reject("oracle_cb_stale");
        if (cfg.coinbase_veto_mode && !cb_led) {
            const bool opposite = row.cb_delta_pct * delta < 0.0;
            const bool faded    = std::abs(row.cb_delta_pct) < 0.35 * std::abs(delta);
            if (opposite || faded) return reject("oracle_cb_veto",
                fmt::format("oracle={:+.4f}% cb={:+.4f}%", delta, row.cb_delta_pct));
        }

        const double price_ceil = std::min(cfg.oracle_max_token_price,
                                           cfg.hard_share_price_ceil);

        // Edge guard: skip if the oracle delta is very small — no meaningful edge.
        // Also require a minimum token price movement to avoid noise.
        // REMOVED: the old "market-caught-up" guard used a flawed formula
        // (implied_up_price = 0.5 + delta/2) that always computed ~0.502 for
        // small deltas, then killed the trade because UP shares always trade
        // above 0.48. This was blocking 100% of oracle-lag trades.
        // Now we simply check that the oracle delta is large enough to offer
        // a real edge, and let the market-price filter (price_ceil) do the rest.

        // Pick direction and build signal
        bool want_up = delta > 0;
        double price = want_up ? row.up_ask : row.down_ask;
        if (price <= 0 || price > price_ceil) return reject("oracle_price_band",
            fmt::format("ask={:.3f} cap={:.3f}", price, price_ceil));
        // P3: entry floor — stay in the mid-window band (e.g. 0.40–0.60).
        if (price < cfg.min_entry_share_price) return reject("oracle_price_band",
            fmt::format("ask={:.3f} floor={:.3f}", price, cfg.min_entry_share_price));

        // ── Polymarket book imbalance filter (Phase 4.2) ──────────────────
        // Require |imbalance| ≥ threshold on the entry side. High imbalance
        // means the book is stacked with passive orders supporting our direction.
        if (cfg.book_imb_filter) {
            double imb = want_up ? row.up_imbalance : row.down_imbalance;
            if (imb == 0.0) {
                // No orderbook data yet — pass-through to avoid cold-start deadlock.
            } else if (std::abs(imb) < cfg.book_imb_min) {
                return std::nullopt;
            }
        }

        // P1: predictor-agreement gate. Only fire when the ensemble model agrees
        // with the oracle direction AND is confident. Filters oracle blips that
        // mean-revert (the predictor folds in tick momentum, trade flow, vol, BS).
        if (cfg.oracle_require_pred_agree) {
            double m = cfg.pred_agree_margin;
            bool pred_agrees = want_up ? (row.pred_up >= 0.5 + m)
                                       : (row.pred_up <= 0.5 - m);
            if (!pred_agrees || row.pred_conf < cfg.pred_agree_min_conf)
                return std::nullopt;
        }

        const auto& f = cb_led ? row.fast_forecast : row.reference_forecast;
        if (!f.ready) return reject("oracle_forecast", f.status);
        const auto edge = vp::engine::execution_edge(
            want_up ? f.probability : 1-f.probability, want_up ? f.low : 1-f.high,
            want_up ? row.up_bid : row.down_bid,price,cfg.taker_fee_rate,cfg.fee_exponent);
        if (!edge.executable || edge.conservative < cfg.min_predictor_edge)
            return reject("oracle_edge", fmt::format("{} ask={:.3f} fair={:.4f} stress={:+.4f} need={:.4f} executable={}",
                want_up ? "UP" : "DOWN", price, want_up ? f.probability : 1-f.probability,
                edge.conservative, cfg.min_predictor_edge, edge.executable));
        const double conf = want_up ? f.probability : 1-f.probability;

        SignalOut s;
        s.side            = want_up ? ShareSide::UP : ShareSide::DOWN;
        s.dir             = OrderDir::Buy;
        s.confidence      = conf;
        s.trigger_delta_pct = delta;
        s.rec_limit_price = price;
        s.rec_size_usd    = cfg.per_click_size_usd;
        s.glyph           = want_up ? '+' : 'v';
        s.reason = fmt::format(
            "ORACLE{} d={:+.3f}% cb={:+.3f}% ask={:.3f} net={:+.3f} stress={:+.3f} t={:.0f}s",
            cb_led ? "+CB-LEAD" : "", delta, row.cb_delta_pct,
            price, edge.net, edge.conservative, (double)secs_left);
        return s;
    }

    return std::nullopt;
}

} // namespace rules
} // namespace vp
