#include "engine/impulse_scalper.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <fmt/format.h>

namespace vp::engine {
namespace {
constexpr int64_t kNsPerMs = 1'000'000LL;
constexpr int64_t kNsPerSecond = 1'000'000'000LL;
constexpr int64_t kHistoryNs = 60LL * kNsPerSecond;
constexpr int64_t kMinWarmupNs = 10LL * kNsPerSecond;
constexpr int64_t kEvalCadenceNs = 100LL * kNsPerMs;
constexpr int64_t kFreshQuoteNs = 300LL * kNsPerMs;
constexpr int64_t kShadowHorizonMs = 5'000;
constexpr int64_t kMarkoutsMs[] = {250, 500, 1'000, 2'000};

std::string side_key(const std::string& condition_id, ShareSide side) {
    return condition_id + (side == ShareSide::UP ? "|UP" : "|DOWN");
}

double signed_ofi_delta(double bid, double bid_size, double ask, double ask_size,
                        double prev_bid, double prev_bid_size,
                        double prev_ask, double prev_ask_size) {
    double ofi = 0.0;
    if (bid > prev_bid) ofi += bid_size;
    else if (bid == prev_bid) ofi += bid_size - prev_bid_size;
    else ofi -= prev_bid_size;

    // An ask moving away is buy pressure; an ask moving toward the market is
    // sell pressure. Size reductions at an unchanged ask are positive OFI.
    if (ask < prev_ask) ofi -= ask_size;
    else if (ask == prev_ask) ofi += prev_ask_size - ask_size;
    else ofi += prev_ask_size;
    return ofi;
}

double quantile90(std::vector<double> values) {
    if (values.empty()) return 0.0;
    const auto n = static_cast<std::size_t>(std::floor(0.90 * (values.size() - 1)));
    std::nth_element(values.begin(), values.begin() + n, values.end());
    return values[n];
}

double markout_pct(double bid, double entry) {
    return bid > 0.0 && entry > 0.0 ? 100.0 * (bid / entry - 1.0) : 0.0;
}

std::string json_escape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const unsigned char c : value) {
        switch (c) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (c < 0x20) escaped += fmt::format("\\u{:04x}", c);
            else escaped.push_back(static_cast<char>(c));
        }
    }
    return escaped;
}
} // namespace

void ImpulseScalperDetector::on_book_ticker(
    const std::string& asset, double bid, double bid_size,
    double ask, double ask_size, int64_t recv_mono_ns) {
    if (asset.empty() || !(bid > 0.0) || !(ask > bid) ||
        bid_size < 0.0 || ask_size < 0.0) return;
    if (recv_mono_ns <= 0) return;

    std::lock_guard<std::mutex> lock(mu_);
    auto& h = history_[asset];
    const double mid = 0.5 * (bid + ask);
    double ofi_norm = 0.0;
    if (!h.quotes.empty()) {
        const auto& prev = h.quotes.back();
        if (recv_mono_ns <= prev.ts_ns) return;
        const double depth = 0.25 * (bid_size + ask_size +
                                     prev.bid_size + prev.ask_size);
        if (depth > 0.0) {
            ofi_norm = signed_ofi_delta(bid, bid_size, ask, ask_size,
                                        prev.bid, prev.bid_size,
                                        prev.ask, prev.ask_size) / depth;
        }
    }
    h.quotes.push_back({recv_mono_ns, mid, bid, bid_size, ask, ask_size, ofi_norm});
    const int64_t cutoff = recv_mono_ns - kHistoryNs;
    while (h.quotes.size() > 2 && h.quotes[1].ts_ns < cutoff)
        h.quotes.pop_front();
}

ImpulseFeatures ImpulseScalperDetector::evaluate(
    const std::string& asset, int64_t now_mono_ns) {
    ImpulseFeatures out;
    std::lock_guard<std::mutex> lock(mu_);
    auto it = history_.find(asset);
    if (it == history_.end() || it->second.quotes.size() < 3) return out;
    auto& h = it->second;
    if (now_mono_ns <= 0) return out;

    const int64_t cutoff_ns = now_mono_ns - kHistoryNs;
    while (h.quotes.size() > 2 && h.quotes[1].ts_ns < cutoff_ns)
        h.quotes.pop_front();
    while (!h.ofi_scores.empty() && h.ofi_scores.front().first < cutoff_ns)
        h.ofi_scores.pop_front();

    const auto& latest = h.quotes.back();
    if (now_mono_ns < latest.ts_ns || now_mono_ns - latest.ts_ns > kFreshQuoteNs) {
        out.gate_reason = "quote_stale";
        return out;
    }

    auto window_values = [&](int64_t window_ms, double& move_bps,
                             double& ofi) -> bool {
        const int64_t start = now_mono_ns - window_ms * kNsPerMs;
        const QuoteSample* prior = nullptr;
        for (const auto& q : h.quotes) {
            if (q.ts_ns <= start) prior = &q;
            else break;
        }
        if (!prior || !(prior->mid > 0.0) || !(latest.mid > 0.0)) return false;
        move_bps = 10'000.0 * std::log(latest.mid / prior->mid);
        ofi = 0.0;
        for (const auto& q : h.quotes)
            if (q.ts_ns > start && q.ts_ns <= now_mono_ns)
                ofi += q.normalized_ofi_delta;
        return std::isfinite(move_bps) && std::isfinite(ofi);
    };
    if (!window_values(250, out.move_250ms_bps, out.ofi_250ms) ||
        !window_values(500, out.move_500ms_bps, out.ofi_500ms)) {
        out.gate_reason = "short_window_history";
        return out;
    }

    double sum_sq_bps = 0.0;
    int returns = 0;
    const QuoteSample* prev = nullptr;
    for (const auto& q : h.quotes) {
        if (q.ts_ns < cutoff_ns) continue;
        if (prev && q.ts_ns > prev->ts_ns && prev->mid > 0.0 && q.mid > 0.0) {
            const double r = 10'000.0 * std::log(q.mid / prev->mid);
            if (std::isfinite(r)) {
                sum_sq_bps += r * r;
                ++returns;
            }
        }
        prev = &q;
    }
    out.quote_samples_60s = returns;
    const int64_t coverage_ns = latest.ts_ns - h.quotes.front().ts_ns;
    if (returns < 20 || coverage_ns < kMinWarmupNs) {
        out.gate_reason = "impulse_warmup";
        return out;
    }
    if (!(sum_sq_bps > 0.0)) {
        out.gate_reason = "volatility_flat";
        return out;
    }

    const double coverage_ms = static_cast<double>(coverage_ns) / kNsPerMs;
    out.realized_vol_1m_bps = std::sqrt(sum_sq_bps * 60'000.0 / coverage_ms);
    if (!(out.realized_vol_1m_bps > 0.0) || !std::isfinite(out.realized_vol_1m_bps)) {
        out.gate_reason = "volatility_unavailable";
        return out;
    }
    const double expected_250 = std::max(
        0.05, out.realized_vol_1m_bps * std::sqrt(250.0 / 60'000.0));
    const double expected_500 = std::max(
        0.05, out.realized_vol_1m_bps * std::sqrt(500.0 / 60'000.0));
    out.move_z_250ms = out.move_250ms_bps / expected_250;
    out.move_z_500ms = out.move_500ms_bps / expected_500;

    if (h.last_eval_ns == 0 || now_mono_ns - h.last_eval_ns >= kEvalCadenceNs) {
        std::vector<double> prior_scores;
        prior_scores.reserve(h.ofi_scores.size());
        for (const auto& s : h.ofi_scores) prior_scores.push_back(s.second);
        if (prior_scores.size() >= 20)
            out.ofi_threshold = std::max(0.15, quantile90(std::move(prior_scores)));
        h.ofi_scores.emplace_back(now_mono_ns, std::abs(out.ofi_250ms));
        h.last_eval_ns = now_mono_ns;
    } else {
        std::vector<double> prior_scores;
        prior_scores.reserve(h.ofi_scores.size());
        for (const auto& s : h.ofi_scores) prior_scores.push_back(s.second);
        if (prior_scores.size() >= 20)
            out.ofi_threshold = std::max(0.15, quantile90(std::move(prior_scores)));
    }

    const int dir = out.move_250ms_bps > 0.0 ? 1
        : out.move_250ms_bps < 0.0 ? -1 : 0;
    out.ready = out.ofi_threshold > 0.0;
    if (!out.ready) {
        out.gate_reason = "ofi_warmup";
        return out;
    }
    const bool short_move = dir * out.move_250ms_bps >= 0.10;
    const bool confirm_move = dir * out.move_500ms_bps >= 0.10;
    const bool order_flow_agrees = dir * out.ofi_250ms >= out.ofi_threshold;
    const bool volatility_scaled_move = dir * out.move_z_250ms >= 1.25 &&
                                        dir * out.move_z_500ms >= 0.50;
    if (dir == 0) out.gate_reason = "midpoint_flat";
    else if (!short_move) out.gate_reason = "midpoint_250ms_below_0.10bp";
    else if (!confirm_move) out.gate_reason = "midpoint_500ms_not_confirming";
    else if (!order_flow_agrees) out.gate_reason = "ofi_250ms_below_p90";
    else if (!volatility_scaled_move) out.gate_reason = "volatility_scaled_move_small";
    else if (dir * out.ofi_500ms <= 0.0) out.gate_reason = "ofi_500ms_disagrees";
    else {
        out.candidate = true;
        out.gate_reason = "candidate";
    }
    out.direction = out.candidate ? dir : 0;
    return out;
}

bool ImpulseShadowTracker::admit(const std::string& condition_id, ShareSide side,
                                 int direction, int64_t now_ms) {
    if (condition_id.empty() || direction == 0) return false;
    std::lock_guard<std::mutex> lock(mu_);
    const std::string key = side_key(condition_id, side) +
        (direction > 0 ? "|UP_IMPULSE" : "|DOWN_IMPULSE");
    auto& last = last_candidate_ms_[key];
    if (last > 0 && now_ms - last < 1'000) return false;
    last = now_ms;
    return true;
}

bool ImpulseShadowTracker::sample_due(const std::string& condition_id,
                                      int64_t now_ms) {
    if (condition_id.empty()) return false;
    std::lock_guard<std::mutex> lock(mu_);
    auto& last = last_sample_ms_[condition_id];
    if (last > 0 && now_ms - last < 1'000) return false;
    last = now_ms;
    return true;
}

void ImpulseShadowTracker::add(ImpulseShadowCandidate candidate) {
    if (candidate.id.empty() || candidate.condition_id.empty() ||
        !(candidate.maker_quote > 0.0)) return;
    std::lock_guard<std::mutex> lock(mu_);
    auto& rows = pending_[side_key(candidate.condition_id, candidate.side)];
    if (rows.size() >= 8) rows.erase(rows.begin());
    rows.push_back(std::move(candidate));
}

std::string ImpulseShadowTracker::make_markout(
    const ImpulseShadowCandidate& c, int milestone_ms, int64_t now_ms,
    bool terminal, const char* terminal_reason) {
    const double age_ms = static_cast<double>(now_ms - c.created_ms);
    return fmt::format(
        R"({{"shadow_id":"{}","instance":"{}","session_id":"{}","side":"{}","milestone_ms":{},"observed_after_ms":{},"quote_age_ms":{},"maker_quote":{:.8f},"best_bid":{:.8f},"best_ask":{:.8f},"markout_pct_vs_quote":{:.6f},"target_bid":{:.8f},"stop_bid":{:.8f},"target_touched":{},"stop_touched":{},"first_barrier":"{}","terminal":{},"terminal_reason":"{}","maker_fill":"UNKNOWN"}})",
        c.id, json_escape(c.instance), c.session_id,
        c.side == ShareSide::UP ? "UP" : "DOWN", milestone_ms,
        age_ms, c.last_quote_ms > 0 ? now_ms - c.last_quote_ms : -1,
        c.maker_quote, c.last_bid, c.last_ask,
        markout_pct(c.last_bid, c.maker_quote), c.target_bid, c.stop_bid,
        c.target_touched, c.stop_touched,
        c.first_barrier.empty() ? "NONE" : c.first_barrier,
        terminal, terminal_reason);
}

std::string ImpulseShadowTracker::make_outcome(
    const ImpulseShadowCandidate& c, int64_t now_ms, const char* reason) {
    return fmt::format(
        R"({{"shadow_id":"{}","instance":"{}","session_id":"{}","side":"{}","created_ms":{},"observed_ms":{},"maker_quote":{:.8f},"target_bid":{:.8f},"stop_bid":{:.8f},"last_bid":{:.8f},"last_ask":{:.8f},"last_quote_age_ms":{},"first_barrier":"{}","first_barrier_ms":{},"target_touched":{},"stop_touched":{},"target_touch_before_stop":{},"maker_fill":"UNKNOWN","label":"bid_touch_path_only","completion_reason":"{}"}})",
        c.id, json_escape(c.instance), c.session_id,
        c.side == ShareSide::UP ? "UP" : "DOWN", c.created_ms, now_ms,
        c.maker_quote, c.target_bid, c.stop_bid, c.last_bid, c.last_ask,
        c.last_quote_ms > 0 ? now_ms - c.last_quote_ms : -1,
        c.first_barrier.empty() ? "NONE" : c.first_barrier,
        c.first_barrier_ms,
        c.target_touched, c.stop_touched,
        c.first_barrier == "TARGET", reason);
}

std::vector<ImpulseShadowEvent> ImpulseShadowTracker::on_book_update(
    const std::string& condition_id, ShareSide side,
    double best_bid, double best_ask, int64_t now_ms) {
    std::vector<ImpulseShadowEvent> events;
    if (condition_id.empty() || !(best_bid > 0.0) || !(best_ask > best_bid))
        return events;
    std::lock_guard<std::mutex> lock(mu_);
    const std::string key = side_key(condition_id, side);
    auto it = pending_.find(key);
    if (it == pending_.end()) return events;
    auto& rows = it->second;
    for (auto& c : rows) {
        if (now_ms < c.created_ms) continue;
        c.last_bid = best_bid;
        c.last_ask = best_ask;
        c.last_quote_ms = now_ms;
        if (c.target_bid > 0.0 && best_bid >= c.target_bid)
            c.target_touched = true;
        if (c.stop_bid > 0.0 && best_bid <= c.stop_bid)
            c.stop_touched = true;
        if (c.first_barrier.empty()) {
            if (c.target_touched) {
                c.first_barrier = "TARGET";
                c.first_barrier_ms = now_ms - c.created_ms;
            } else if (c.stop_touched) {
                c.first_barrier = "STOP";
                c.first_barrier_ms = now_ms - c.created_ms;
            }
        }
        const int64_t age_ms = now_ms - c.created_ms;
        for (std::size_t i = 0; i < std::size(kMarkoutsMs); ++i) {
            if (!c.markout_written[i] && age_ms >= kMarkoutsMs[i]) {
                c.markout_written[i] = true;
                events.push_back({c.condition_id, "scalper_impulse_markout",
                    make_markout(c, static_cast<int>(kMarkoutsMs[i]), now_ms,
                                 false, "")});
            }
        }
    }
    auto row = rows.begin();
    while (row != rows.end()) {
        if (now_ms - row->created_ms >= kShadowHorizonMs) {
            events.push_back({row->condition_id, "scalper_impulse_outcome",
                make_outcome(*row, now_ms,
                    row->last_quote_ms > 0 && now_ms - row->last_quote_ms <= 1'000
                        ? "five_second_horizon" : "quote_stale")});
            row = rows.erase(row);
        } else ++row;
    }
    if (rows.empty()) pending_.erase(it);
    return events;
}

std::vector<ImpulseShadowEvent> ImpulseShadowTracker::advance(int64_t now_ms) {
    std::vector<ImpulseShadowEvent> events;
    std::lock_guard<std::mutex> lock(mu_);
    for (auto it = pending_.begin(); it != pending_.end();) {
        auto& rows = it->second;
        auto row = rows.begin();
        while (row != rows.end()) {
            if (now_ms - row->created_ms < kShadowHorizonMs) {
                ++row;
                continue;
            }
            for (std::size_t i = 0; i < std::size(kMarkoutsMs); ++i) {
                if (!row->markout_written[i]) {
                    row->markout_written[i] = true;
                    events.push_back({row->condition_id, "scalper_impulse_markout",
                        make_markout(*row, static_cast<int>(kMarkoutsMs[i]),
                                     now_ms, true, "no_quote_at_deadline")});
                }
            }
            events.push_back({row->condition_id, "scalper_impulse_outcome",
                make_outcome(*row, now_ms,
                    row->last_quote_ms > 0 && now_ms - row->last_quote_ms <= 1'000
                        ? "five_second_horizon" : "quote_stale")});
            row = rows.erase(row);
        }
        if (rows.empty()) it = pending_.erase(it);
        else ++it;
    }
    for (auto it = last_candidate_ms_.begin(); it != last_candidate_ms_.end();) {
        if (now_ms - it->second > 60'000) it = last_candidate_ms_.erase(it);
        else ++it;
    }
    for (auto it = last_sample_ms_.begin(); it != last_sample_ms_.end();) {
        if (now_ms - it->second > 60'000) it = last_sample_ms_.erase(it);
        else ++it;
    }
    return events;
}

} // namespace vp::engine
