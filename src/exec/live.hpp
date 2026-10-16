#pragma once
// Live order executor — places FAK taker and post-only GTC maker orders.
//
// API surface mirrors PaperExecutor so the caller can swap them by mode.
// All orders require an explicit human keypress to reach this code
// (invariant enforced in main.cpp key callback).
//
// Blocking: submit() makes a synchronous HTTP call (~200-500ms typical).
// Call from a separate thread, not the TUI or engine thread.
#include "exec/paper.hpp"       // reuses OrderRequest / OrderResult / Journal
#include "market/orderbook.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vp {

struct WalletConfig;  // forward decl (defined in config.hpp)

class LiveExecutor {
public:
    LiveExecutor(Positions& pos, Journal journal,
                 double max_order_usd, double max_deployed_usd,
                 double max_per_window_usd,
                 double min_order_usd,
                 double clob_min_shares_fallback,
                 const std::string& clob_url,
                 const std::string& maker_addr,     // funder (== signer for sigType 0)
                 const std::string& signer_addr,    // EOA
                 const std::string& api_key,
                 const std::string& api_secret,
                 const std::string& api_passphrase,
                 const std::string& private_key_hex,  // loaded from private_key_file
                 int  signature_type,                // 0=EOA, 1=POLY_PROXY, 2=POLY_GNOSIS_SAFE
                 bool neg_risk,
                 std::string builder_code = {});     // bytes32 hex from builder registration

    // Submit an order. Identical return semantics to PaperExecutor::submit.
    // Returns OrderResult with accepted=false + reject_class + reject_reason on error.
    OrderResult submit(const OrderRequest& req,
                       const std::function<const OrderBook*(const std::string&, ShareSide)>& book_lookup);

    // Poll, fill-reconcile, cancel/reprice and (for hybrid mode) fall back to FAK.
    // Must be called periodically from the engine thread.
    std::size_t maintain_open_orders(
        const std::function<const OrderBook*(const std::string&, ShareSide)>& book_lookup,
        std::int64_t now_ms, const std::function<bool()>& may_replace);
    std::size_t open_order_count() const;
    struct OpenOrderView {
        OrderRequest req;
        std::string client_order_id;
        std::string exchange_order_id;
        double price = 0;
        double remaining_shares = 0;
    };
    std::vector<OpenOrderView> open_orders() const;
    void cancel_open_orders();
    // Cancel one locally tracked venue order and journal the acknowledged result.
    bool cancel_open_order(const std::string& exchange_order_id,
                           const std::string& client_order_id);

    // Restore one locally-persisted GTC order after a process restart. Venue state
    // is verified by maintain_open_orders before any fill is applied.
    void adopt_open_order(const OrderRequest& req,
                          const std::string& client_order_id,
                          const std::string& exchange_order_id,
                          double price, double original_shares,
                          double already_matched_shares,
                          std::int64_t placed_ms);

    // CLOB auth circuit breaker — set true when maker-not-allowed is detected;
    // all subsequent submissions are rejected until the caller resets it on
    // credential change. Lives on the executor because it must survive hot-reload.
    bool auth_circuit_open() const noexcept { return auth_circuit_open_.load(); }
    void set_auth_circuit(bool open) noexcept { auth_circuit_open_.store(open); }

    // Hot-reload the risk caps at runtime. Called from the refresh thread when
    // config/polypulse.toml changes (same thread that calls submit(), so no
    // locking needed). Lets the supervisor scale sizing up to the wallet.
    void set_limits(double max_order_usd, double max_deployed_usd,
                    double max_per_window_usd, double min_order_usd,
                    double clob_min_shares_fallback) {
        max_order_usd_      = max_order_usd;
        max_deployed_usd_   = max_deployed_usd;
        max_per_window_usd_ = max_per_window_usd;
        min_order_usd_      = min_order_usd;
        clob_min_shares_fallback_ = clob_min_shares_fallback;
    }

    // Fetch the live pUSD wallet balance from the CLOB /balance endpoint.
    // Returns -1.0 on failure so callers can ignore bad reads.
    double fetch_balance() const;

    // Fetch the REAL conditional-token (share) balance for a token from the
    // CLOB. Used to cap sells to what we actually hold and reconcile phantom
    // positions. Returns -1.0 on failure (caller should not act on a bad read).
    double fetch_conditional_balance(const std::string& token_id) const;

    // Post a CLOB session heartbeat (POST /v1/heartbeats). Docs: without one
    // every ~5s, the CLOB cancels open orders for safety. Call periodically.
    // Returns the heartbeat_id from the response for use in the next call.
    // An empty string on first call is correct per docs.
    std::string post_heartbeat(const std::string& prev_heartbeat_id = "") const;

    // Refresh the USDC balance cache from the CLOB (network call). Called by
    // the periodic wallet-refresh loop and at startup; the submit hot path
    // only ever reads the cache (never blocks on I/O).
    // Returns the fetched balance (or -1.0 on failure).
    double refresh_balance_cache() const;

    // Phase 1.6: cold-start mitigation — pre-populate per-token caches
    // (tick-size, neg-risk, min-order-size) BEFORE the first order fires.
    // Without this, the first submit pays 3 sequential REST lookups
    // (~0.5-1.5s) at signal time on a book that is microseconds stale.
    // Call from a background thread at startup; paces requests politely.
    void warm_token_caches(const std::vector<std::string>& token_ids) const;


private:
    Positions&  positions_;
    Journal     journal_;
    double      max_order_usd_;
    double      max_deployed_usd_;
    double      max_per_window_usd_;
    double      min_order_usd_;
    double      clob_min_shares_fallback_;
    std::string clob_url_;
    std::string maker_addr_;
    std::string signer_addr_;
    std::string api_key_;
    std::string api_secret_;
    std::string api_passphrase_;
    std::string private_key_hex_;
    int         signature_type_;
    bool        neg_risk_;
    std::string builder_code_;
    // Per-token caches filled lazily from CLOB endpoints. mutable so the
    // const cold-start warm-up thread can populate them.
    mutable std::mutex                            min_order_mu_;
    mutable std::unordered_map<std::string, double>       min_order_size_cache_;
    mutable std::unordered_map<std::string, double>       tick_size_cache_;   // authoritative CLOB tick per token
    mutable std::unordered_map<std::string, bool>         neg_risk_cache_;
    // Venue taker fee rate per token (base_fee bps / 10000), fetched by the
    // cold-start warm-up. Fee accounting reads this cache when fresh and
    // falls back to the configured rate otherwise — the submit path never
    // blocks on I/O for it.
    mutable std::mutex                            fee_rate_mu_;
    mutable std::unordered_map<std::string, std::pair<int64_t,double>> fee_rate_cache_;  // token -> (fetched_ms, rate)
    // Taker fee actually charged: fresh venue rate when known, else config.
    double effective_taker_fee_rate(const OrderRequest& req) const;
    // Auth circuit breaker — tripped on "maker address not allowed"; blocks all
    // submissions until the caller resets it (e.g. on credential rotation).
    std::atomic<bool>                             auth_circuit_open_{false};
    // ── Phase 1.2: error classification at the CLOB boundary ──
    OrderResult::RejectClass classify_reject(int status, const std::string& body) const;
    std::string build_reject_detail(OrderResult::RejectClass cls, int status,
                                    double balance_usdc,
                                    const std::string& token_prefix,
                                    double exec_price, double tick,
                                    bool neg_risk, int sig_type) const;
    // ── Phase 1.1: signature-type probe-and-select + auto-heal ──
    double probe_balance_for_sigtype(int sig_type) const;
    int    probe_best_signature_type() const;
    bool   refresh_signature_type();
    // ── Phase 1.7: balance cache + local reservation ──
    // Hot-path read: cached value only, -1.0 when no fresh cache (gate skipped).
    double cached_balance_if_fresh_usd() const;
    // Cold-path read: refreshes from the CLOB when stale.
    double cached_balance_usd() const;
    void   reserve_usdc(double amount);
    void   release_reservation(double amount);
    double available_usdc() const;   // synchronous refresh + reservations
    // mutable: fetch_balance() is const but populates the cache.
    mutable std::mutex                            balance_mu_;
    mutable double                                cached_balance_usd_ = -1.0;
    mutable int64_t                               cached_balance_ts_ms_ = 0;
    double                                        reserved_usdc_ = 0.0;

    struct OpenOrder {
        OrderRequest req;
        std::string client_order_id;
        std::string exchange_order_id;
        double price = 0;
        double original_shares = 0;
        double matched_shares = 0;
        std::int64_t placed_ms = 0;
        std::int64_t last_poll_ms = 0;
        std::int64_t last_reprice_ms = 0;
        bool cancel_requested = false;
    };
    mutable std::mutex open_mu_;
    std::unordered_map<std::string, OpenOrder> open_orders_; // exchange id -> order

    bool cancel_order(const std::string& exchange_order_id) const;
};

} // namespace vp
