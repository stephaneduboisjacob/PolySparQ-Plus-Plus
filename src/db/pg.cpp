#include "pg.hpp"
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
namespace vp {
namespace {
std::string side_to_pg(ShareSide s) { return s == ShareSide::UP ? "UP" : "DOWN"; }
std::string dir_to_pg (OrderDir d)  { return d == OrderDir::Buy ? "buy" : "sell"; }
// Normalise caller's mode string to lowercase DB enum value.
// Accepts "PAPER", "LIVE", "PAPER(!)", "paper", "live", "replay", etc.
std::string mode_to_pg(const std::string& m) {
    if (m.find("live") != std::string::npos || m.find("LIVE") != std::string::npos)
        return "live";
    if (m.find("replay") != std::string::npos || m.find("REPLAY") != std::string::npos)
        return "replay";
    return "paper";  // default
}
} // namespace
Pg::Pg(const std::string& conninfo)
    : conninfo_(conninfo), conn_(std::make_unique<pqxx::connection>(conninfo)) {
    spdlog::debug("postgres connected");
    try {
        pqxx::work tx{*conn_};
        tx.exec("SET search_path = vp, public");
        tx.commit();
    } catch (...) {}
    // Idempotent migration: per-instance/per-session tagging on orders so
    // parallel bot instances (different strategy configs) compare in SQL.
    try {
        pqxx::work tx{*conn_};
        tx.exec("ALTER TABLE vp.orders ADD COLUMN IF NOT EXISTS session_id uuid");
        tx.exec("ALTER TABLE vp.orders ADD COLUMN IF NOT EXISTS instance text");
        tx.exec("ALTER TABLE vp.orders ADD COLUMN IF NOT EXISTS execution_mode text NOT NULL DEFAULT 'taker'");
        tx.exec("ALTER TABLE vp.orders ADD COLUMN IF NOT EXISTS post_only boolean NOT NULL DEFAULT false");
        tx.exec("ALTER TABLE vp.orders ADD COLUMN IF NOT EXISTS manual_market_question text NOT NULL DEFAULT ''");
        tx.exec("ALTER TABLE vp.orders ADD COLUMN IF NOT EXISTS outcome_label text NOT NULL DEFAULT ''");
        tx.exec("CREATE UNIQUE INDEX IF NOT EXISTS strategy_runs_instance_session_idx "
                "ON vp.strategy_runs(instance, session_id)");
        tx.commit();
    } catch (const std::exception& e) {
        spdlog::warn("pg orders tag migration failed: {}", e.what());
    }
    try {
        pqxx::work tx{*conn_};
        tx.exec("ALTER TABLE vp.pm_markets ADD COLUMN IF NOT EXISTS reference_is_binance boolean NOT NULL DEFAULT false");
        tx.commit();
    } catch (const std::exception& e) {
        spdlog::warn("pg market reference-source migration failed: {}", e.what());
    }
    try {
        pqxx::work tx{*conn_};
        tx.exec("ALTER TYPE vp.order_status ADD VALUE IF NOT EXISTS 'uncertain'");
        tx.commit();
    } catch (const std::exception& e) {
        spdlog::warn("pg uncertain-order migration unavailable: {}", e.what());
    }
    try {
        pqxx::work tx{*conn_};
        tx.exec("CREATE TABLE IF NOT EXISTS vp.order_intents ("
                "intent_id uuid PRIMARY KEY, instance text NOT NULL, session_id uuid, "
                "mode text NOT NULL, condition_id text NOT NULL, token_id text NOT NULL, "
                "side text NOT NULL, direction text NOT NULL, execution_mode text NOT NULL, "
                "size_usd double precision, size_shares double precision, limit_price double precision, "
                "source text NOT NULL, state text NOT NULL DEFAULT 'INTENT', "
                "created_at timestamptz NOT NULL DEFAULT now(), updated_at timestamptz NOT NULL DEFAULT now())");
        tx.exec("CREATE INDEX IF NOT EXISTS order_intents_instance_state_idx ON vp.order_intents(instance,mode,state)");
        tx.commit();
    } catch (const std::exception& e) {
        spdlog::error("pg durable intent migration unavailable: {}", e.what());
    }
    try {
        pqxx::work tx{*conn_};
        tx.exec("CREATE TABLE IF NOT EXISTS vp.order_events ("
                "event_id bigserial PRIMARY KEY, schema_version integer NOT NULL DEFAULT 1, "
                "event_ts timestamptz NOT NULL DEFAULT now(), "
                "client_order_id uuid NOT NULL, instance text, session_id uuid, "
                "exchange_order_id text, execution_mode text NOT NULL, "
                "post_only boolean NOT NULL, status text NOT NULL, "
                "price double precision, size_shares double precision, "
                "fill_delta_shares double precision NOT NULL DEFAULT 0, "
                "reject_reason text)");
        tx.exec("CREATE INDEX IF NOT EXISTS order_events_client_time_idx "
                "ON vp.order_events(client_order_id, event_ts, event_id)");
        tx.exec("CREATE INDEX IF NOT EXISTS order_events_instance_time_idx "
                "ON vp.order_events(instance, event_ts DESC)");
        tx.commit();
        order_events_enabled_ = true;
    } catch (const std::exception& e) {
        spdlog::warn("pg order event migration unavailable: {}", e.what());
    }
    try {
        pqxx::work tx{*conn_};
        tx.exec("CREATE OR REPLACE VIEW vp.v_recycler_fill_markouts AS "
                "SELECT f.id AS fill_id,f.ts AS fill_ts,o.instance,o.session_id,o.condition_id,o.token_id,"
                "o.side_label::text AS side,o.direction::text AS direction,o.source,o.execution_mode,"
                "f.price AS fill_price,f.size_shares,f.fee_usd, "
                "b1.ts AS mark_1s_ts,b1.best_bid AS bid_1s,b1.midprice AS mid_1s,"
                "(b1.best_bid-f.price) AS bid_markout_1s, "
                "b5.ts AS mark_5s_ts,b5.best_bid AS bid_5s,b5.midprice AS mid_5s,"
                "(b5.best_bid-f.price) AS bid_markout_5s,ex.ts AS exit_ts,ex.price AS exit_price,ex.size_shares AS exit_shares "
                "FROM vp.fills f JOIN vp.orders o USING(client_order_id) "
                "LEFT JOIN LATERAL (SELECT ts,best_bid,midprice FROM vp.pm_book_snapshots s "
                "WHERE s.token_id=o.token_id AND s.ts>=f.ts+interval '1 second' "
                "AND s.ts<=f.ts+interval '3 seconds' ORDER BY s.ts LIMIT 1) b1 ON true "
                "LEFT JOIN LATERAL (SELECT ts,best_bid,midprice FROM vp.pm_book_snapshots s "
                "WHERE s.token_id=o.token_id AND s.ts>=f.ts+interval '5 seconds' "
                "AND s.ts<=f.ts+interval '8 seconds' ORDER BY s.ts LIMIT 1) b5 ON true "
                "LEFT JOIN LATERAL (SELECT fx.ts,fx.price,fx.size_shares FROM vp.fills fx "
                "JOIN vp.orders ox USING(client_order_id) WHERE ox.token_id=o.token_id "
                "AND ox.side_label=o.side_label AND ox.direction::text='sell' AND fx.ts>f.ts "
                "AND COALESCE(ox.instance,'')=COALESCE(o.instance,'') "
                "ORDER BY fx.ts LIMIT 1) ex ON true "
                "WHERE o.source LIKE 'RECYCLER_%' AND o.direction::text='buy'");
        tx.commit();
    } catch (const std::exception& e) {
        spdlog::warn("pg recycler markout view migration unavailable: {}", e.what());
    }
}

void Pg::recover_disconnected(const char* operation,
                              const std::string& reason) noexcept {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(conn_mu_);
    if (last_reconnect_attempt_ != std::chrono::steady_clock::time_point{} &&
        now - last_reconnect_attempt_ < reconnect_backoff_)
        return;

    last_reconnect_attempt_ = now;
    try {
        auto fresh = std::make_unique<pqxx::connection>(conninfo_);
        pqxx::work tx{*fresh};
        tx.exec("SET search_path = vp, public");
        tx.commit();
        conn_ = std::move(fresh);
        reconnect_backoff_ = std::chrono::milliseconds(1000);
        spdlog::warn("postgres connection recovered after {} failure: {}; the failed row was not retried",
                     operation, reason);
    } catch (const std::exception& e) {
        reconnect_backoff_ = std::min(reconnect_backoff_ * 2,
                                      std::chrono::milliseconds(30000));
        spdlog::error("postgres reconnect after {} failed (next retry in {}ms): {}",
                      operation, reconnect_backoff_.count(), e.what());
    }
}

void Pg::set_session_context(const std::string& session_id,
                             const std::string& instance_label) {
    std::lock_guard<std::mutex> lock(conn_mu_);
    session_id_     = session_id;
    instance_label_ = instance_label;
}
bool Pg::has_uncertain_live_order_for_instance() noexcept {
    try {
        std::lock_guard<std::mutex> lock(conn_mu_);
        pqxx::work tx{*conn_};
        auto row = tx.exec_params1(
            "SELECT EXISTS(SELECT 1 FROM vp.orders WHERE instance=$1 "
            "AND mode='live' AND status::text='uncertain') OR "
            "EXISTS(SELECT 1 FROM vp.order_intents WHERE instance=$1 AND mode='live' "
            "AND state IN ('INTENT','uncertain'))", instance_label_);
        tx.commit();
        return row[0].as<bool>();
    } catch (const pqxx::broken_connection& e) {
        recover_disconnected("has_uncertain_live_order_for_instance", e.what());
        spdlog::error("pg uncertain-order startup check failed: {}", e.what());
        return true; // fail closed for live startup if order state is unreadable
    } catch (const std::exception& e) {
        spdlog::error("pg uncertain-order startup check failed: {}", e.what());
        return true; // fail closed for live startup if order state is unreadable
    }
}
bool Pg::create_order_intent(const std::string& intent_id, const std::string& mode,
        const std::string& condition_id, const std::string& token_id,
        ShareSide side, OrderDir direction, const std::string& execution_mode,
        double size_usd, double size_shares, double limit_price,
        const std::string& source) noexcept {
    try {
        std::lock_guard<std::mutex> lock(conn_mu_);
        pqxx::work tx{*conn_};
        const auto rows = tx.exec_params(
            "INSERT INTO vp.order_intents(intent_id,instance,session_id,mode,condition_id,token_id,"
            "side,direction,execution_mode,size_usd,size_shares,limit_price,source) "
            "VALUES($1::uuid,$2,NULLIF($3,'')::uuid,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13) "
            "ON CONFLICT(intent_id) DO NOTHING RETURNING intent_id",
            intent_id,instance_label_,session_id_,mode_to_pg(mode),condition_id,token_id,
            side_to_pg(side),dir_to_pg(direction),execution_mode,size_usd,size_shares,limit_price,source);
        tx.commit();
        return !rows.empty();
    } catch (const pqxx::broken_connection& e) {
        recover_disconnected("create_order_intent", e.what());
        spdlog::error("pg intent reservation failed; submission blocked: {}", e.what());
        return false;
    } catch (const std::exception& e) {
        spdlog::error("pg intent reservation failed; submission blocked: {}",e.what());
        return false;
    }
}
bool Pg::ping() noexcept {
    try {
        std::lock_guard<std::mutex> lock(conn_mu_);
        pqxx::work tx{*conn_};
        auto r = tx.exec1("SELECT 1");
        tx.commit();
        return r[0].as<int>() == 1;
    } catch (const pqxx::broken_connection& e) {
        recover_disconnected("ping", e.what());
        spdlog::error("pg ping failed: {}", e.what());
        return false;
    } catch (const std::exception& e) {
        spdlog::error("pg ping failed: {}", e.what());
        return false;
    }
}
void Pg::exec_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open SQL file: " + path);
    std::stringstream ss; ss << f.rdbuf();
    std::lock_guard<std::mutex> lock(conn_mu_);
    pqxx::work tx{*conn_};
    tx.exec(ss.str());
    tx.commit();
}
long Pg::candles_count(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(conn_mu_);
    pqxx::work tx{*conn_};
    auto r = tx.exec_params1("SELECT count(*) FROM vp.candles_1m WHERE symbol = $1", symbol);
    return r[0].as<long>();
}
std::pair<std::string, std::string> Pg::candles_range(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(conn_mu_);
    pqxx::work tx{*conn_};
    auto r = tx.exec_params1(
        "SELECT COALESCE(MIN(ts)::text,''), COALESCE(MAX(ts)::text,'') "
        "FROM vp.candles_1m WHERE symbol = $1", symbol);
    return {r[0].as<std::string>(), r[1].as<std::string>()};
}

double Pg::oracle_price_at_or_before(const std::string& asset,
                                     std::int64_t ts_ms) noexcept {
    if (asset.empty() || ts_ms <= 0) return 0.0;
    try {
        std::lock_guard<std::mutex> lock(conn_mu_);
        pqxx::work tx{*conn_};
        auto rs = tx.exec_params(
            "SELECT price FROM vp.oracle_ticks "
            "WHERE asset = $1 AND ts <= to_timestamp($2 / 1000.0) "
            "AND ts >= to_timestamp($2 / 1000.0) - interval '10 seconds' "
            "ORDER BY ts DESC LIMIT 1",
            asset, (long long)ts_ms);
        tx.commit();
        if (rs.empty()) return 0.0;
        return rs[0][0].as<double>(0.0);
    } catch (const pqxx::broken_connection& e) {
        recover_disconnected("oracle_price_at_or_before", e.what());
        spdlog::warn("pg oracle_price_at_or_before({}, {}): {}", asset, ts_ms, e.what());
        return 0.0;
    } catch (const std::exception& e) {
        spdlog::warn("pg oracle_price_at_or_before({}, {}): {}", asset, ts_ms, e.what());
        return 0.0;
    }
}

std::vector<Pg::CandleRow> Pg::load_recent_candles(const std::string& symbol,
                                                    int limit) noexcept {
    std::vector<CandleRow> out;
    try {
        std::lock_guard<std::mutex> lock(conn_mu_);
        pqxx::work tx{*conn_};
        // Fetch most-recent `limit` rows (DESC), then we reverse below for ASC order.
        auto rs = tx.exec_params(
            "SELECT EXTRACT(EPOCH FROM ts)*1000.0, open, high, low, close, volume "
            "FROM vp.candles_1m WHERE symbol = $1 AND ts < date_trunc('minute', now()) "
            "ORDER BY ts DESC LIMIT $2",
            symbol, limit);
        out.reserve(rs.size());
        for (const auto& r : rs) {
            CandleRow c;
            c.ts_ms = (std::int64_t)r[0].as<double>();
            c.open  = r[1].as<double>();
            c.high  = r[2].as<double>();
            c.low   = r[3].as<double>();
            c.close = r[4].as<double>();
            c.volume= r[5].as<double>();
            out.push_back(c);
        }
        // Currently DESC; reverse to ASC so callers see chronological order.
        std::reverse(out.begin(), out.end());
    } catch (const pqxx::broken_connection& e) {
        recover_disconnected("load_recent_candles", e.what());
        spdlog::warn("load_recent_candles({}): {}", symbol, e.what());
        out.clear();
    } catch (const std::exception& e) {
        spdlog::warn("load_recent_candles({}): {}", symbol, e.what());
        out.clear();
    }
    return out;
}
void Pg::insert_order(const std::string& cid, const std::string& exchange_order_id,
                      const std::string& execution_mode,
                      const std::string& mode,
                      const std::string& condition_id, const std::string& token_id,
                      ShareSide side, OrderDir dir,
                      double price, double size_shares, double notional_usd,
                      const std::string& status, const std::string& source,
                      const std::string& reject_reason,
                      double fill_price, double fill_shares, double fill_fee,
                      const std::string& manual_market_question,
                      const std::string& outcome_label) noexcept {
    try {
        std::lock_guard<std::mutex> lock(conn_mu_);
        pqxx::work tx{*conn_};
        tx.exec_params(
            "INSERT INTO vp.orders (client_order_id, exchange_order_id, execution_mode, post_only, mode, condition_id, token_id, "
            "side_label, direction, price, size_shares, notional_usd, status, "
            "source, reject_reason, ts_submitted, session_id, instance, manual_market_question, outcome_label) "
            "VALUES ($1::uuid, NULLIF($2,''), $3, $3 IN ('maker','hybrid'), $4::vp.trade_mode, $5, $6, $7::vp.share_side, "
            "$8::vp.order_side, $9, $10, $11, $12::vp.order_status, $13, $14, now(), "
            "$15::uuid, $16, $17, $18) "
            "ON CONFLICT (client_order_id) DO UPDATE SET "
            "exchange_order_id=COALESCE(NULLIF(EXCLUDED.exchange_order_id,''), vp.orders.exchange_order_id), "
            "status=EXCLUDED.status, price=EXCLUDED.price, "
            "size_shares=GREATEST(vp.orders.size_shares, EXCLUDED.size_shares), "
            "ts_terminal=CASE WHEN EXCLUDED.status IN ('filled','cancelled','rejected') "
            "THEN now() ELSE vp.orders.ts_terminal END, "
            "reject_reason=COALESCE(EXCLUDED.reject_reason, vp.orders.reject_reason), "
            "manual_market_question=COALESCE(NULLIF(EXCLUDED.manual_market_question,''),vp.orders.manual_market_question), "
            "outcome_label=COALESCE(NULLIF(EXCLUDED.outcome_label,''),vp.orders.outcome_label)",
            cid, exchange_order_id, execution_mode, mode_to_pg(mode), condition_id, token_id,
            side_to_pg(side), dir_to_pg(dir),
            price, size_shares, notional_usd, status, source,
            reject_reason.empty() ? std::optional<std::string>{} : std::optional<std::string>{reject_reason},
            session_id_.empty() ? std::optional<std::string>{} : std::optional<std::string>{session_id_},
            instance_label_.empty() ? std::optional<std::string>{} : std::optional<std::string>{instance_label_},
            manual_market_question, outcome_label);
        if (fill_shares > 0) {
            tx.exec_params("INSERT INTO vp.fills (ts,client_order_id,price,size_shares,fee_usd) "
                           "VALUES (now(),$1::uuid,$2,$3,$4)",cid,fill_price,fill_shares,fill_fee);
        }
        // Commit the intent outcome atomically with its order/fill projection.
        // Older callers without an intent remain compatible (zero rows updated).
        tx.exec_params("UPDATE vp.order_intents SET state=$2,updated_at=now() WHERE intent_id=$1::uuid",cid,status);
        if (order_events_enabled_) {
            try {
                pqxx::subtransaction event_tx{tx};
                event_tx.exec_params(
                    "INSERT INTO vp.order_events "
                    "(client_order_id, instance, session_id, exchange_order_id, "
                    "execution_mode, post_only, status, price, size_shares, "
                    "fill_delta_shares) "
                    "VALUES ($1::uuid, NULLIF($2,''), NULLIF($3,'')::uuid, "
                    "NULLIF($4,''), $5, $5 IN ('maker','hybrid'), $6, $7, $8, $9)",
                    cid, instance_label_, session_id_, exchange_order_id,
                    execution_mode, status, price, size_shares, fill_shares);
                event_tx.commit();
            } catch (const std::exception& e) {
                spdlog::warn("pg order event append unavailable: {}", e.what());
            }
        }
        tx.commit();
    } catch (const pqxx::broken_connection& e) {
        recover_disconnected("insert_order", e.what());
        spdlog::warn("pg insert_order failed: {}", e.what());
    } catch (const std::exception& e) {
        spdlog::warn("pg insert_order failed: {}", e.what());
    }
}
void Pg::insert_order_reprice(const std::string& cid,
                              const std::string& execution_mode,
                              double price, double remaining_shares) noexcept {
    if (!order_events_enabled_) return;
    try {
        std::lock_guard<std::mutex> lock(conn_mu_);
        pqxx::work tx{*conn_};
        tx.exec_params(
            "INSERT INTO vp.order_events "
            "(client_order_id, instance, session_id, exchange_order_id, "
            "execution_mode, post_only, status, price, size_shares, fill_delta_shares) "
            "VALUES ($1::uuid, NULLIF($2,''), NULLIF($3,'')::uuid, NULLIF($4,''), "
            "$5, $6, 'repriced', $7, $8, 0)",
            cid, instance_label_,
            session_id_.empty() ? std::optional<std::string>{} : std::optional<std::string>{session_id_},
            cid, execution_mode,
            execution_mode == "maker" || execution_mode == "hybrid",
            price, remaining_shares);
        tx.commit();
    } catch (const pqxx::broken_connection& e) {
        recover_disconnected("insert_order_reprice", e.what());
        spdlog::warn("pg insert_order_reprice failed: {}", e.what());
    } catch (const std::exception& e) {
        spdlog::warn("pg insert_order_reprice failed: {}", e.what());
    }
}
void Pg::insert_fill(const std::string& cid, double price,
                     double size_shares, double fee_usd) noexcept {
    try {
        std::lock_guard<std::mutex> lock(conn_mu_);
        pqxx::work tx{*conn_};
        tx.exec_params(
            "INSERT INTO vp.fills (ts, client_order_id, price, size_shares, fee_usd) "
            "VALUES (now(), $1::uuid, $2, $3, $4)",
            cid, price, size_shares, fee_usd);
        tx.commit();
    } catch (const pqxx::broken_connection& e) {
        recover_disconnected("insert_fill", e.what());
        spdlog::warn("pg insert_fill failed: {}", e.what());
    } catch (const std::exception& e) {
        spdlog::warn("pg insert_fill failed: {}", e.what());
    }
}
void Pg::insert_signal(const std::string& condition_id,
                       ShareSide side, OrderDir dir,
                       double conf, const std::string& rec,
                       double rec_size, double rec_limit,
                       const std::string& reason,
                       const std::string& model_version,
                       const std::string& payload_json,
                       const std::string& client_order_id) noexcept {
    try {
        std::lock_guard<std::mutex> lock(conn_mu_);
        pqxx::work tx{*conn_};
        tx.exec_params(
            "INSERT INTO vp.signals (ts, condition_id, side_label, direction, "
            "confidence, recommended, rec_size_usd, rec_limit_price, reason, "
            "model_version, payload) "
            "VALUES (now(), $1, $2::vp.share_side, $3::vp.order_side, "
            "$4, $5, $6, $7, $8, NULLIF($9,'')::text, "
            "COALESCE(NULLIF($10,'')::jsonb,'{}'::jsonb) || "
            "jsonb_build_object('instance',$11::text,'session_id',$12::text,"
            "'client_order_id',NULLIF($13::text,'')))",
            condition_id, side_to_pg(side), dir_to_pg(dir),
            conf, rec, rec_size, rec_limit, reason, model_version, payload_json,
            instance_label_, session_id_, client_order_id);
        tx.commit();
    } catch (const pqxx::broken_connection& e) {
        recover_disconnected("insert_signal", e.what());
        spdlog::warn("pg insert_signal failed: {}", e.what());
    } catch (const std::exception& e) {
        spdlog::warn("pg insert_signal failed: {}", e.what());
    }
}
void Pg::insert_book_snapshot(const std::string& cid, const std::string& token_id,
                              ShareSide side,
                              const std::vector<PriceLevel>& bids,
                              const std::vector<PriceLevel>& asks,
                              double best_bid, double best_ask) noexcept {
    try {
        std::lock_guard<std::mutex> lock(conn_mu_);
        // Build JSONB arrays for top-20 levels
        auto levels_json = [](const std::vector<PriceLevel>& v, int lim) {
            std::string s = "[";
            int n = std::min<int>(lim, (int)v.size());
            for (int i = 0; i < n; ++i) {
                if (i) s += ',';
                s += fmt::format("[{:.4f},{:.4f}]", v[i].price, v[i].size);
            }
            return s + "]";
        };
        std::string bids_j = levels_json(bids, 20);
        std::string asks_j = levels_json(asks, 20);
        double mid = (best_bid > 0 && best_ask > 0) ? 0.5*(best_bid+best_ask) : 0;
        double spread = (best_bid > 0 && best_ask > 0) ? (best_ask - best_bid) : 0;
        double bsz = bids.empty() ? 0 : bids[0].size;
        double asz = asks.empty() ? 0 : asks[0].size;
        double imb1 = (bsz + asz > 0) ? (bsz - asz) / (bsz + asz) : 0;
        pqxx::work tx{*conn_};
        tx.exec_params(
            "INSERT INTO vp.pm_book_snapshots "
            "(ts, condition_id, token_id, side_label, bids, asks, "
            " best_bid, best_ask, midprice, spread, imbalance_top1) "
            "VALUES (now(), $1, $2, $3::vp.share_side, $4::jsonb, $5::jsonb, "
            " $6, $7, $8, $9, $10)",
            cid, token_id, side_to_pg(side),
            bids_j, asks_j,
            best_bid > 0 ? std::optional<double>(best_bid) : std::optional<double>{},
            best_ask > 0 ? std::optional<double>(best_ask) : std::optional<double>{},
            mid > 0 ? std::optional<double>(mid) : std::optional<double>{},
            spread > 0 ? std::optional<double>(spread) : std::optional<double>{},
            std::optional<double>(imb1));
        tx.commit();
    } catch (const pqxx::broken_connection& e) {
        recover_disconnected("insert_book_snapshot", e.what());
        spdlog::warn("pg insert_book_snapshot failed: {}", e.what());
    } catch (const std::exception& e) {
        spdlog::warn("pg insert_book_snapshot failed: {}", e.what());
    }
}
void Pg::insert_pm_trade(const std::string& cid, const std::string& token_id,
                         ShareSide side, double price, double size,
                         std::int64_t exchange_ms,
                         std::optional<OrderDir> aggressor) noexcept {
    try {
        std::lock_guard<std::mutex> lock(conn_mu_);
        pqxx::work tx{*conn_};
        tx.exec_params(
            "INSERT INTO vp.pm_trades "
            "(ts, condition_id, token_id, side_label, price, size, aggressor) "
            "VALUES (to_timestamp($1/1000.0), $2, $3, $4::vp.share_side, $5, $6, "
            "NULLIF($7, '')::vp.order_side)",
            (long long)exchange_ms, cid, token_id, side_to_pg(side), price, size,
            aggressor ? dir_to_pg(*aggressor) : std::string{});
        tx.commit();
    } catch (const pqxx::broken_connection& e) {
        recover_disconnected("insert_pm_trade", e.what());
        spdlog::warn("pg insert_pm_trade failed: {}", e.what());
    } catch (const std::exception& e) {
        spdlog::warn("pg insert_pm_trade failed: {}", e.what());
    }
}
void Pg::upsert_market(const std::string& cid, const std::string& asset,
                       int horizon_s, double target_price,
                       std::int64_t open_ms, std::int64_t close_ms,
                       const std::string& yes_token, const std::string& no_token,
                       const std::string& question, const std::string& slug,
                       int settlement_window_s, bool reference_is_binance) noexcept {
    try {
        std::lock_guard<std::mutex> lock(conn_mu_);
        pqxx::work tx{*conn_};
        tx.exec_params(
            "INSERT INTO vp.pm_markets (condition_id, asset, horizon_seconds, "
            "target_price, window_open, window_close, yes_token_id, no_token_id, "
            "question, slug, settlement_window_s, reference_is_binance) "
            "VALUES ($1, $2, $3, $4, to_timestamp($5/1000.0), to_timestamp($6/1000.0), "
            "$7, $8, $9, $10, $11, $12) "
            "ON CONFLICT (condition_id) DO UPDATE SET "
            "  last_seen = now(), target_price = EXCLUDED.target_price, "
            "settlement_window_s = EXCLUDED.settlement_window_s, "
            "reference_is_binance = EXCLUDED.reference_is_binance",
            cid, asset, horizon_s, target_price,
            (long long)open_ms, (long long)close_ms,
            yes_token, no_token, question, slug, settlement_window_s,
            reference_is_binance);
        tx.commit();
    } catch (const pqxx::broken_connection& e) {
        recover_disconnected("upsert_market", e.what());
        spdlog::warn("pg upsert_market failed: {}", e.what());
    } catch (const std::exception& e) {
        spdlog::warn("pg upsert_market failed: {}", e.what());
    }
}
void Pg::snapshot_positions(const std::string& mode, double cash_usd,
                            double equity_usd, double realized, double unrealized,
                            double deployed, const std::string& open_json) noexcept {
    try {
        std::lock_guard<std::mutex> lock(conn_mu_);
        pqxx::work tx{*conn_};
        tx.exec_params(
            "INSERT INTO vp.positions_history (ts, mode, cash_usd, equity_usd, "
            "realized_pnl, unrealized_pnl, deployed_usd, open_positions, instance, session_id) "
            "VALUES (now(), $1::vp.trade_mode, $2, $3, $4, $5, $6, $7::jsonb, NULLIF($8,''),NULLIF($9,'')::uuid)",
            mode_to_pg(mode), cash_usd, equity_usd, realized, unrealized, deployed, open_json,
            instance_label_,session_id_);
        tx.commit();
    } catch (const pqxx::broken_connection& e) {
        recover_disconnected("snapshot_positions", e.what());
        spdlog::warn("pg snapshot_positions failed: {}", e.what());
    } catch (const std::exception& e) {
        spdlog::warn("pg snapshot_positions failed: {}", e.what());
    }
}

void Pg::upsert_strategy_run(const std::string& instance,
                             const std::string& session_id,
                             int fills, int settles, int wins, int losses,
                             double net_pnl, double fees, bool closed) noexcept {
    try {
        std::lock_guard<std::mutex> lock(conn_mu_);
        pqxx::work tx{*conn_};
        tx.exec_params(
            "INSERT INTO vp.strategy_runs "
            "  (instance, session_id, started_at, ended_at, fills, settles, "
            "   wins, losses, net_pnl, fees) "
            "VALUES (NULLIF($1,''), NULLIF($2,'')::uuid, now(), "
            "        CASE WHEN $7 THEN now() ELSE NULL END, "
            "        $3, $4, $5, $6, $8, $9) "
            "ON CONFLICT (instance, session_id) DO UPDATE SET "
            "  fills=EXCLUDED.fills, settles=EXCLUDED.settles, "
            "  wins=EXCLUDED.wins, losses=EXCLUDED.losses, "
            "  net_pnl=EXCLUDED.net_pnl, fees=EXCLUDED.fees, "
            "  ended_at=CASE WHEN $7 THEN now() ELSE strategy_runs.ended_at END",
            instance, session_id, fills, settles, wins, losses, closed,
            net_pnl, fees);
        tx.commit();
    } catch (const pqxx::broken_connection& e) {
        recover_disconnected("upsert_strategy_run", e.what());
        spdlog::warn("pg upsert_strategy_run failed: {}", e.what());
    } catch (const std::exception& e) {
        spdlog::warn("pg upsert_strategy_run failed: {}", e.what());
    }
}

void Pg::close_stale_strategy_runs(const std::string& instance,
                                   const std::string& keep_session_id) noexcept {
    try {
        std::lock_guard<std::mutex> lock(conn_mu_);
        pqxx::work tx{*conn_};
        tx.exec_params(
            "UPDATE vp.strategy_runs SET ended_at = now() "
            "WHERE instance = NULLIF($1,'') AND ended_at IS NULL "
            "  AND (session_id IS NULL OR session_id <> NULLIF($2,'')::uuid)",
            instance, keep_session_id);
        tx.commit();
    } catch (const pqxx::broken_connection& e) {
        recover_disconnected("close_stale_strategy_runs", e.what());
        spdlog::warn("pg close_stale_strategy_runs failed: {}", e.what());
    } catch (const std::exception& e) {
        spdlog::warn("pg close_stale_strategy_runs failed: {}", e.what());
    }
}

void Pg::insert_oracle_tick(const std::string& asset, double price,
                            std::int64_t ts_ms, const std::string& source) noexcept {
    try {
        std::lock_guard<std::mutex> lock(conn_mu_);
        pqxx::work tx{*conn_};
        tx.exec_params(
            "INSERT INTO vp.oracle_ticks (ts, asset, price, source) "
            "VALUES (to_timestamp($1/1000.0), $2, $3, $4)",
            (long long)ts_ms, asset, price, source);
        tx.commit();
    } catch (const pqxx::broken_connection& e) {
        recover_disconnected("insert_oracle_tick", e.what());
    } catch (const std::exception& e) {
        spdlog::warn("pg insert_oracle_tick failed: {}", e.what());
    }
}

void Pg::insert_binance_tick(const std::string& symbol, double price,
                             double qty, bool buyer_is_maker,
                             std::int64_t ts_ms) noexcept {
    try {
        std::lock_guard<std::mutex> lock(conn_mu_);
        pqxx::work tx{*conn_};
        std::string aggressor = buyer_is_maker ? "sell" : "buy";
        tx.exec_params(
            "INSERT INTO vp.binance_ticks "
            "(ts, symbol, price, qty, buyer_is_maker, aggressor_side) "
            "VALUES (to_timestamp($1/1000.0), $2, $3, $4, $5, "
            " $6::vp.order_side)",
            (long long)ts_ms, symbol, price, qty, buyer_is_maker, aggressor);
        tx.commit();
    } catch (const pqxx::broken_connection& e) {
        recover_disconnected("insert_binance_tick", e.what());
    } catch (const std::exception& e) {
        spdlog::warn("pg insert_binance_tick failed: {}", e.what());
    }
}

void Pg::insert_coinbase_tick(const std::string& asset, double price,
                              std::int64_t ts_ms) noexcept {
    try {
        std::lock_guard<std::mutex> lock(conn_mu_);
        pqxx::work tx{*conn_};
        tx.exec_params(
            "INSERT INTO vp.coinbase_ticks (ts, asset, price) "
            "VALUES (to_timestamp($1/1000.0), $2, $3)",
            (long long)ts_ms, asset, price);
        tx.commit();
    } catch (const pqxx::broken_connection& e) {
        recover_disconnected("insert_coinbase_tick", e.what());
    } catch (const std::exception& e) {
        spdlog::warn("pg insert_coinbase_tick failed: {}", e.what());
    }
}

void Pg::insert_window_event(const std::string& condition_id,
                             const std::string& event_type,
                             const std::string& payload_json) noexcept {
    try {
        std::lock_guard<std::mutex> lock(conn_mu_);
        pqxx::work tx{*conn_};
        tx.exec_params(
            "INSERT INTO vp.window_events (condition_id, event_type, payload) "
            "VALUES ($1, $2, $3::jsonb)",
            condition_id, event_type,
            payload_json.empty()
                ? std::optional<std::string>{}
                : std::optional<std::string>{payload_json});
        tx.commit();
    } catch (const pqxx::broken_connection& e) {
        recover_disconnected("insert_window_event", e.what());
        spdlog::warn("pg insert_window_event failed: {}", e.what());
    } catch (const std::exception& e) {
        spdlog::warn("pg insert_window_event failed: {}", e.what());
    }
}

void Pg::insert_feature(const std::string& condition_id,
                        const std::string& payload_json,
                        int schema_ver) noexcept {
    try {
        std::lock_guard<std::mutex> lock(conn_mu_);
        pqxx::work tx{*conn_};
        tx.exec_params(
            "INSERT INTO vp.features (ts, condition_id, schema_ver, payload) "
            "VALUES (now(), $1, $2, $3::jsonb)",
            condition_id, schema_ver, payload_json);
        tx.commit();
    } catch (const pqxx::broken_connection& e) {
        recover_disconnected("insert_feature", e.what());
        spdlog::warn("pg insert_feature failed: {}", e.what());
    } catch (const std::exception& e) {
        spdlog::warn("pg insert_feature failed: {}", e.what());
    }
}

std::vector<Pg::RawFillRow> Pg::load_fills_for_session(const std::string& mode) noexcept {
    std::vector<RawFillRow> out;
    try {
        std::lock_guard<std::mutex> lock(conn_mu_);
        pqxx::work tx{*conn_};
        // Replay only this bot instance's fill ledger. Live sessions must never
        // inherit historical fills from another instance or from legacy rows with
        // no instance label; that creates phantom positions and synthetic settles.
        auto res = tx.exec_params(
            "SELECT o.condition_id, o.token_id, o.side_label::text, o.direction::text,"
            "       f.price, f.size_shares, f.fee_usd,"
            "       COALESCE(m.asset,'') AS asset,"
            "       COALESCE(m.horizon_seconds,0) AS horizon_s,"
            "       COALESCE(EXTRACT(EPOCH FROM m.window_close)::bigint*1000,0) AS wc_ms"
            "  FROM vp.fills f"
            "  JOIN vp.orders o ON o.client_order_id = f.client_order_id"
            "  LEFT JOIN pm_markets m ON m.condition_id = o.condition_id"
            " WHERE o.mode = $1::vp.trade_mode"
            "   AND COALESCE(o.instance,'') = $2"
            " ORDER BY f.ts ASC",
            mode_to_pg(mode), instance_label_);
        for (const auto& row : res) {
            RawFillRow r;
            r.condition_id    = row[0].c_str();
            r.token_id        = row[1].c_str();
            r.side_label      = row[2].c_str();
            r.direction       = row[3].c_str();
            r.price           = row[4].as<double>(0.0);
            r.shares          = row[5].as<double>(0.0);
            r.fee_usd         = row[6].as<double>(0.0);
            r.asset           = row[7].c_str();
            r.horizon_s       = row[8].as<int>(0);
            r.window_close_ms = row[9].as<int64_t>(0);
            out.push_back(std::move(r));
        }
        tx.commit();
    } catch (const pqxx::broken_connection& e) {
        recover_disconnected("load_fills_for_session", e.what());
        spdlog::warn("pg load_fills_for_session failed: {}", e.what());
    } catch (const std::exception& e) {
        spdlog::warn("pg load_fills_for_session failed: {}", e.what());
    }
    return out;
}

std::vector<Pg::RawOpenOrderRow> Pg::load_open_orders_for_instance() noexcept {
    std::vector<RawOpenOrderRow> out;
    try {
        std::lock_guard<std::mutex> lock(conn_mu_);
        pqxx::work tx{*conn_};
        auto rows = tx.exec_params(
            "SELECT o.client_order_id::text, o.exchange_order_id, o.condition_id, o.token_id, "
            "o.side_label::text, o.source, COALESCE(m.asset,''), "
            "COALESCE(m.horizon_seconds,0), o.price, o.size_shares, "
            "COALESCE(SUM(f.size_shares),0), "
            "COALESCE(EXTRACT(EPOCH FROM m.window_close)*1000,0)::bigint, "
            "COALESCE(EXTRACT(EPOCH FROM o.ts_submitted)*1000,0)::bigint "
            "FROM vp.orders o LEFT JOIN vp.fills f ON f.client_order_id=o.client_order_id "
            "LEFT JOIN vp.pm_markets m ON m.condition_id=o.condition_id "
            "WHERE o.mode='live' AND COALESCE(o.instance,'')=$1 "
            "AND o.exchange_order_id IS NOT NULL "
            "AND o.status IN ('submitted','acked','partial','uncertain') "
            "GROUP BY o.client_order_id,m.asset,m.horizon_seconds,m.window_close "
            "ORDER BY o.ts_submitted",
            instance_label_);
        tx.commit();
        for (const auto& r : rows) {
            RawOpenOrderRow o;
            o.client_order_id = r[0].as<std::string>();
            o.exchange_order_id = r[1].as<std::string>();
            o.condition_id = r[2].as<std::string>();
            o.token_id = r[3].as<std::string>();
            o.side_label = r[4].as<std::string>();
            o.source = r[5].as<std::string>();
            o.asset = r[6].as<std::string>();
            o.horizon_s = r[7].as<int>(0);
            o.price = r[8].as<double>(0);
            o.original_shares = r[9].as<double>(0);
            o.matched_shares = r[10].as<double>(0);
            o.window_close_ms = r[11].as<long long>(0);
            o.placed_ms = r[12].as<long long>(0);
            out.push_back(std::move(o));
        }
    } catch (const pqxx::broken_connection& e) {
        recover_disconnected("load_open_orders_for_instance", e.what());
        spdlog::warn("load_open_orders_for_instance: {}", e.what());
        out.clear();
    } catch (const std::exception& e) {
        spdlog::warn("load_open_orders_for_instance: {}", e.what());
        out.clear();
    }
    return out;
}
std::optional<Pg::PredictionGateSnapshot> Pg::load_prediction_gate_snapshot(
        const std::string& condition_id) noexcept {
    try {
        std::lock_guard<std::mutex> lock(conn_mu_);
        pqxx::work tx{*conn_};
        const auto rows = tx.exec_params(
            "SELECT condition_id, asset, horizon_seconds, "
            "(extract(epoch FROM window_open)*1000)::bigint, "
            "(extract(epoch FROM window_close)*1000)::bigint, model_status, "
            "up_probability, model_version, model_basis, "
            "(extract(epoch FROM (clock_timestamp()-source_updated_at))*1000)::bigint "
            "FROM vp.prediction_gate_current WHERE condition_id=$1",
            condition_id);
        tx.commit();
        if (rows.empty()) return std::nullopt;
        const auto& row = rows.front();
        PredictionGateSnapshot snapshot;
        snapshot.condition_id = row[0].as<std::string>();
        snapshot.asset = row[1].as<std::string>();
        snapshot.horizon_s = row[2].as<int>();
        snapshot.window_open_ms = row[3].as<std::int64_t>();
        snapshot.window_close_ms = row[4].as<std::int64_t>();
        snapshot.model_status = row[5].as<std::string>();
        if (!row[6].is_null()) snapshot.up_probability = row[6].as<double>();
        snapshot.model_version = row[7].as<std::string>();
        snapshot.model_basis = row[8].as<std::string>();
        snapshot.age_ms = row[9].as<std::int64_t>();
        return snapshot;
    } catch (const pqxx::broken_connection& e) {
        recover_disconnected("load_prediction_gate_snapshot", e.what());
        spdlog::debug("prediction page gate snapshot unavailable: {}", e.what());
        return std::nullopt;
    } catch (const std::exception& e) {
        spdlog::debug("prediction page gate snapshot unavailable: {}", e.what());
        return std::nullopt;
    }
}

} // namespace vp

std::string vp::Pg::market_reference_key(const std::string& cid) noexcept {
    try {
        std::lock_guard<std::mutex> lock(conn_mu_);
        pqxx::work tx{*conn_};
        auto rs = tx.exec_params("SELECT asset, settlement_window_s, reference_is_binance FROM vp.pm_markets WHERE condition_id=$1", cid);
        if (rs.empty()) return {};
        int window = rs[0][1].as<int>(-1);
        if (window < 0) return {};
        if (rs[0][2].as<bool>(false)) return rs[0][0].as<std::string>();
        return rs[0][0].as<std::string>() + (window == 0 ? ":SPOT" : ":TWAP" + std::to_string(window));
    } catch (const pqxx::broken_connection& e) {
        recover_disconnected("market_reference_key", e.what());
        spdlog::warn("market reference lookup: {}", e.what());
        return {};
    } catch (const std::exception& e) {
        spdlog::warn("market reference lookup: {}", e.what());
        return {};
    }
}

std::vector<vp::Pg::CandleRow> vp::Pg::load_recent_hourly_candles(const std::string& symbol, int hours) noexcept {
    std::vector<CandleRow> out;
    try {
        std::lock_guard<std::mutex> lock(conn_mu_);
        pqxx::work tx{*conn_};
        auto rs = tx.exec_params(
            "SELECT EXTRACT(EPOCH FROM date_trunc('hour',ts))*1000, "
            "(array_agg(open ORDER BY ts))[1], max(high), min(low), "
            "(array_agg(close ORDER BY ts DESC))[1], sum(volume) "
            "FROM vp.candles_1m WHERE symbol=$1 "
            "AND ts >= date_trunc('hour',now()) - $2 * interval '1 hour' "
            "AND ts < date_trunc('hour',now()) "
            "GROUP BY date_trunc('hour',ts) HAVING count(*)=60 ORDER BY 1", symbol, hours);
        for (const auto& r : rs) out.push_back({(int64_t)r[0].as<double>(),
            r[1].as<double>(),r[2].as<double>(),r[3].as<double>(),r[4].as<double>(),r[5].as<double>()});
    } catch (const pqxx::broken_connection& e) {
        recover_disconnected("load_recent_hourly_candles", e.what());
        spdlog::warn("hourly candles: {}", e.what());
    } catch (const std::exception& e) { spdlog::warn("hourly candles: {}",e.what()); }
    return out;
}
