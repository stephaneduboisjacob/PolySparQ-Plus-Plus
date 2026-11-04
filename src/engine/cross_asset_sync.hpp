#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vp {

// Read-only observations used to study contemporaneous 5-minute crypto
// markets. These values never enter an order request or a trading gate.
struct CrossAssetSyncConfig {
    bool enabled = false;
    double min_divergence_pct = 4.0;       // probability percentage points
    int max_book_age_ms = 1500;
    double max_spread = 0.08;              // absolute share-price spread
    double min_touch_depth_usd = 5.0;
    int min_distinct_assets = 3;
    double taker_fee_rate = 0.07;
    int fee_exponent = 1;
};

struct CrossAssetSyncInput {
    std::string condition_id;
    std::string asset;
    int horizon_seconds = 0;
    std::int64_t window_open_ms = 0;
    std::int64_t window_close_ms = 0;
    std::int64_t up_book_age_ms = -1;
    std::int64_t down_book_age_ms = -1;
    double up_bid = 0.0;
    double up_bid_size = 0.0;
    double up_ask = 0.0;
    double up_ask_size = 0.0;
    double down_bid = 0.0;
    double down_bid_size = 0.0;
    double down_ask = 0.0;
    double down_ask_size = 0.0;
};

struct CrossAssetSyncSample {
    std::int64_t sample_slot_ms = 0;
    std::string condition_id;
    std::string asset;
    int horizon_seconds = 300;
    std::int64_t window_open_ms = 0;
    std::int64_t window_close_ms = 0;
    int eligible_assets = 0;
    bool quote_eligible = false;
    bool candidate = false;
    bool has_probability = false;
    bool has_peer_comparison = false;
    std::string side;
    std::string reason;
    double up_probability = 0.0;           // midpoint proxy, not a forecast
    double peer_median_up = 0.0;
    double divergence_pct = 0.0;
    double up_bid = 0.0;
    double up_bid_size = 0.0;
    double up_ask = 0.0;
    double up_ask_size = 0.0;
    double down_bid = 0.0;
    double down_bid_size = 0.0;
    double down_ask = 0.0;
    double down_ask_size = 0.0;
    double up_spread = 0.0;
    double down_spread = 0.0;
    double up_ask_depth_usd = 0.0;          // best-ask touch only
    double down_ask_depth_usd = 0.0;
    std::int64_t book_age_ms = -1;
    double ask_price = 0.0;
    double ask_depth_usd = 0.0;
    double fee_rate = 0.0;
    int fee_exponent = 1;
    double estimated_taker_fee_per_share = 0.0;
    double estimated_net_proxy_gap_per_share = 0.0;
};

// Groups exact matching window boundaries, rejects stale/wide books, and
// measures outliers against the median of the *other* eligible assets.
// Candidate tags describe research opportunities only; this function cannot
// submit, size, or modify orders.
std::vector<CrossAssetSyncSample> evaluate_cross_asset_sync(
    const std::vector<CrossAssetSyncInput>& inputs,
    std::int64_t sample_slot_ms,
    const CrossAssetSyncConfig& config);

} // namespace vp
