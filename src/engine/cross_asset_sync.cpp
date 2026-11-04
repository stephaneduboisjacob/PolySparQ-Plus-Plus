#include "engine/cross_asset_sync.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

namespace vp {
namespace {

bool valid_book(double bid, double ask, double bid_size, double ask_size) {
    return std::isfinite(bid) && std::isfinite(ask) &&
           std::isfinite(bid_size) && std::isfinite(ask_size) &&
           bid > 0.0 && ask < 1.0 && ask >= bid &&
           bid_size > 0.0 && ask_size > 0.0;
}

double median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const auto n = values.size();
    if (n % 2) return values[n / 2];
    return (values[n / 2 - 1] + values[n / 2]) * 0.5;
}

} // namespace

std::vector<CrossAssetSyncSample> evaluate_cross_asset_sync(
    const std::vector<CrossAssetSyncInput>& inputs,
    std::int64_t sample_slot_ms,
    const CrossAssetSyncConfig& config) {
    std::map<std::pair<std::int64_t, std::int64_t>,
             std::vector<const CrossAssetSyncInput*>> groups;
    for (const auto& input : inputs) {
        if (input.horizon_seconds != 300 || input.condition_id.empty() ||
            input.asset.empty() || input.window_open_ms <= 0 ||
            input.window_close_ms <= input.window_open_ms)
            continue;
        groups[{input.window_open_ms, input.window_close_ms}].push_back(&input);
    }

    std::vector<CrossAssetSyncSample> output;
    for (const auto& [window, members] : groups) {
        std::set<std::string> eligible_assets;
        std::map<const CrossAssetSyncInput*, double> probabilities;
        std::map<const CrossAssetSyncInput*, std::string> invalid_reasons;

        for (const auto* input : members) {
            const auto up_age = input->up_book_age_ms;
            const auto down_age = input->down_book_age_ms;
            const auto age = (up_age < 0 || down_age < 0)
                ? std::int64_t{-1} : std::max(up_age, down_age);
            const bool books_valid =
                valid_book(input->up_bid, input->up_ask,
                           input->up_bid_size, input->up_ask_size) &&
                valid_book(input->down_bid, input->down_ask,
                           input->down_bid_size, input->down_ask_size);
            if (!books_valid) {
                invalid_reasons[input] = "incomplete_or_invalid_book";
                continue;
            }
            if (age < 0 || age > config.max_book_age_ms) {
                invalid_reasons[input] = "stale_book";
                continue;
            }
            const double up_spread = input->up_ask - input->up_bid;
            const double down_spread = input->down_ask - input->down_bid;
            if (up_spread > config.max_spread || down_spread > config.max_spread) {
                invalid_reasons[input] = "spread_too_wide";
                continue;
            }
            const double up_mid = (input->up_bid + input->up_ask) * 0.5;
            const double down_mid = (input->down_bid + input->down_ask) * 0.5;
            const double probability = (up_mid + (1.0 - down_mid)) * 0.5;
            if (!std::isfinite(probability) || probability <= 0.0 || probability >= 1.0) {
                invalid_reasons[input] = "invalid_midpoint_proxy";
                continue;
            }
            // One vote per asset for an exact window, even if market discovery
            // temporarily returns duplicate condition rows.
            if (!eligible_assets.contains(input->asset)) {
                eligible_assets.insert(input->asset);
                probabilities.emplace(input, probability);
            } else {
                invalid_reasons[input] = "duplicate_asset_in_window";
            }
        }

        for (const auto* input : members) {
            CrossAssetSyncSample sample;
            sample.sample_slot_ms = sample_slot_ms;
            sample.condition_id = input->condition_id;
            sample.asset = input->asset;
            sample.window_open_ms = input->window_open_ms;
            sample.window_close_ms = input->window_close_ms;
            sample.eligible_assets = static_cast<int>(eligible_assets.size());
            sample.fee_rate = config.taker_fee_rate;
            sample.fee_exponent = config.fee_exponent;
            sample.up_bid = input->up_bid;
            sample.up_bid_size = input->up_bid_size;
            sample.up_ask = input->up_ask;
            sample.up_ask_size = input->up_ask_size;
            sample.down_bid = input->down_bid;
            sample.down_bid_size = input->down_bid_size;
            sample.down_ask = input->down_ask;
            sample.down_ask_size = input->down_ask_size;
            sample.up_spread = std::max(0.0, input->up_ask - input->up_bid);
            sample.down_spread = std::max(0.0, input->down_ask - input->down_bid);
            sample.up_ask_depth_usd = std::max(0.0, input->up_ask * input->up_ask_size);
            sample.down_ask_depth_usd = std::max(0.0, input->down_ask * input->down_ask_size);
            sample.book_age_ms = input->up_book_age_ms < 0 || input->down_book_age_ms < 0
                ? -1 : std::max(input->up_book_age_ms, input->down_book_age_ms);

            const auto probability_it = probabilities.find(input);
            if (probability_it == probabilities.end()) {
                const auto reason_it = invalid_reasons.find(input);
                sample.reason = reason_it == invalid_reasons.end()
                    ? "duplicate_asset_in_window" : reason_it->second;
                output.push_back(std::move(sample));
                continue;
            }

            sample.quote_eligible = true;
            sample.has_probability = true;
            sample.up_probability = probability_it->second;
            if (static_cast<int>(eligible_assets.size()) < config.min_distinct_assets) {
                sample.reason = "insufficient_cross_asset_coverage";
                output.push_back(std::move(sample));
                continue;
            }

            std::vector<double> peer_probabilities;
            for (const auto& [peer, probability] : probabilities)
                if (peer != input) peer_probabilities.push_back(probability);
            if (peer_probabilities.size() < 2) {
                sample.reason = "insufficient_distinct_peers";
                output.push_back(std::move(sample));
                continue;
            }
            sample.peer_median_up = median(std::move(peer_probabilities));
            sample.has_peer_comparison = true;
            const double up_gap = sample.peer_median_up - sample.up_probability;
            sample.divergence_pct = std::abs(up_gap) * 100.0;
            if (sample.divergence_pct < config.min_divergence_pct) {
                sample.reason = "below_min_divergence";
                output.push_back(std::move(sample));
                continue;
            }

            sample.side = up_gap > 0.0 ? "UP" : "DOWN";
            sample.ask_price = sample.side == "UP" ? input->up_ask : input->down_ask;
            sample.ask_depth_usd = sample.side == "UP"
                ? sample.up_ask_depth_usd : sample.down_ask_depth_usd;
            if (sample.ask_depth_usd < config.min_touch_depth_usd) {
                sample.reason = "insufficient_best_ask_touch_depth";
                output.push_back(std::move(sample));
                continue;
            }

            const int exponent = std::max(0, config.fee_exponent);
            sample.estimated_taker_fee_per_share = config.taker_fee_rate *
                std::pow(sample.ask_price * (1.0 - sample.ask_price), exponent);
            const double peer_side_probability = sample.side == "UP"
                ? sample.peer_median_up : (1.0 - sample.peer_median_up);
            sample.estimated_net_proxy_gap_per_share = peer_side_probability -
                sample.ask_price - sample.estimated_taker_fee_per_share;
            if (sample.estimated_net_proxy_gap_per_share <= 0.0) {
                sample.reason = "nonpositive_after_configured_fee_proxy";
                output.push_back(std::move(sample));
                continue;
            }

            sample.candidate = true;
            sample.reason = "shadow_candidate_only";
            output.push_back(std::move(sample));
        }
    }
    return output;
}

} // namespace vp
