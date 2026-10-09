#pragma once
// Polymarket REST: market discovery via Gamma + CLOB endpoints.
#include "core/types.hpp"
#include "polymarket_md.hpp"
#include <string>
#include <vector>
#include <optional>

namespace vp {

struct PmMarket {
    std::string condition_id;
    std::string asset;            // BTC / ETH / SOL / XRP
    int         horizon_seconds = 0;
    // -1 means unsupported/unknown: never silently assume spot settlement.
    int         settlement_window_s = -1;
    bool        reference_is_binance = false;
    std::string reference_key() const {
        if (reference_is_binance) return asset; // Binance buffer uses the bare asset key.
        return settlement_window_s == 0 ? asset + ":SPOT" : asset +
            (settlement_window_s > 0 ? ":TWAP" + std::to_string(settlement_window_s) : ":UNKNOWN");
    }
    std::string reference_price_key() const {
        return reference_is_binance ? asset : asset + ":SPOT";
    }
    double      target_price    = 0;
    std::string yes_token_id;    // UP
    std::string no_token_id;     // DOWN
    std::string question;
    std::string slug;
    std::int64_t window_open_ms  = 0;
    std::int64_t window_close_ms = 0;
    bool        neg_risk = true;
    double      tick_size = 0.01;
};

class PolymarketRest {
public:
    PolymarketRest(std::string clob_url, std::string gamma_url);
    ~PolymarketRest();

    // Returns supported BTC/ETH/SOL/XRP UP-DOWN markets across 5m/15m/1h/4h/24h.
    std::vector<PmMarket> discover_updown_markets();

    // Fetches order books for stale tokens via CLOB REST endpoint.
    // Returns vector of BookUpdate for each token that responded.
    std::vector<PolymarketMD::BookUpdate> fetch_orderbooks(const std::vector<std::string>& token_ids);
    // Live accounting waits for a venue-resolved outcome; oracle estimates
    // are only provisional paper outcomes, never proof of a redeemable payout.
    std::optional<bool> resolved_up_outcome(const std::string& condition_id);
    static std::optional<bool> parse_resolved_up_outcome(std::string_view json,
                                                        std::string_view condition_id);

private:
    std::string clob_url_;
    std::string gamma_url_;
};

} // namespace vp
