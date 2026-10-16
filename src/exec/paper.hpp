#pragma once
// Paper-trading executor. Simulates fills against the live order book at
// time of submission. All orders + fills are persisted via the journal
// callback. Caller is responsible for risk checks (caps, deployed limit).
#include "core/types.hpp"
#include "exec/positions.hpp"
#include "market/orderbook.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
namespace vp {
enum class ExecutionMode : uint8_t { Taker = 0, Maker, Hybrid };

struct OrderRequest {
    std::string intent_id;          // immutable idempotency key; generated once when absent
    std::string condition_id;
    std::string asset;
    std::string manual_market_question; // populated for mobile catalog trades
    std::string outcome_label;          // exact Gamma outcome label for the token
    int         horizon_s = 0;
    std::string token_id;
    ShareSide   side = ShareSide::UP;
    OrderDir    dir  = OrderDir::Buy;
    double      size_usd = 0;       // for buys
    double      size_shares = 0;    // sells; internal maker replacement may pin remaining buy shares
    std::string source;             // HUMAN_KEYPRESS | RULE | LLM
    double      limit_price = 0;    // 0 => market
    std::int64_t window_close_ms = 0;
    bool        neg_risk = true;    // per-market signing domain
    double      tick_size = 0.01;   // price tick for CLOB amount rounding
    double      entry_cross_ticks = 2.0; // buy FAK crosses best ask by this many ticks
    ExecutionMode execution_mode = ExecutionMode::Maker;
    int         maker_improve_ticks = 0; // improve best bid, while remaining passive
    int         maker_reprice_ms = 750;
    int         maker_poll_ms = 500;
    int         maker_max_age_ms = 5000;
    int         maker_cancel_before_close_ms = 3000;
    bool        maker_fixed_price = false; // strategy-owned target; never chase the touch
    double      maker_min_price = 0.0; // optional floor for a passive BUY quote
    bool        stop_loss_fak_authorized = false; // fixed-source hard stops only; executors re-check the source
    std::int64_t maker_root_deadline_ms = 0; // internal: preserved across replacements
    // Fee model (Polymarket taker fee)
    double      fee_rate = 0.07;    // from strategy.hot.taker_fee_rate
    int         fee_exponent = 1;   // from strategy.hot.fee_exponent
    // Recycler decision-time evidence, persisted with the signal record.
    double      decision_fair_probability = 0;
    double      decision_edge = 0;
    double      decision_reference_price = 0;
    double      decision_reference_strike = 0;
    double      decision_reference_vol_per_sec = 0;
    double      decision_model_up_probability = 0;
    double      decision_model_up_low = 0;
    double      decision_model_up_high = 0;
    double      decision_midpoint = 0;
    int64_t     decision_reference_age_ms = 0;
};
struct OrderResult {
    bool         accepted = false;
    std::string  reject_reason;
    std::string  client_order_id;   // UUID
    std::string  exchange_order_id; // venue order hash (live orders)
    std::string  status;            // rejected|acked|partial|filled|cancelled
    bool         resting = false;
    bool         terminal = false;
    double       order_price = 0;
    double       order_shares = 0;
    double       fill_price = 0;
    double       fill_shares = 0;
    double       fee_usd = 0;       // taker fee deducted: rate * p * (1-p) * shares
    Position     after;
    // Structured rejection classification — machine-readable code for telemetry + retry logic.
    enum class RejectClass : uint8_t {
        None = 0,
        InvalidSignature,       // POLY_1271 / "invalid signature" — retry after sigtype refresh
        BalanceAllowance,       // "not enough balance/allowance" — inspect shortage, retry or abort
        PostOnlyCross,          // post-only order would cross — reprice toward touch
        Transport,              // HTTP error / timeout — backoff + retry
        AuthFailure,            // "maker address not allowed" / HMAC auth failures — trip circuit breaker
        InvalidPayload,         // "Invalid order payload" — stale tick / neg_risk / price off-grid
        ServerError,            // 5xx CLOB internal error — retry with backoff
        NoBook,                 // no bids/asks on book
        SizeBelowMinimum,       // rounded order size < CLOB minimum
        OperatorBand,           // outside operator-installed entry price band (defense-in-depth)
        DuplicateOrRateLimit,   // 429 or duplicate order id
        RiskCap,                // blocked by the central vp::risk checks (entry only)
        Unknown,                // catch-all
    };
    RejectClass  reject_class = RejectClass::None;
    // Per-reject diagnostic payload (JSON for extensibility).
    std::string  reject_detail_json;
};
// Journaling callback the executor invokes for every order + fill.
struct Journal {
    std::function<void(const OrderRequest&, const OrderResult&)> on_order;
    // Must durably reserve a new intent before the executor can affect positions
    // or contact a venue. False means duplicate ID or persistence unavailable.
    std::function<bool(const OrderRequest&, const std::string&)> before_submit;
    // Record each changed resting price so paper fills can be replayed against
    // the quote that was active when the simulated fill occurred.
    std::function<void(const OrderRequest&, const std::string&, double, double)>
        on_reprice;
};
class PaperExecutor {
public:
    PaperExecutor(Positions& pos, Journal journal,
                  double max_order_usd, double max_deployed_usd,
                  double max_per_window_usd = 10.0,
                  double min_order_usd      = 1.0,
                  double clob_min_shares_fallback = 5.0);
    // Submit a manual or rule-driven order. Looks up the live book via the
    // book_lookup callback (cond_id, ShareSide -> OrderBook*).
    OrderResult submit(const OrderRequest& req,
                       const std::function<const OrderBook*(const std::string&, ShareSide)>& book_lookup);

    // Advance simulated maker orders. Returns the number of lifecycle changes.
    std::size_t maintain_open_orders(
        const std::function<const OrderBook*(const std::string&, ShareSide)>& book_lookup,
        std::int64_t now_ms);
    std::size_t open_order_count() const;
    void cancel_open_orders();
    bool cancel_order(const std::string& client_order_id);
    struct OpenOrderView {
        OrderRequest req;
        std::string client_order_id;
        double price = 0;
        double remaining_shares = 0;
    };
    std::vector<OpenOrderView> open_orders() const;

    // Hot-reload risk caps at runtime (refresh thread; see LiveExecutor).
    void set_limits(double max_order_usd, double max_deployed_usd,
                    double max_per_window_usd, double min_order_usd,
                    double clob_min_shares_fallback) {
        max_order_usd_      = max_order_usd;
        max_deployed_usd_   = max_deployed_usd;
        max_per_window_usd_ = max_per_window_usd;
        min_order_usd_      = min_order_usd;
        clob_min_shares_fallback_ = clob_min_shares_fallback;
    }
private:
    Positions& positions_;
    Journal    journal_;
    double     max_order_usd_;
    double     max_deployed_usd_;
    double     max_per_window_usd_;
    double     min_order_usd_;
    double     clob_min_shares_fallback_;
    struct OpenOrder {
        OrderRequest req;
        std::string client_order_id;
        double price = 0;
        double shares = 0;
        std::int64_t placed_ms = 0;
        std::int64_t last_reprice_ms = 0;
        double original_shares = 0;
        std::uint64_t last_fill_generation = 0;
    };
    mutable std::mutex open_mu_;
    std::unordered_map<std::string, OpenOrder> open_orders_;
};
} // namespace vp
