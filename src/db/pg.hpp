#pragma once
#include <pqxx/pqxx>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "core/types.hpp"

// libpqxx 7.10 explicitly instantiates these variable templates in its shared
// library.  Instantiating them again in an executable lets ELF interposition
// make two finalizers destroy the same std::string at process exit (Ubuntu's
// package even documents the underlying GCC/DSO failure in strconv.hxx).
// Use the library-owned instances for every type it exports.
namespace pqxx {
extern template std::string const type_name<bool>;
extern template std::string const type_name<short>;
extern template std::string const type_name<unsigned short>;
extern template std::string const type_name<int>;
extern template std::string const type_name<unsigned int>;
extern template std::string const type_name<long>;
extern template std::string const type_name<unsigned long>;
extern template std::string const type_name<long long>;
extern template std::string const type_name<unsigned long long>;
extern template std::string const type_name<float>;
extern template std::string const type_name<double>;
extern template std::string const type_name<long double>;
extern template std::string const type_name<std::string>;
extern template std::string const type_name<std::string_view>;
extern template std::string const type_name<std::shared_ptr<std::string>>;
} // namespace pqxx

namespace vp {
class Pg {
public:
    explicit Pg(const std::string& conninfo);
    pqxx::connection& conn() noexcept { return *conn_; }
    bool ping() noexcept;
    void exec_file(const std::string& path);
    long candles_count(const std::string& symbol);
    std::pair<std::string, std::string> candles_range(const std::string& symbol);
    double oracle_price_at_or_before(const std::string& asset,
                                     std::int64_t ts_ms) noexcept;

    std::string market_reference_key(const std::string& condition_id) noexcept;

    // Fetch the most-recent `limit` 1-min candles for a Binance symbol.
    // ordered by ts ASCENDING. Used by the engine to bootstrap technical-indicator
    // history. Returns empty vector on error (logged). Best-effort; non-throwing.
    struct CandleRow {
        std::int64_t ts_ms = 0;
        double open = 0, high = 0, low = 0, close = 0, volume = 0;
    };
    std::vector<CandleRow> load_recent_hourly_candles(const std::string& symbol, int hours) noexcept;
    std::vector<CandleRow> load_recent_candles(const std::string& symbol,
                                               int limit) noexcept;
    // Session context: stamped onto every vp.orders row so multiple bot
    // instances (different configs/strategies) can be compared in the DB.
    void set_session_context(const std::string& session_id,
                             const std::string& instance_label);
    // Conservative startup gate: unresolved ambiguous LIVE submissions block
    // autonomous trading until venue reconciliation is completed.
    bool has_uncertain_live_order_for_instance() noexcept;
    bool create_order_intent(const std::string& intent_id, const std::string& mode,
        const std::string& condition_id, const std::string& token_id,
        ShareSide side, OrderDir direction, const std::string& execution_mode,
        double size_usd, double size_shares, double limit_price,
        const std::string& source) noexcept;

    // ----- Insert helpers (best-effort; swallow exceptions, log) -----
    void insert_order(const std::string& client_order_id,
                      const std::string& exchange_order_id,
                      const std::string& execution_mode,
                      const std::string& mode,
                      const std::string& condition_id,
                      const std::string& token_id,
                      ShareSide side, OrderDir dir,
                      double price, double size_shares, double notional_usd,
                      const std::string& status,
                      const std::string& source,
                      const std::string& reject_reason,
                      double fill_price=0, double fill_shares=0, double fill_fee=0,
                      const std::string& manual_market_question="",
                      const std::string& outcome_label="") noexcept;
    void insert_order_reprice(const std::string& client_order_id,
                              const std::string& execution_mode,
                              double price, double remaining_shares) noexcept;
    void insert_fill(const std::string& client_order_id,
                     double price, double size_shares,
                     double fee_usd) noexcept;
    void insert_signal(const std::string& condition_id,
                       ShareSide side_label, OrderDir direction,
                       double confidence, const std::string& recommended,
                       double rec_size_usd, double rec_limit_price,
                       const std::string& reason,
                       const std::string& model_version = "",
                       const std::string& payload_json = "",
                       const std::string& client_order_id = "") noexcept;
    void insert_book_snapshot(const std::string& condition_id,
                              const std::string& token_id,
                              ShareSide side,
                              const std::vector<PriceLevel>& bids,
                              const std::vector<PriceLevel>& asks,
                              double best_bid, double best_ask) noexcept;
    void insert_pm_trade(const std::string& condition_id,
                         const std::string& token_id,
                         ShareSide side,
                         double price, double size,
                         std::int64_t exchange_ms,
                         std::optional<OrderDir> aggressor = std::nullopt) noexcept;
    void upsert_market(const std::string& condition_id,
                       const std::string& asset, int horizon_s,
                       double target_price,
                       std::int64_t window_open_ms,
                       std::int64_t window_close_ms,
                       const std::string& yes_token, const std::string& no_token,
                       const std::string& question, const std::string& slug,
                       int settlement_window_s = -1,
                       bool reference_is_binance = false) noexcept;
    void snapshot_positions(const std::string& mode, double cash_usd,
                            double equity_usd, double realized,
                            double unrealized, double deployed,
                            const std::string& open_positions_json) noexcept;
    // Session run accounting for vp.strategy_runs: create/refresh the row for
    // (instance, session_id); closed=true stamps ended_at. Idempotent per
    // session (unique index created in the Pg migrations). net_pnl/fees are
    // net-of-fee numbers matching vp.v_strategy_net_pnl.
    void upsert_strategy_run(const std::string& instance,
                             const std::string& session_id,
                             int fills, int settles, int wins, int losses,
                             double net_pnl, double fees, bool closed) noexcept;
    // Stamp ended_at on this instance's earlier open rows (left open when a
    // previous session was killed without a clean shutdown).
    void close_stale_strategy_runs(const std::string& instance,
                                   const std::string& keep_session_id) noexcept;

    // Research data collection helpers
    void insert_oracle_tick(const std::string& asset, double price,
                            std::int64_t ts_ms, const std::string& source = "chainlink_rtds") noexcept;
    void insert_binance_tick(const std::string& symbol, double price,
                             double qty, bool buyer_is_maker,
                             std::int64_t ts_ms) noexcept;
    void insert_coinbase_tick(const std::string& asset, double price,
                              std::int64_t ts_ms) noexcept;
    void insert_window_event(const std::string& condition_id,
                             const std::string& event_type,
                             const std::string& payload_json) noexcept;
    void insert_feature(const std::string& condition_id,
                        const std::string& payload_json,
                        int schema_ver = 1) noexcept;

    // Load this instance's fills from the fills table for startup restore.
    // Caller replays them into Positions; scoping is enforced with the session
    // context's instance label to avoid cross-instance phantom positions.
    struct RawFillRow {
        std::string condition_id;
        std::string token_id;
        std::string side_label;       // "up" / "down"
        std::string direction;        // "buy" / "sell"
        double price = 0;
        double shares = 0;
        double fee_usd = 0;
        std::string asset;            // "BTC" | "ETH" (from pm_markets join)
        int    horizon_s = 0;         // 300 / 900 / 3600
        int64_t window_close_ms = 0;  // epoch ms; 0 if market not found in DB
    };
    std::vector<RawFillRow> load_fills_for_session(const std::string& mode) noexcept;

    struct RawOpenOrderRow {
        std::string client_order_id;
        std::string exchange_order_id;
        std::string condition_id;
        std::string token_id;
        std::string side_label;
        std::string source;
        std::string asset;
        int horizon_s = 0;
        double price = 0;
        double original_shares = 0;
        double matched_shares = 0;
        std::int64_t window_close_ms = 0;
        std::int64_t placed_ms = 0;
    };
    std::vector<RawOpenOrderRow> load_open_orders_for_instance() noexcept;
private:
    void recover_disconnected(const char* operation,
                             const std::string& reason) noexcept;
    std::string conninfo_;
    std::unique_ptr<pqxx::connection> conn_;
    std::chrono::steady_clock::time_point last_reconnect_attempt_{};
    std::chrono::milliseconds reconnect_backoff_{1000};
    // A libpqxx connection permits only one active transaction. Most writes run
    // on the persistence thread, while settlement may perform synchronous
    // oracle lookups on the engine thread. Serialize both paths so a lookup
    // cannot make a queued settlement order/fill fail with
    // "Started new transaction while transaction was still active".
    mutable std::mutex conn_mu_;
    std::string session_id_;       // stamped on vp.orders rows ("" = NULL)
    std::string instance_label_;   // stamped on vp.orders rows ("" = NULL)
    bool order_events_enabled_ = false;
};
} // namespace vp
