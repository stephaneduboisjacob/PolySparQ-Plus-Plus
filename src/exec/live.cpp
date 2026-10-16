#include "live.hpp"
#include "core/uuid.hpp"
#include "sign/eip712.hpp"
#include "sign/hmac_auth.hpp"
#include "net/http_client.hpp"
#include "core/clock.hpp"

#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <simdjson.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <thread>
#include <cstdint>

namespace vp {

LiveExecutor::LiveExecutor(Positions& pos, Journal j,
                           double max_order_usd, double max_deployed_usd,
                           double max_per_window_usd, double min_order_usd,
                           double clob_min_shares_fallback,
                           const std::string& clob_url,
                           const std::string& maker, const std::string& signer,
                           const std::string& api_key,
                           const std::string& api_secret,
                           const std::string& api_passphrase,
                           const std::string& private_key_hex,
                           int  signature_type,
                           bool neg_risk,
                           std::string builder_code)
    : positions_(pos), journal_(std::move(j)),
      max_order_usd_(max_order_usd), max_deployed_usd_(max_deployed_usd),
      max_per_window_usd_(max_per_window_usd), min_order_usd_(min_order_usd),
      clob_min_shares_fallback_(clob_min_shares_fallback),
      clob_url_(clob_url),
      maker_addr_(maker), signer_addr_(signer),
      api_key_(api_key), api_secret_(api_secret), api_passphrase_(api_passphrase),
      private_key_hex_(private_key_hex),
      signature_type_(signature_type), neg_risk_(neg_risk),
      builder_code_(std::move(builder_code)) {
    spdlog::info("live exec: sigType={} neg_risk={} maker={} signer={}",
                 signature_type_, neg_risk_, maker_addr_, signer_addr_);

    // Phase 1.1: probe-and-select at startup — pick the wallet type that
    // actually holds collateral. Heals the "authenticated but unfillable"
    // class of POLY_1271 rejects caused by a stale configured sigType
    // (TOML says proxy, funds live in the SAFE, etc.).
    int best = probe_best_signature_type();
    if (best >= 0 && best != signature_type_) {
        spdlog::warn("live: sigtype probe-and-select: configured {} -> using {} "
                     "(largest USDC balance)", signature_type_, best);
        signature_type_ = best;
    }
    // Prime the balance cache so the first in-flight buy gate is non-blocking.
    refresh_balance_cache();
}

// Query CLOB /fee-rate?token_id= — must match what the CLOB expects in the signed struct.
// Python SDK: get_fee_rate_bps() → GET /fee-rate?token_id=..., returns result["base_fee"].
// Returns 0 on error (safe fallback — worst case is signature mismatch, not a crash).
static uint64_t fetch_fee_rate_bps(const std::string& clob_url,
                                   const std::string& token_id) {
    std::string url = clob_url + "/fee-rate?token_id=" + token_id;
    auto resp = http::get(url, {}, 5000);
    if (resp.status != 200) {
        spdlog::warn("live: GET /fee-rate token={}… status={} — defaulting to 0",
                     token_id.substr(0, 12), resp.status);
        return 0;
    }
    try {
        simdjson::dom::parser parser;
        simdjson::padded_string padded(resp.body.data(), resp.body.size());
        simdjson::dom::element doc;
        if (parser.parse(padded).get(doc) == simdjson::SUCCESS) {
            int64_t v = 0;
            if (!doc["base_fee"].get(v)) return (uint64_t)v;
        }
    } catch (...) {}
    spdlog::warn("live: /fee-rate parse failed for token={}…", token_id.substr(0, 12));
    return 0;
}

// Query CLOB /neg-risk?token_id= — the authoritative source for exchange selection.
// Python SDK: get_neg_risk() → GET /neg-risk?token_id=...
// Returns true (neg-risk exchange) on any error so we fail safely.
static bool fetch_neg_risk(const std::string& clob_url,
                           const std::string& token_id) {
    std::string url = clob_url + "/neg-risk?token_id=" + token_id;
    auto resp = http::get(url, {}, 5000);
    if (resp.status != 200) {
        spdlog::warn("live: GET /neg-risk token={}… status={} — assuming neg_risk=true",
                     token_id.substr(0, 12), resp.status);
        return true;
    }
    try {
        simdjson::dom::parser parser;
        simdjson::padded_string padded(resp.body.data(), resp.body.size());
        simdjson::dom::element doc;
        if (parser.parse(padded).get(doc) == simdjson::SUCCESS) {
            bool v = true;
            if (!doc["neg_risk"].get(v)) return v;
        }
    } catch (...) {}
    spdlog::warn("live: /neg-risk parse failed for token={}…", token_id.substr(0, 12));
    return true;
}

static double fetch_min_order_size(const std::string& clob_url,
                                   const std::string& token_id) {
    std::string url = clob_url + "/book?token_id=" + token_id;
    auto resp = http::get(url, {}, 5000);
    if (resp.status != 200) {
        spdlog::warn("live: GET /book token={}… status={}",
                     token_id.substr(0, 12), resp.status);
        return 5.0; // CLOB common minimum; fail safely if metadata unavailable.
    }
    try {
        simdjson::dom::parser parser;
        simdjson::padded_string padded(resp.body.data(), resp.body.size());
        simdjson::dom::element doc;
        if (parser.parse(padded).get(doc) == simdjson::SUCCESS) {
            std::string_view sv;
            if (!doc["min_order_size"].get(sv)) return std::stod(std::string(sv));
            double v = 0;
            if (!doc["min_order_size"].get(v)) return v;
        }
    } catch (...) {}
    spdlog::warn("live: /book min_order_size parse failed");
    return 5.0;
}

// Authoritative per-token tick size from the CLOB. Gamma market metadata often
// carries a stale/wrong tick (e.g. 0.01 when the real tick is 0.001); building
// order amounts at the wrong tick rounds the price off-grid → the CLOB rejects
// "Invalid order payload". Returns 0 on failure (caller keeps its fallback).
static double fetch_tick_size(const std::string& clob_url,
                              const std::string& token_id) {
    std::string url = clob_url + "/tick-size?token_id=" + token_id;
    auto resp = http::get(url, {}, 5000);
    if (resp.status != 200) {
        spdlog::warn("live: GET /tick-size token={}… status={}",
                     token_id.substr(0, 12), resp.status);
        return 0.0;
    }
    try {
        simdjson::dom::parser parser;
        simdjson::padded_string padded(resp.body.data(), resp.body.size());
        simdjson::dom::element doc;
        if (parser.parse(padded).get(doc) == simdjson::SUCCESS) {
            double v = 0;
            if (!doc["minimum_tick_size"].get(v)) return v;
            std::string_view sv;
            if (!doc["minimum_tick_size"].get(sv)) return std::stod(std::string(sv));
        }
    } catch (...) {}
    spdlog::warn("live: /tick-size parse failed");
    return 0.0;
}

// ---------------------------------------------------------------------------
// Error classification — map CLOB response to a typed RejectClass + detail.
// ---------------------------------------------------------------------------

namespace {

// Lightweight case-insensitive substring search.
bool icontains(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return true;
    if (hay.size() < needle.size()) return false;
    auto it = std::search(
        hay.begin(), hay.end(),
        needle.begin(), needle.end(),
        [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
    return it != hay.end();
}

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(
        a.begin(), a.end(), b.begin(),
        [](char x, char y) {
            return std::tolower(static_cast<unsigned char>(x)) ==
                   std::tolower(static_cast<unsigned char>(y));
        });
}

} // namespace

OrderResult::RejectClass LiveExecutor::classify_reject(
    int status, const std::string& body) const {
    if (status == 0) return OrderResult::RejectClass::Transport;

    // 4xx family
    if (icontains(body, "maker address not allowed") || status == 401 || status == 403)
        return OrderResult::RejectClass::AuthFailure;
    if (icontains(body, "invalid signature") || icontains(body, "POLY_1271"))
        return OrderResult::RejectClass::InvalidSignature;
    if (icontains(body, "not enough balance") || icontains(body, "allowance"))
        return OrderResult::RejectClass::BalanceAllowance;
    if (icontains(body, "post-only"))
        return OrderResult::RejectClass::PostOnlyCross;
    if (icontains(body, "Invalid order payload"))
        return OrderResult::RejectClass::InvalidPayload;
    if (icontains(body, "min_order_size") || icontains(body, "minimum size"))
        return OrderResult::RejectClass::SizeBelowMinimum;
    if (icontains(body, "duplicate") || status == 429)
        return OrderResult::RejectClass::DuplicateOrRateLimit;
    if (status >= 400 && status < 500)
        return OrderResult::RejectClass::Unknown;

    // 5xx
    if (status >= 500)
        return OrderResult::RejectClass::ServerError;

    return OrderResult::RejectClass::Unknown;
}

// Build a compact JSON diagnostic payload for a rejected order.
std::string LiveExecutor::build_reject_detail(
    OrderResult::RejectClass cls, int status,
    double balance_usdc, const std::string& token_prefix,
    double exec_price, double tick, bool neg_risk, int sig_type) const {
    std::string cls_name;
    switch (cls) {
        case OrderResult::RejectClass::InvalidSignature: cls_name = "invalid_signature"; break;
        case OrderResult::RejectClass::BalanceAllowance:   cls_name = "balance_allowance"; break;
        case OrderResult::RejectClass::PostOnlyCross:      cls_name = "post_only_cross"; break;
        case OrderResult::RejectClass::Transport:          cls_name = "transport"; break;
        case OrderResult::RejectClass::AuthFailure:        cls_name = "auth_failure"; break;
        case OrderResult::RejectClass::InvalidPayload:     cls_name = "invalid_payload"; break;
        case OrderResult::RejectClass::ServerError:        cls_name = "server_error"; break;
        case OrderResult::RejectClass::NoBook:             cls_name = "no_book"; break;
        case OrderResult::RejectClass::SizeBelowMinimum:   cls_name = "size_below_minimum"; break;
        case OrderResult::RejectClass::OperatorBand:       cls_name = "operator_price_band"; break;
        case OrderResult::RejectClass::DuplicateOrRateLimit: cls_name = "duplicate_or_rate_limit"; break;
        default: cls_name = "unknown"; break;
    }
    return fmt::format(
        R"({{"class":"{}","status":{},"balance_usdc":{:.2f},)"
        R"("token":"{}","exec_price":{:.4f},"tick":{:.4f},"neg_risk":{},)"
        R"("sig_type":{}}})",
        cls_name, status, balance_usdc, token_prefix,
        exec_price, tick, neg_risk ? "true" : "false", sig_type);
}

// ---------------------------------------------------------------------------
// Phase 1.1 — signature-type probe-and-select + auto-heal
// ---------------------------------------------------------------------------

// Fetch USDC balance for a given signature_type. Returns -1.0 on failure.
double LiveExecutor::probe_balance_for_sigtype(int sig_type) const {
    auto hdrs = hmac_auth::build(signer_addr_, api_key_, api_secret_, api_passphrase_,
                                 "GET", "/balance-allowance");
    auto resp = http::get(clob_url_ +
        "/balance-allowance?asset_type=COLLATERAL"
        "&signature_type=" + std::to_string(sig_type), hdrs, 5000);
    if (resp.status != 200) return -1.0;
    try {
        simdjson::dom::parser parser;
        simdjson::padded_string padded(resp.body.data(), resp.body.size());
        simdjson::dom::element doc;
        if (parser.parse(padded).get(doc) == simdjson::SUCCESS) {
            std::string_view sv;
            if (!doc["balance"].get(sv)) return std::stod(std::string(sv)) / 1e6;
            double v = 0;
            if (!doc["balance"].get(v)) return v / 1e6;
        }
    } catch (...) {}
    return -1.0;
}

// Probe every V2 signature type (EOA, proxy, Safe, and POLY_1271 deposit wallet)
// and return the one with the largest pUSD balance.
// Returns -1 if all fail or balances are zero.
int LiveExecutor::probe_best_signature_type() const {
    double best_bal = -1.0;
    int best_type = -1;
    for (int t = 0; t <= 3; ++t) {
        double b = probe_balance_for_sigtype(t);
        spdlog::debug("live: sigtype probe t={} balance=${:.4f}", t, b);
        if (b > best_bal) { best_bal = b; best_type = t; }
    }
    if (best_type >= 0 && best_bal > 0.0) {
        spdlog::info("live: sigtype probe winner t={} balance=${:.4f}", best_type, best_bal);
    } else {
        spdlog::warn("live: sigtype probe found no usable balance (types 0/1/2/3 all empty)");
    }
    return best_type;
}

// Force a re-probe of the best signature type and update the cached value.
// Returns true if a different type was found and applied.
bool LiveExecutor::refresh_signature_type() {
    int best = probe_best_signature_type();
    if (best >= 0 && best != signature_type_) {
        spdlog::warn("live: sigtype auto-heal {} -> {} (prior had 0/empty balance)",
                     signature_type_, best);
        signature_type_ = best;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Phase 1.7 — balance cache with local reservation
// ---------------------------------------------------------------------------

// Hot-path read: cached value only. Returns -1.0 when no fresh cache — the
// caller (buy gate) then SKIPS the check instead of blocking on I/O.
double LiveExecutor::cached_balance_if_fresh_usd() const {
    std::lock_guard<std::mutex> lk(balance_mu_);
    if (cached_balance_ts_ms_ > 0 &&
        (Clock::wall_ns() / 1'000'000LL - cached_balance_ts_ms_) < 30'000)
        return cached_balance_usd_ - reserved_usdc_;
    return -1.0;
}

// Cold-path read: refreshes from the CLOB when stale (>30s old).
double LiveExecutor::cached_balance_usd() const {
    {
        std::lock_guard<std::mutex> lk(balance_mu_);
        if (cached_balance_ts_ms_ > 0 &&
            (Clock::wall_ns() / 1'000'000LL - cached_balance_ts_ms_) < 30'000)
            return cached_balance_usd_;
    }
    // fetch_balance() performs network I/O and updates the cache under
    // balance_mu_.  It must run outside this lock to avoid self-deadlock.
    double b = fetch_balance();
    if (b >= 0.0) return b;
    std::lock_guard<std::mutex> lk(balance_mu_);
    return cached_balance_usd_;
}

// Public explicit refresh (periodic wallet-refresh loop, startup probe).
double LiveExecutor::refresh_balance_cache() const {
    double b = fetch_balance();
    if (b >= 0.0) return b;
    std::lock_guard<std::mutex> lk(balance_mu_);
    return cached_balance_usd_;
}

// Phase 1.6 — cold-start warm-up: pre-fetch per-token authoritative values
// so the first submit is cache-hits only. Polite pacing (40ms) keeps us off
// any rate-limit; a background thread calls this once after discovery.
void LiveExecutor::warm_token_caches(const std::vector<std::string>& token_ids) const {
    int warmed = 0;
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    for (const auto& tok : token_ids) {
        if (tok.empty()) continue;
        double t = fetch_tick_size(clob_url_, tok);
        bool nr = fetch_neg_risk(clob_url_, tok);
        double msz = fetch_min_order_size(clob_url_, tok);
        // Venue fee schedule truth-check: fetch_fee_rate_bps returns base_fee
        // bps (0 can mean both "no fee" and "fetch failed" — the fetch logs
        // which). Cached so fill accounting can charge the venue rate while
        // fresh; the order path never blocks on this.
        const uint64_t fee_bps = fetch_fee_rate_bps(clob_url_, tok);
        {
            std::lock_guard<std::mutex> lk(min_order_mu_);
            if (t > 0) tick_size_cache_[tok] = t;
            neg_risk_cache_[tok] = nr;
            min_order_size_cache_[tok] =
                msz > 0 ? msz : std::max(0.0, clob_min_shares_fallback_);
        }
        {
            std::lock_guard<std::mutex> lk(fee_rate_mu_);
            fee_rate_cache_[tok] = {now_ms, double(fee_bps) / 10000.0};
        }
        spdlog::info("live: token {}… warm (tick={} neg_risk={} min_size={} base_fee={}bps)",
                     tok.substr(0, 12), t, nr, msz, fee_bps);
        ++warmed;
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    spdlog::info("live: cold-start warm-up complete ({} tokens cached: tick, neg_risk, min_size, base_fee)",
                 warmed);
}

double LiveExecutor::effective_taker_fee_rate(const OrderRequest& req) const {
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    {
        std::lock_guard<std::mutex> lk(fee_rate_mu_);
        auto it = fee_rate_cache_.find(req.token_id);
        if (it != fee_rate_cache_.end() &&
            now_ms - it->second.first < 15 * 60 * 1000)
            return it->second.second;
    }
    return req.fee_rate;
}

void LiveExecutor::reserve_usdc(double amount) {
    std::lock_guard<std::mutex> lk(balance_mu_);
    reserved_usdc_ += amount;
}

void LiveExecutor::release_reservation(double amount) {
    std::lock_guard<std::mutex> lk(balance_mu_);
    reserved_usdc_ = std::max(0.0, reserved_usdc_ - amount);
}

double LiveExecutor::available_usdc() const {
    double b = cached_balance_usd();
    if (b < 0.0) return b;
    std::lock_guard<std::mutex> lk(balance_mu_);
    return b - reserved_usdc_;
}


std::string LiveExecutor::post_heartbeat(const std::string& prev_heartbeat_id) const {
    // L2-authed POST /v1/heartbeats with body {"heartbeat_id":"..."} — maintains
    // session liveness so the CLOB doesn't cancel orders (see clob-client-v2).
    // The docs require using the most recent heartbeat_id returned by the server;
    // an empty string is correct for the first call.
    std::string body = fmt::format(R"({{"heartbeat_id":"{}"}})", prev_heartbeat_id);
    auto hdrs = hmac_auth::build(signer_addr_, api_key_, api_secret_, api_passphrase_,
                                 "POST", "/v1/heartbeats", body);
    auto resp = http::post(clob_url_ + "/v1/heartbeats", body, hdrs);
    if (resp.status == 200) {
        try {
            simdjson::dom::parser parser;
            simdjson::padded_string padded(resp.body.data(), resp.body.size());
            simdjson::dom::element doc;
            if (parser.parse(padded).get(doc) == simdjson::SUCCESS) {
                std::string_view sv;
                if (!doc["heartbeat_id"].get(sv))
                    return std::string(sv);
            }
        } catch (...) {}
    } else {
        spdlog::debug("live: heartbeat status={}", resp.status);
    }
    return {};
}

double LiveExecutor::fetch_balance() const {
    // GET /balance-allowance?asset_type=COLLATERAL — returns live pUSD balance.
    // Response: {"balance": "2104377", "allowances": {...}}
    // balance is in raw pUSD units (6 decimals); divide by 1e6 to get USD.
    auto hdrs = hmac_auth::build(signer_addr_, api_key_, api_secret_, api_passphrase_,
                                 "GET", "/balance-allowance");
    // signature_type MUST match the wallet type: for a proxy/1271 wallet (type 3)
    // the funds live in the proxy (funder) account, NOT the signer EOA. Omitting
    // this param defaults the CLOB to type 0 (EOA), which reads the empty signer
    // and reports $0 even though the proxy holds the collateral.
    auto resp = http::get(clob_url_ + "/balance-allowance?asset_type=COLLATERAL"
                          "&signature_type=" + std::to_string(signature_type_), hdrs, 5000);
    if (resp.status != 200) {
        spdlog::warn("live: GET /balance-allowance status={}", resp.status);
        return -1.0;
    }
    try {
        simdjson::dom::parser parser;
        simdjson::padded_string padded(resp.body.data(), resp.body.size());
        simdjson::dom::element doc;
        if (parser.parse(padded).get(doc) == simdjson::SUCCESS) {
            std::string_view sv;
            if (!doc["balance"].get(sv)) {
                // balance is in raw pUSD units (integer string, 6 decimals)
                double raw = std::stod(std::string(sv));
                double usd = raw / 1e6;
                spdlog::info("live: wallet balance = ${:.4f} (pUSD)", usd);
                // Populate the Phase 1.7 cache so the hot path never re-reads.
                std::lock_guard<std::mutex> lk(balance_mu_);
                cached_balance_usd_ = usd;
                cached_balance_ts_ms_ = Clock::wall_ns() / 1'000'000LL;
                return usd;
            }
        }
    } catch (...) {}
    spdlog::warn("live: /balance-allowance parse failed");
    return -1.0;
}

double LiveExecutor::fetch_conditional_balance(const std::string& token_id) const {
    // GET /balance-allowance?asset_type=CONDITIONAL&token_id=… — real shares held.
    // balance is in raw units (6 decimals); divide by 1e6 to get share count.
    auto hdrs = hmac_auth::build(signer_addr_, api_key_, api_secret_, api_passphrase_,
                                 "GET", "/balance-allowance");
    auto resp = http::get(clob_url_ +
        "/balance-allowance?asset_type=CONDITIONAL&token_id=" + token_id +
        "&signature_type=" + std::to_string(signature_type_), hdrs, 5000);
    if (resp.status != 200) {
        spdlog::warn("live: GET /balance-allowance(CONDITIONAL) status={}", resp.status);
        return -1.0;
    }
    try {
        simdjson::dom::parser parser;
        simdjson::padded_string padded(resp.body.data(), resp.body.size());
        simdjson::dom::element doc;
        if (parser.parse(padded).get(doc) == simdjson::SUCCESS) {
            std::string_view sv;
            if (!doc["balance"].get(sv))
                return std::stod(std::string(sv)) / 1e6;
            double v = 0;
            if (!doc["balance"].get(v)) return v / 1e6;
        }
    } catch (...) {}
    spdlog::warn("live: /balance-allowance(CONDITIONAL) parse failed");
    return -1.0;
}

OrderResult LiveExecutor::submit(
    const OrderRequest& req,
    const std::function<const OrderBook*(const std::string&, ShareSide)>& book_lookup) {

    OrderResult r;
    r.client_order_id = req.intent_id.empty() ? vp::core::make_uuid() : req.intent_id;
    if (!journal_.before_submit || !journal_.before_submit(req, r.client_order_id)) {
        r.status = "rejected";
        r.terminal = true;
        r.reject_reason = "durable intent unavailable or already reserved; no venue submission";
        r.reject_class = OrderResult::RejectClass::RiskCap;
        return r;
    }

    // Authoritative tick size (filled from the CLOB inside the build block below;
    // declared here so it is in scope for the whole function despite the gotos).
    double real_tick = req.tick_size > 0 ? req.tick_size : 0.01;
    const bool maker_order = req.execution_mode != ExecutionMode::Taker;

    // Phase 1.7: set true when a BUY reservation is placed (released at done:).
    bool reserved = false;
    int reject_http_status = 0;

    const bool authorized_stop_fak =
        req.stop_loss_fak_authorized &&
        (req.source == "CB_LEAD_SCALP_HARD_STOP" ||
         req.source == "LATE_SETTLEMENT_HARD_STOP") &&
        req.dir == OrderDir::Sell;
    if (req.execution_mode == ExecutionMode::Taker && !authorized_stop_fak) {
        r.reject_class = OrderResult::RejectClass::RiskCap;
        r.reject_reason = "taker execution disabled except for an authorized tick hard stop";
        goto done;
    }

    // Auth circuit breaker — trip blocks all submissions until credentials change.
    if (auth_circuit_open_.load()) {
        r.reject_class = OrderResult::RejectClass::AuthFailure;
        r.reject_reason = "auth circuit open (maker-not-allowed detected — check credentials)";
        r.reject_detail_json = R"({"class":"auth_failure","circuit":"open"})";
        spdlog::error("live: REJECT — auth circuit open, submission blocked");
        goto done;
    }

    // Risk checks (same as PaperExecutor)
    if (req.dir == OrderDir::Buy) {
        if (req.size_usd <= 0) {
            r.reject_class = OrderResult::RejectClass::Unknown;
            r.reject_reason = "buy size_usd <= 0";
            goto done;
        }
        if (req.size_usd + 1e-9 < min_order_usd_) {
            r.reject_class = OrderResult::RejectClass::SizeBelowMinimum;
            r.reject_reason = fmt::format("size ${:.2f} < min ${:.2f}",
                                          req.size_usd, min_order_usd_);
            goto done;
        }
        if (req.size_usd > max_order_usd_ + 1e-9) {
            r.reject_class = OrderResult::RejectClass::Unknown;
            r.reject_reason = fmt::format("size ${:.2f} > cap ${:.2f}",
                                          req.size_usd, max_order_usd_);
            goto done;
        }
        // Cached balance + local reservation gate (Phase 1.7). Cache-only:
        // no network in the hot path. When no fresh cache exists the gate is
        // skipped — the CLOB-side balance reject is classified and healed below.
        double avail = cached_balance_if_fresh_usd();
        if (avail >= 0.0 && avail < req.size_usd - 1e-9) {
            r.reject_class = OrderResult::RejectClass::BalanceAllowance;
            r.reject_detail_json = fmt::format(
                R"({{"class":"balance_allowance","cached_available":{:.2f},"required":{:.2f}}})",
                avail, req.size_usd);
            r.reject_reason = fmt::format("cached available ${:.2f} < required ${:.2f}",
                                          avail, req.size_usd);
            goto done;
        }
        double open_notional = 0.0;
        double open_in_window = 0.0;
        {
            std::lock_guard<std::mutex> lk(open_mu_);
            for (const auto& [_, o] : open_orders_) {
                const double n = o.price * std::max(0.0, o.original_shares - o.matched_shares);
                open_notional += n;
                if (o.req.condition_id == req.condition_id) open_in_window += n;
            }
        }
        double already = positions_.total_deployed_usd() + open_notional;
        if (already + req.size_usd > max_deployed_usd_ + 1e-9) {
            r.reject_class = OrderResult::RejectClass::Unknown;
            r.reject_reason = fmt::format("deployed ${:.2f} + ${:.2f} > cap ${:.2f}",
                                          already, req.size_usd, max_deployed_usd_);
            goto done;
        }
        double in_win = positions_.deployed_in_market(req.condition_id) + open_in_window;
        if (in_win + req.size_usd > max_per_window_usd_ + 1e-9) {
            r.reject_class = OrderResult::RejectClass::Unknown;
            r.reject_reason = fmt::format("window ${:.2f} + ${:.2f} > cap ${:.2f}",
                                          in_win, req.size_usd, max_per_window_usd_);
            goto done;
        }
    }

    {
        const OrderBook* book = book_lookup(req.condition_id, req.side);
        if (!book) {
            r.reject_class = OrderResult::RejectClass::NoBook;
            r.reject_reason = "no book for market/side";
            goto done;
        }

        // Resolve the AUTHORITATIVE tick size from the CLOB (cached per token).
        // This is the fix for the "Invalid order payload" rejects: the gamma tick
        // (req.tick_size) is frequently wrong, so amounts rounded to it land the
        // price off-grid. We round the price to THIS tick before signing.
        {
            std::lock_guard<std::mutex> lk(min_order_mu_);
            auto it = tick_size_cache_.find(req.token_id);
            if (it != tick_size_cache_.end()) {
                real_tick = it->second;
            } else {
                double t = fetch_tick_size(clob_url_, req.token_id);
                if (t > 0) { tick_size_cache_[req.token_id] = t; real_tick = t; }
            }
        }
        if (real_tick <= 0) real_tick = req.tick_size > 0 ? req.tick_size : 0.01;

        // Determine exec price and size
        double exec_price = 0;
        double exec_shares = 0;

        if (req.dir == OrderDir::Buy) {
            auto a = book->best_ask();
            if (a.price <= 0 || a.size <= 0) {
                r.reject_class = OrderResult::RejectClass::NoBook;
                r.reject_reason = "no asks on book";
                goto done;
            }
            // A taker signal is stale when the ask passed its cap. A maker can
            // still quote below that cap, so it does not require an executable ask.
            if (!maker_order && req.limit_price > 0 && a.price > req.limit_price) {
                r.reject_class = OrderResult::RejectClass::PostOnlyCross;
                r.reject_reason = fmt::format("ask {:.3f} > limit {:.3f}",
                                              a.price, req.limit_price);
                goto done;
            }
            if (maker_order) {
                const auto b = book->best_bid();
                exec_price = b.price > 0
                    ? b.price + std::max(0, req.maker_improve_ticks) * real_tick
                    : a.price - real_tick;
                // Guarantee passivity locally; postOnly=true is the venue-side
                // invariant in case the book moves between this read and POST.
                exec_price = std::min(exec_price, a.price - real_tick);
                if (req.limit_price > 0) exec_price = std::min(exec_price, req.limit_price);
                exec_price = std::floor(exec_price / real_tick + 1e-9) * real_tick;
                if (exec_price <= 0 || exec_price >= a.price - 1e-9) {
                    r.reject_class = OrderResult::RejectClass::PostOnlyCross;
                    r.reject_reason = "no passive maker price available";
                    goto done;
                }
                if (req.maker_min_price > 0.0 &&
                    exec_price + 1e-9 < req.maker_min_price) {
                    r.reject_reason = fmt::format(
                        "maker quote {:.3f} below required minimum {:.3f}",
                        exec_price, req.maker_min_price);
                    goto done;
                }
            } else {
            // Marketable FAK: price a couple ticks THROUGH the best ask so the
            // order actually crosses if the book moved a tick since the signal.
            // A FAK priced exactly at best_ask misses the moment the top level
            // is taken or shifts up — this is the #1 cause of "no orders found
            // to match". Capped at the limit and just below 1.0 (CLOB rejects
            // any price that rounds to 1.0).
            double ts = real_tick;
            const double cross_ticks =
                std::isfinite(req.entry_cross_ticks)
                    ? std::max(0.0, req.entry_cross_ticks)
                    : 0.0;
            exec_price = a.price + cross_ticks * ts;
            if (req.limit_price > 0) exec_price = std::min(exec_price, req.limit_price);
            exec_price = std::min(exec_price, 1.0 - ts);
            // Snap to the tick grid so the signed maker/taker imply an on-grid price.
            exec_price = std::round(exec_price / ts) * ts;
            }

            // ── Operator-installed entry band (Phase 1.5) ────────────────
            // Defense-in-depth: compile-time constants enforced at the submit
            // boundary. NO TOML value, hot-reload, or strategy config can widen
            // this band. A misconfigured hard_share_price_ceil=0.999 or a
            // min_entry_share_price=0.001 can never reach the wire.
            constexpr double kOpMinEntryPrice = 0.03;
            constexpr double kOpMaxEntryPrice = 0.97;
            if (exec_price < kOpMinEntryPrice || exec_price > kOpMaxEntryPrice) {
                r.reject_class = OrderResult::RejectClass::OperatorBand;
                r.reject_reason = fmt::format(
                    "operator band: exec_price {:.4f} outside [{:.2f},{:.2f}] (not config-visible)",
                    exec_price, kOpMinEntryPrice, kOpMaxEntryPrice);
                goto done;
            }
        } else { // SELL
            // Verify we actually hold the shares on-chain and reconcile the tracker
            // FIRST — before the no-bids check. The tracker can desync (settled at
            // window close, sold manually on the web, never really filled), and a
            // phantom position must be DROPPED here (shares→0) or it re-fires an exit
            // every sweep and only ever hits "no bids on book"/"Invalid order payload",
            // spinning forever. Reconciling to the chain is what makes the spin stop.
            double held = 0;
            for (auto& p : positions_.snapshot())
                if (p.condition_id == req.condition_id && p.side == req.side)
                    { held = p.shares; break; }
            double on_chain = fetch_conditional_balance(req.token_id);
            if (on_chain >= 0.0 && on_chain < held - 1e-6) {
                double prior = positions_.reconcile_shares(req.condition_id, req.side, on_chain);
                spdlog::warn("live: SELL reconcile {} {} tracker={:.4f}sh -> on-chain={:.4f}sh "
                             "(position desync corrected)",
                             req.asset, to_string(req.side), prior, on_chain);
                held = on_chain;
            }
            double want = req.size_shares > 0 ? req.size_shares : held;
            exec_shares = std::min(want, held);
            if (maker_order) {
                double already_reserved = 0.0;
                std::lock_guard<std::mutex> lk(open_mu_);
                for (const auto& [_, order] : open_orders_) {
                    if (order.req.dir == OrderDir::Sell &&
                        order.req.condition_id == req.condition_id &&
                        order.req.side == req.side) {
                        already_reserved += std::max(
                            0.0, order.original_shares - order.matched_shares);
                    }
                }
                exec_shares = std::min(exec_shares,
                    std::max(0.0, held - already_reserved));
            }
            if (exec_shares <= 0) {
                r.reject_class = OrderResult::RejectClass::BalanceAllowance;
                r.reject_reason = "no shares held (reconciled to 0)";
                goto done;
            }
            // A maker sell joins the ask side and must remain strictly above
            // the bid. A taker sell instead crosses the best bid immediately.
            const auto b = book->best_bid();
            const auto a = book->best_ask();
            if (b.price <= 0 || (maker_order && a.price <= 0)) {
                r.reject_class = OrderResult::RejectClass::NoBook;
                r.reject_reason = maker_order ? "no two-sided book for maker sell"
                                              : "no bids on book";
                goto done;
            }
            double ts = real_tick;
            if (maker_order) {
                if (req.maker_fixed_price && req.limit_price > 0) {
                    exec_price = req.limit_price;
                } else {
                    exec_price = a.price -
                        std::max(0, req.maker_improve_ticks) * ts;
                    exec_price = std::max(exec_price, b.price + ts);
                    if (req.limit_price > 0)
                        exec_price = std::max(exec_price, req.limit_price);
                }
                exec_price = std::ceil(exec_price / ts - 1e-9) * ts;
                exec_price = std::min(exec_price, 1.0 - ts);
                if (exec_price <= b.price + 1e-9) {
                    r.reject_class = OrderResult::RejectClass::PostOnlyCross;
                    r.reject_reason = "maker sell price is not above the best bid";
                    goto done;
                }
            } else {
                // Price the FAK a few ticks through the bid so a one-tick book
                // move between the local read and POST does not strand an exit.
                // A caller's limit_price remains a hard minimum sale price.
                const double cross_ticks = std::isfinite(req.entry_cross_ticks)
                    ? std::max(0.0, req.entry_cross_ticks) : 0.0;
                exec_price = std::min(b.price, 1.0 - ts) - cross_ticks * ts;
                exec_price = std::max(ts, exec_price);
                exec_price = std::floor(exec_price / ts + 1e-9) * ts;
                if (req.limit_price > 0 && exec_price + 1e-9 < req.limit_price) {
                    r.reject_class = OrderResult::RejectClass::PostOnlyCross;
                    r.reject_reason = fmt::format("bid {:.3f} < sell minimum {:.3f}",
                                                  exec_price, req.limit_price);
                    goto done;
                }
            }
        }

        // Build EIP-712 order
        // Replacements pin their unfilled share remainder in size_shares so a
        // changing quote price cannot gradually grow or shrink exposure.
        double maker_requested_shares = maker_order
            ? (req.size_shares > 0
                ? std::floor(req.size_shares * 100.0) / 100.0
                : std::floor((req.size_usd / exec_price) * 100.0) / 100.0)
            : 0.0;
        auto amts = (req.dir == OrderDir::Buy)
            ? (maker_order
                ? eip712::calc_amounts(0, exec_price, maker_requested_shares, real_tick)
                : eip712::calc_buy_market_amounts(exec_price, req.size_usd, real_tick))
            : (maker_order
                ? eip712::calc_amounts(1, exec_price,
                    std::floor(exec_shares * 100.0) / 100.0, real_tick)
                : eip712::calc_sell_market_amounts(exec_price, exec_shares, real_tick));
        if (amts.maker_amount == 0 || amts.taker_amount == 0) {
            r.reject_class = OrderResult::RejectClass::InvalidPayload;
            r.reject_reason = "rounded order amount is zero";
            goto done;
        }
        double min_order_size = std::max(0.0, clob_min_shares_fallback_);
        {
            std::lock_guard<std::mutex> lk(min_order_mu_);
            auto it = min_order_size_cache_.find(req.token_id);
            if (it == min_order_size_cache_.end()) {
                min_order_size = fetch_min_order_size(clob_url_, req.token_id);
                if (min_order_size <= 0) min_order_size = std::max(0.0, clob_min_shares_fallback_);
                min_order_size_cache_[req.token_id] = min_order_size;
                spdlog::info("live: min_order_size token={}… = {:.4f} shares",
                             req.token_id.substr(0, 12), min_order_size);
            } else {
                min_order_size = it->second;
            }
        }
        if (req.dir == OrderDir::Buy) {
            exec_shares = (double)amts.taker_amount / 1e6;
            if (exec_shares <= 0) {
                r.reject_reason = fmt::format("size ${:.2f} @ {:.3f} = 0 shares after rounding",
                                              req.size_usd, exec_price);
                goto done;
            }
            if (exec_shares + 1e-9 < min_order_size) {
                double min_usd = min_order_size * exec_price;
                // Try to bump to min_order_size if within the order cap
                double needed_usd = min_order_size * exec_price;
                if (needed_usd <= std::min(req.size_usd,max_order_usd_) + 1e-9) {
                    exec_shares = min_order_size;
                    amts = maker_order
                        ? eip712::calc_amounts(0, exec_price, exec_shares, real_tick)
                        : eip712::calc_buy_market_amounts(exec_price,
                              exec_shares * exec_price, real_tick);
                } else {
                    r.reject_class = OrderResult::RejectClass::SizeBelowMinimum;
                    r.reject_reason = fmt::format(
                        "CLOB min {:.2f} shares: ${:.2f} @ {:.3f} buys {:.4f}sh; need ${:.2f} > cap ${:.2f}",
                        min_order_size, req.size_usd, exec_price, exec_shares,
                        needed_usd, max_order_usd_);
                    goto done;
                }
            }
        } else {
            double sell_shares = (double)amts.maker_amount / 1e6;
            if (sell_shares + 1e-9 < min_order_size) {
                // On a sell-all (size_shares<=0), sell_shares == the entire
                // on-chain holding (already capped above). If that's below the
                // CLOB minimum sellable size, the position can NEVER be sold via
                // the API — it's stuck dust (a partial manual sell remainder,
                // settlement dust, etc.). Leaving it in the tracker makes the
                // exit logic retry forever: a 1–5 share remainder clears the
                // exit guards' 1-share floor but fails this 5-share sell minimum,
                // so STOP_LOSS spams a network balance-fetch + reject every cycle.
                // Force-close it so the exits stop firing on unsellable dust.
                // Unsellable shares remain owned and can still settle. A
                // rejected sale is not a fill or permission to erase holdings.
                r.reject_class = OrderResult::RejectClass::SizeBelowMinimum;
                r.reject_reason = fmt::format(
                    "CLOB min {:.2f} shares: sell size {:.4f}sh too small (holding retained)",
                    min_order_size, sell_shares);
                goto done;
            }
        }
        if (req.dir == OrderDir::Buy) {
            const double actual_notional = double(amts.maker_amount)/1e6;
            if (actual_notional > std::min(req.size_usd,max_order_usd_)+1e-6) {
                r.reject_reason="rounded/repriced buy exceeds requested dollar budget";
                goto done;
            }
            const double fee = maker_order ? 0 : effective_taker_fee_rate(req) *
                std::pow(exec_price*(1-exec_price),req.fee_exponent)*exec_shares;
            const double available = cached_balance_if_fresh_usd();
            if (available >= 0 && actual_notional+fee > available+1e-6) {
                r.reject_class=OrderResult::RejectClass::BalanceAllowance;
                r.reject_reason="buy plus fee exceeds cached available balance";
                goto done;
            }
        }
        // Authoritative neg_risk: fetch from CLOB /neg-risk once per token and cache.
        // The Gamma discovery value can default to false when the field is absent,
        // causing the wrong exchange contract to be used in the EIP-712 domain
        // separator → "Invalid order payload".
        bool token_neg_risk = req.neg_risk;
        {
            std::lock_guard<std::mutex> lk(min_order_mu_);
            auto nit = neg_risk_cache_.find(req.token_id);
            if (nit == neg_risk_cache_.end()) {
                token_neg_risk = fetch_neg_risk(clob_url_, req.token_id);
                neg_risk_cache_[req.token_id] = token_neg_risk;
                spdlog::info("live: neg_risk token={}… = {} (fetched from CLOB)",
                             req.token_id.substr(0, 12), token_neg_risk);
            } else {
                token_neg_risk = nit->second;
            }
        }

        eip712::OrderParams op;
        op.maker        = maker_addr_;
        // The order `signer` field must equal `maker` (the proxy/funder), NOT the
        // EOA. Verified against @polymarket/clob-client-v2: a type-3 (POLY_1271)
        // order signs with maker == signer == proxy address, while the EOA only
        // appears as the HMAC POLY_ADDRESS (signer_addr_) and as the ECDSA key.
        // Setting this to signer_addr_ (the EOA) yielded "Invalid order payload".
        op.signer       = maker_addr_;
        op.salt         = eip712::make_salt();
        op.token_id     = req.token_id;
        op.maker_amount = amts.maker_amount;
        op.taker_amount = amts.taker_amount;
        op.expiration   = 0;
        op.timestamp    = (uint64_t)(std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch()).count());
        op.side         = (req.dir == OrderDir::Buy) ? 0 : 1;
        op.sig_type     = signature_type_;
        op.neg_risk     = token_neg_risk;
        op.builder_code = builder_code_;

        std::string order_json = eip712::sign_order(op, private_key_hex_);
        if (order_json.empty()) {
            r.reject_class = OrderResult::RejectClass::InvalidSignature;
            r.reject_reason = "EIP-712 signing failed";
            goto done;
        }

        const char* wire_type = maker_order ? "GTC" : "FAK";
        std::string body = fmt::format(
            R"({{"deferExec":false,"postOnly":{},"order":{},"owner":"{}","orderType":"{}"}})",
            maker_order ? "true" : "false", order_json, api_key_, wire_type);

        // Log only non-sensitive pricing metadata. The signed request includes
        // the venue API owner and signature, so it must never enter a log.
        spdlog::debug("live: order meta token={}… tick={:.4f} negRisk={} sigType={} ask={:.4f}",
                  req.token_id.substr(0, 12), real_tick, token_neg_risk,
                  signature_type_, exec_price);

        // ── Reserve notional while in flight (Phase 1.7) ──
        // Held against the cached balance so parallel submissions cannot
        // overspend it. Released at done: on every outcome.
        if (req.dir == OrderDir::Buy) {
            reserve_usdc(req.size_usd);
            reserved = true;
        }

        // POST to CLOB — classify-then-act retry loop (Phase 1.2 + 1.1):
        //   • invalid signature (POLY_1271) → refresh signature_type, retry once
        //     (signature validation happens pre-match: nothing executed)
        //   • BUY balance/allowance → drop stale USDC cache, retry once
        //   • 5xx signer-validation hiccup → backoff retry (provably pre-match)
        //   • any other 5xx/transport → NEVER retried (the venue may already
        //     have accepted the order, including a SELL)
        //   • auth failure → trip the circuit breaker, no retry
        std::string url = clob_url_ + "/order";
        http::Response resp;
        bool sig_healed = false;
        bool bal_refreshed = false;
        for (int attempt = 0; attempt < 4; ++attempt) {
            if (attempt > 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(std::min(250 * (1 << (attempt - 1)), 2000)));
                spdlog::warn("live: retry POST /order (attempt {}/3) after status {}",
                             attempt, resp.status);
            }
            // Re-sign HMAC each attempt (POLY_TIMESTAMP is time-based).
            auto hdrs = hmac_auth::build(
                signer_addr_, api_key_, api_secret_, api_passphrase_,
                "POST", "/order", body);
            resp = http::post(url, body, hdrs);
            if (!resp.error.empty()) break;   // transport error — handled below
            if (resp.status == 200) break;
            if (resp.status < 500) {
                auto cls = classify_reject(resp.status, resp.body);
                if (cls == OrderResult::RejectClass::InvalidSignature && !sig_healed) {
                    sig_healed = true;
                    if (refresh_signature_type()) {
                        op.sig_type = signature_type_;
                        order_json = eip712::sign_order(op, private_key_hex_);
                        if (!order_json.empty()) {
                            body = fmt::format(
                                R"({{"deferExec":false,"postOnly":{},"order":{},"owner":"{}","orderType":"{}"}})",
                                maker_order ? "true" : "false", order_json, api_key_, wire_type);
                            continue;  // re-send the healed order
                        }
                    }
                    break;
                }
                if (cls == OrderResult::RejectClass::BalanceAllowance &&
                    req.dir == OrderDir::Buy && !bal_refreshed) {
                    // Rejected pre-match: stale USDC cache. Drop it, retry once.
                    bal_refreshed = true;
                    release_reservation(req.size_usd);
                    {
                        std::lock_guard<std::mutex> lk(balance_mu_);
                        cached_balance_ts_ms_ = 0;   // force refresh on next read
                    }
                    reserve_usdc(req.size_usd);
                    continue;
                }
                break;  // other 4xx: final — act below
            }
            // Retry a 5xx only when the venue explicitly reports pre-match
            // signer validation failure. Every other 5xx is ambiguous.
            bool pre_match = resp.body.find("signer validation") != std::string::npos;
            if (!pre_match) break;
        }

        if (!resp.error.empty()) {
            r.reject_class = OrderResult::RejectClass::Transport;
            r.reject_reason = "submission outcome uncertain after HTTP transport error; reconcile venue before retry";
            r.status = "uncertain";
            spdlog::error("live: POST /order transport error; outcome uncertain");
            goto done;
        }
        if (resp.status != 200) {
            auto cls = classify_reject(resp.status, resp.body);
            r.reject_class = cls;
            reject_http_status = resp.status;
            const bool ambiguous_server_failure = resp.status >= 500 &&
                resp.body.find("signer validation") == std::string::npos;
            if (ambiguous_server_failure) r.status = "uncertain";
            r.reject_reason = ambiguous_server_failure
                ? fmt::format("CLOB status {} has uncertain submission outcome; reconcile venue before retry", resp.status)
                : fmt::format("CLOB status {} class {}", resp.status, (int)cls);
            spdlog::error("live: POST /order status={} class={}", resp.status, (int)cls);
            if (cls == OrderResult::RejectClass::AuthFailure) {
                // Trip the circuit breaker: block ALL submissions until creds change.
                set_auth_circuit(true);
                spdlog::error("live: AUTH CIRCUIT TRIPPED (maker-not-allowed/auth) — "
                              "all submissions blocked; maker={} signer={} sigType={}",
                              maker_addr_, signer_addr_, signature_type_);
            } else if (resp.status == 400 && resp.body.find("Invalid order payload") != std::string::npos) {
                spdlog::error("live: INVALID-PAYLOAD req: exec_price={:.4f} tick={:.4f} negRisk={} "
                              "maker_amt={} taker_amt={} token={}… side={}",
                              exec_price, real_tick, token_neg_risk,
                              amts.maker_amount, amts.taker_amount, req.token_id.substr(0, 16),
                              (req.dir == OrderDir::Buy) ? "BUY" : "SELL");
                // Self-heal: Polymarket flips a token's tick (0.01→0.001) once its
                // price crosses >0.96 or <0.04 (see docs: tick_size_change). A tick
                // we cached earlier then goes stale and rounds the price off-grid →
                // this 400. Drop the cached tick (and neg_risk, the other 400 cause)
                // so the next attempt re-fetches the authoritative values and the
                // order succeeds instead of spinning on a stale tick.
                std::lock_guard<std::mutex> lk(min_order_mu_);
                tick_size_cache_.erase(req.token_id);
                neg_risk_cache_.erase(req.token_id);
                min_order_size_cache_.erase(req.token_id);
            }
            goto done;
        }

        // Parse response — use DOM parser so fields can be accessed in any order.
        simdjson::dom::parser parser;
        simdjson::padded_string padded(resp.body.data(), resp.body.size());
        simdjson::dom::element doc;
        bool success = false;
        std::string_view order_id;
        std::string_view wire_status;
        double taking = 0, making = 0;
        const bool parsed = !parser.parse(padded).get(doc);
        if (parsed) {
            try {
                bool s; if (!doc["success"].get(s)) success = s;
                std::string_view sv;
                if (!doc["orderID"].get(sv))        order_id = sv;
                if (!doc["status"].get(sv))         wire_status = sv;
                std::string_view ta, ma;
                if (!doc["takingAmount"].get(ta))    taking = std::stod(std::string(ta));
                if (!doc["makingAmount"].get(ma))    making = std::stod(std::string(ma));
            } catch (...) {}
        }

        if (!parsed || (success && order_id.empty())) {
            r.reject_class = OrderResult::RejectClass::Transport;
            r.reject_reason = "CLOB returned HTTP 200 without a usable order acknowledgement; reconcile venue before retry";
            r.status = "uncertain";
            goto done;
        }
        if (!success || (!maker_order && taking <= 0)) {
            std::string_view err;
            try {
                simdjson::dom::element e; if (!doc["errorMsg"].get(e)) {
                    std::string_view sv; if (!e.get(sv)) err = sv;
                }
            } catch (...) {}
            // 200-with-success=false: FAK found nothing to match (market moved),
            // or a post-match venue rejection. Classify the errorMsg so telemetry
            // stays machine-readable. No auto-retry — a FAK that didn't cross is
            // a market miss, not an error.
            r.reject_class = classify_reject(200, std::string(err));
            // The venue error string is untrusted and can echo signed request
            // fields. Persist only the classified outcome, never raw text.
            r.reject_reason = err.empty()
                ? "CLOB returned HTTP 200 with incomplete fill outcome; reconcile venue before retry"
                : fmt::format("CLOB rejected order: class {}", (int)r.reject_class);
            if (err.empty()) r.status = "uncertain";
            spdlog::warn("live: order not filled; venue status={}", resp.status);
            goto done;
        }

        r.accepted = true;
        r.exchange_order_id = std::string(order_id);
        r.order_price = exec_price;
        r.order_shares = req.dir == OrderDir::Buy
            ? (double)amts.taker_amount / 1e6
            : (double)amts.maker_amount / 1e6;

        // Fill accepted (a post-only GTC normally reports zero here and rests).
        // The CLOB API response returns amounts in real units (not 1e6 micro-units):
        //   BUY:  takingAmount = shares received,  makingAmount = USDC spent
        //   SELL: makingAmount = shares sold,      takingAmount = USDC received
        if (req.dir == OrderDir::Buy) {
            r.fill_shares = taking;                              // shares received
            if (taking > 0 && making > 0)
                r.fill_price = making / taking;                  // actual USD/share
            else
                r.fill_price = exec_price;
        } else {
            r.fill_shares = making;                              // shares sold
            if (making > 0 && taking > 0)
                r.fill_price = taking / making;                  // actual USD/share
            else
                r.fill_price = exec_price;
        }
        // Maker fills do not pay the configured taker fee. Rebates are accounted
        // separately by the venue and are deliberately not estimated here.
        if (r.fill_shares > 0 && !maker_order) {
            double p = r.fill_price;
            double pe = 1.0, qe = 1.0;
            const double fee_rate = effective_taker_fee_rate(req);
            for (int i = 0; i < req.fee_exponent; ++i) { pe *= p; qe *= (1.0 - p); }
            r.fee_usd = fee_rate * pe * qe * r.fill_shares;
        }
        if (r.fill_shares > 0) {
            r.after = positions_.apply_fill(
                req.condition_id, req.asset, req.horizon_s, req.token_id,
                req.side, req.dir, r.fill_price, r.fill_shares,
                req.window_close_ms, r.fee_usd);
        }

        const bool fully_matched = !maker_order ||
            iequals(wire_status, "matched") || iequals(wire_status, "filled") ||
            r.fill_shares + 1e-9 >= r.order_shares;
        r.resting = maker_order && !fully_matched;
        r.terminal = fully_matched;
        r.status = fully_matched ? "filled" :
                   (r.fill_shares > 0 ? "partial" : "acked");

        if (r.resting && !r.exchange_order_id.empty()) {
            OrderRequest stored = req;
            const auto now_ms = Clock::wall_ns() / 1'000'000LL;
            if (stored.maker_root_deadline_ms <= 0 && stored.maker_max_age_ms > 0)
                stored.maker_root_deadline_ms = now_ms + stored.maker_max_age_ms;
            std::lock_guard<std::mutex> lk(open_mu_);
            open_orders_[r.exchange_order_id] = OpenOrder{
                stored, r.client_order_id, r.exchange_order_id, exec_price,
                r.order_shares, r.fill_shares, now_ms, now_ms, now_ms};
        }

        spdlog::info("LIVE {} {} {}@{:.3f} = ${:.2f} fee=${:.4f} id={} status={} ({})",
                     to_string(req.dir), to_string(req.side),
                     r.fill_shares, r.fill_price,
                     r.fill_shares * r.fill_price, r.fee_usd, order_id, r.status, req.source);
    }

done:
    // ── Phase 1.7: settle the in-flight reservation ──
    if (reserved) {
        release_reservation(req.size_usd);
        if (r.accepted) {
            // The fill changed the on-chain balance: force a fresh read next
            // time so the cache never diverges from reality.
            std::lock_guard<std::mutex> lk(balance_mu_);
            cached_balance_ts_ms_ = 0;
        }
    }
    if (!r.accepted) {
        if (r.status != "uncertain") {
            r.status = "rejected";
            r.terminal = true;
        }
        // ── Phase 1.4: structured rejection diagnostics ──
        // One line, machine-parseable: class + code + balance context +
        // per-token pricing inputs. Everything a post-mortem needs.
        double bal = cached_balance_if_fresh_usd();
        r.reject_detail_json = build_reject_detail(
            r.reject_class, reject_http_status,
            bal, req.token_id.substr(0, 16),
            r.fill_price, real_tick, req.neg_risk, signature_type_);
        spdlog::warn("LIVE REJECT ({}): {} | {}",
                     req.source, r.reject_reason, r.reject_detail_json);
    }
    if (journal_.on_order) journal_.on_order(req, r);
    return r;
}

bool LiveExecutor::cancel_order(const std::string& exchange_order_id) const {
    if (exchange_order_id.empty()) return false;
    const std::string body = fmt::format(R"({{"orderID":"{}"}})", exchange_order_id);
    auto hdrs = hmac_auth::build(signer_addr_, api_key_, api_secret_, api_passphrase_,
                                 "DELETE", "/order", body);
    auto resp = http::del(clob_url_ + "/order", body, hdrs, 5000);
    if (resp.status != 200) {
        spdlog::warn("live: cancel {} failed status={}",
                     exchange_order_id.substr(0, 14), resp.status);
        return false;
    }
    // Cancellation is partial-result capable. A 200 that puts this id in
    // not_canceled must not be treated as success.
    if (resp.body.find("not_canceled") != std::string::npos &&
        resp.body.find(exchange_order_id) != std::string::npos &&
        resp.body.find("\"canceled\":[\"") == std::string::npos) {
        spdlog::warn("live: cancel not confirmed {}", exchange_order_id.substr(0, 14));
        return false;
    }
    return true;
}

bool LiveExecutor::cancel_open_order(const std::string& exchange_order_id,
                                     const std::string& client_order_id) {
    OpenOrder snapshot;
    {
        std::lock_guard<std::mutex> lk(open_mu_);
        auto it = open_orders_.find(exchange_order_id);
        if (it == open_orders_.end() || it->second.client_order_id != client_order_id) return false;
        snapshot = it->second;
    }
    if (!cancel_order(exchange_order_id)) return false;
    {
        std::lock_guard<std::mutex> lk(open_mu_);
        auto it = open_orders_.find(exchange_order_id);
        if (it == open_orders_.end() || it->second.client_order_id != client_order_id) return false;
        it->second.cancel_requested = true;
        snapshot = it->second;
    }

    // DELETE confirms the request, while /data/order confirms the final match
    // quantity. Keep the local order tracked unless the venue reports a
    // terminal state so the normal poll loop can recover from a delayed read.
    const std::string path = "/data/order/" + exchange_order_id;
    auto hdrs = hmac_auth::build(signer_addr_, api_key_, api_secret_, api_passphrase_,
                                 "GET", path);
    auto resp = http::get(clob_url_ + path, hdrs, 5000);
    if (resp.status != 200) {
        spdlog::warn("live: final cancel reconciliation {} status={}",
                     exchange_order_id.substr(0, 14), resp.status);
        return false;
    }
    double matched = snapshot.matched_shares;
    double original = snapshot.original_shares;
    double price = snapshot.price;
    std::string venue_status;
    try {
        simdjson::dom::parser parser;
        simdjson::padded_string padded(resp.body.data(), resp.body.size());
        simdjson::dom::element doc;
        if (parser.parse(padded).get(doc) != simdjson::SUCCESS) return false;
        auto number = [&](const char* key, double fallback) {
            std::string_view sv;
            if (doc[key].get(sv) == simdjson::SUCCESS) {
                try { return std::stod(std::string(sv)); } catch (...) {}
            }
            double value = 0;
            return doc[key].get(value) == simdjson::SUCCESS ? value : fallback;
        };
        matched = number("size_matched", matched);
        original = number("original_size", original);
        price = number("price", price);
        std::string_view status;
        if (doc["status"].get(status) == simdjson::SUCCESS)
            venue_status = std::string(status);
    } catch (...) {
        spdlog::warn("live: final cancel reconciliation parse failed {}",
                     exchange_order_id.substr(0, 14));
        return false;
    }
    const bool venue_filled = matched + 1e-8 >= original ||
                              iequals(venue_status, "matched") ||
                              iequals(venue_status, "filled");
    const bool venue_cancelled = iequals(venue_status, "cancelled") ||
                                 iequals(venue_status, "canceled") ||
                                 iequals(venue_status, "expired");
    if (!venue_filled && !venue_cancelled) {
        spdlog::warn("live: cancel {} is not terminal yet (status={})",
                     exchange_order_id.substr(0, 14), venue_status);
        return false;
    }

    const double delta = std::max(0.0, matched - snapshot.matched_shares);
    if (delta > 1e-8) {
        OrderResult fill;
        fill.client_order_id = snapshot.client_order_id;
        fill.exchange_order_id = exchange_order_id;
        fill.accepted = true;
        fill.fill_price = price;
        fill.fill_shares = delta;
        fill.order_price = price;
        fill.order_shares = original;
        fill.status = venue_filled ? "filled" : "partial";
        fill.terminal = venue_filled;
        fill.after = positions_.apply_fill(
            snapshot.req.condition_id, snapshot.req.asset, snapshot.req.horizon_s,
            snapshot.req.token_id, snapshot.req.side, snapshot.req.dir,
            price, delta, snapshot.req.window_close_ms, 0.0);
        if (journal_.on_order) journal_.on_order(snapshot.req, fill);
    }
    {
        std::lock_guard<std::mutex> lk(open_mu_);
        open_orders_.erase(exchange_order_id);
    }
    if (venue_cancelled || delta <= 1e-8) {
        OrderResult result;
        result.client_order_id = snapshot.client_order_id;
        result.exchange_order_id = exchange_order_id;
        result.accepted = true;
        result.terminal = true;
        result.status = venue_filled ? "filled" : "cancelled";
        result.order_price = price;
        result.order_shares = original;
        if (journal_.on_order) journal_.on_order(snapshot.req, result);
    }
    return true;
}

void LiveExecutor::adopt_open_order(const OrderRequest& req,
                                    const std::string& client_order_id,
                                    const std::string& exchange_order_id,
                                    double price, double original_shares,
                                    double already_matched_shares,
                                    std::int64_t placed_ms) {
    if (client_order_id.empty() || exchange_order_id.empty() || original_shares <= 0) return;
    OrderRequest stored = req;
    if (stored.maker_root_deadline_ms <= 0 && stored.maker_max_age_ms > 0)
        stored.maker_root_deadline_ms = placed_ms + stored.maker_max_age_ms;
    std::lock_guard<std::mutex> lk(open_mu_);
    open_orders_[exchange_order_id] = OpenOrder{
        stored, client_order_id, exchange_order_id, price, original_shares,
        std::max(0.0, already_matched_shares), placed_ms, 0, placed_ms};
    spdlog::info("live: adopted maker order {} {} {:.2f}@{:.3f}",
                 exchange_order_id.substr(0, 14), req.asset, original_shares, price);
}

std::size_t LiveExecutor::open_order_count() const {
    std::lock_guard<std::mutex> lk(open_mu_);
    return open_orders_.size();
}

std::vector<LiveExecutor::OpenOrderView> LiveExecutor::open_orders() const {
    std::lock_guard<std::mutex> lk(open_mu_);
    std::vector<OpenOrderView> out;
    out.reserve(open_orders_.size());
    for (const auto& [_, order] : open_orders_) {
        out.push_back({order.req, order.client_order_id, order.exchange_order_id,
            order.price, std::max(0.0, order.original_shares - order.matched_shares)});
    }
    return out;
}

std::size_t LiveExecutor::maintain_open_orders(
    const std::function<const OrderBook*(const std::string&, ShareSide)>& book_lookup,
    std::int64_t now_ms, const std::function<bool()>& may_replace) {
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lk(open_mu_);
        ids.reserve(open_orders_.size());
        for (const auto& [id, _] : open_orders_) ids.push_back(id);
    }
    std::size_t changes = 0;
    for (const auto& id : ids) {
        OpenOrder snapshot;
        {
            std::lock_guard<std::mutex> lk(open_mu_);
            auto it = open_orders_.find(id);
            if (it == open_orders_.end()) continue;
            if (now_ms - it->second.last_poll_ms < std::max(100, it->second.req.maker_poll_ms))
                continue;
            it->second.last_poll_ms = now_ms;
            snapshot = it->second;
        }

        const std::string path = "/data/order/" + id;
        auto hdrs = hmac_auth::build(signer_addr_, api_key_, api_secret_, api_passphrase_,
                                     "GET", path);
        auto resp = http::get(clob_url_ + path, hdrs, 5000);
        if (resp.status != 200) {
            spdlog::warn("live: maker poll {} status={}", id.substr(0, 14), resp.status);
            continue;
        }

        double matched = snapshot.matched_shares;
        double original = snapshot.original_shares;
        double price = snapshot.price;
        std::string status;
        try {
            simdjson::dom::parser parser;
            simdjson::padded_string padded(resp.body.data(), resp.body.size());
            simdjson::dom::element doc;
            if (!parser.parse(padded).get(doc)) {
                auto number = [&](const char* key, double fallback) {
                    std::string_view sv;
                    if (!doc[key].get(sv)) {
                        try { return std::stod(std::string(sv)); } catch (...) {}
                    }
                    double v = 0; if (!doc[key].get(v)) return v;
                    return fallback;
                };
                matched = number("size_matched", matched);
                original = number("original_size", original);
                price = number("price", price);
                std::string_view sv;
                if (!doc["status"].get(sv)) status = std::string(sv);
            }
        } catch (...) {
            spdlog::warn("live: maker poll parse failed {}", id.substr(0, 14));
            continue;
        }

        const double delta = std::max(0.0, matched - snapshot.matched_shares);
        if (delta > 1e-8) {
            OrderResult u;
            u.client_order_id = snapshot.client_order_id;
            u.exchange_order_id = id;
            u.accepted = true;
            u.fill_price = price;
            u.fill_shares = delta;
            u.order_price = price;
            u.order_shares = original;
            u.status = matched + 1e-8 >= original ? "filled" : "partial";
            u.terminal = u.status == "filled";
            u.after = positions_.apply_fill(
                snapshot.req.condition_id, snapshot.req.asset, snapshot.req.horizon_s,
                snapshot.req.token_id, snapshot.req.side, snapshot.req.dir,
                price, delta, snapshot.req.window_close_ms, 0.0);
            if (journal_.on_order) journal_.on_order(snapshot.req, u);
            spdlog::info("LIVE MAKER FILL {} {} +{:.2f}@{:.3f} total={:.2f}/{:.2f}",
                         id.substr(0, 14), snapshot.req.asset, delta, price, matched, original);
            ++changes;
        }

        const bool venue_filled = matched + 1e-8 >= original ||
                                  iequals(status, "matched") || iequals(status, "filled");
        const bool venue_cancelled = iequals(status, "cancelled") ||
                                     iequals(status, "canceled") ||
                                     iequals(status, "expired");
        const bool terminal = venue_filled || venue_cancelled;
        if (terminal) {
            // Persist a zero-fill terminal transition when the latest poll did
            // not already write `filled`. This prevents a venue-cancelled or
            // already-filled order from remaining `acked/partial` in Postgres
            // and being incorrectly re-adopted after a restart.
            if (venue_cancelled || delta <= 1e-8) {
                OrderResult terminal_event;
                terminal_event.client_order_id = snapshot.client_order_id;
                terminal_event.exchange_order_id = id;
                terminal_event.accepted = true;
                terminal_event.terminal = true;
                terminal_event.status = venue_filled ? "filled" : "cancelled";
                terminal_event.order_price = price;
                terminal_event.order_shares = original;
                if (journal_.on_order) journal_.on_order(snapshot.req, terminal_event);
            }
            std::lock_guard<std::mutex> lk(open_mu_);
            open_orders_.erase(id);
            continue;
        }

        {
            std::lock_guard<std::mutex> lk(open_mu_);
            auto it = open_orders_.find(id);
            if (it != open_orders_.end()) {
                it->second.matched_shares = matched;
                it->second.original_shares = original;
                it->second.price = price;
            }
        }

        // An operator cancellation has already reached the venue. Poll until
        // the venue confirms a terminal state, but never reprice or replace it.
        if (snapshot.cancel_requested) continue;

        const bool closing = snapshot.req.window_close_ms > 0 &&
            now_ms >= snapshot.req.window_close_ms -
                      std::max(0, snapshot.req.maker_cancel_before_close_ms);
        const bool expired = snapshot.req.maker_root_deadline_ms > 0 &&
                             now_ms >= snapshot.req.maker_root_deadline_ms;
        bool reprice = false;
        if (!closing && !expired && snapshot.req.maker_reprice_ms > 0 &&
            now_ms - snapshot.last_reprice_ms >= snapshot.req.maker_reprice_ms) {
            if (const OrderBook* book = book_lookup(snapshot.req.condition_id, snapshot.req.side)) {
                const auto bid = book->best_bid();
                const auto ask = book->best_ask();
                // Use the CLOB-authoritative cached tick, not the possibly
                // stale gamma tick carried on the request — a 0.01-vs-0.001
                // mismatch misprices the replacement order (the same class of
                // bug the submit path fixed by consulting this cache).
                double tick = 0;
                {
                    std::lock_guard<std::mutex> lk(min_order_mu_);
                    auto it = tick_size_cache_.find(snapshot.req.token_id);
                    if (it != tick_size_cache_.end()) tick = it->second;
                }
                if (!(tick > 0))
                    tick = snapshot.req.tick_size > 0 ? snapshot.req.tick_size : 0.01;
                double target = 0.0;
                if (snapshot.req.dir == OrderDir::Buy) {
                    target = bid.price > 0
                        ? bid.price + std::max(0, snapshot.req.maker_improve_ticks) * tick
                        : ask.price - tick;
                    if (ask.price > 0) target = std::min(target, ask.price - tick);
                    if (snapshot.req.limit_price > 0)
                        target = std::min(target, snapshot.req.limit_price);
                    target = std::floor(target / tick + 1e-9) * tick;
                } else {
                    target = ask.price > 0
                        ? ask.price - std::max(0, snapshot.req.maker_improve_ticks) * tick
                        : bid.price + tick;
                    target = std::max(target, bid.price + tick);
                    if (snapshot.req.limit_price > 0)
                        target = std::max(target, snapshot.req.limit_price);
                    target = std::ceil(target / tick - 1e-9) * tick;
                    target = std::min(target, 1.0 - tick);
                }
                reprice = target > 0 && std::fabs(target - price) >= tick * 0.5;
            }
        }
        if (!closing && !expired && !reprice) continue;

        if (!cancel_order(id)) continue;
        {
            std::lock_guard<std::mutex> lk(open_mu_);
            open_orders_.erase(id);
        }
        OrderResult cancelled;
        cancelled.client_order_id = snapshot.client_order_id;
        cancelled.exchange_order_id = id;
        cancelled.accepted = true;
        cancelled.terminal = true;
        cancelled.status = "cancelled";
        cancelled.order_price = price;
        cancelled.order_shares = original;
        if (journal_.on_order) journal_.on_order(snapshot.req, cancelled);
        ++changes;

        const double remaining = std::max(0.0, original - matched);
        const bool fallback = expired && !closing &&
                              snapshot.req.execution_mode == ExecutionMode::Hybrid;
        const bool allow_replacement = may_replace();
        if ((reprice || fallback) && remaining > 1e-8 && !allow_replacement) {
            spdlog::critical("live: maker replacement/fallback suppressed by order uncertainty halt");
        }
        if ((reprice || fallback) && remaining > 1e-8 && allow_replacement) {
            auto next = snapshot.req;
            next.intent_id.clear(); // Replacement is a distinct order attempt.
            next.size_usd = remaining * price;
            if (fallback) {
                next.execution_mode = ExecutionMode::Taker;
                next.size_shares = 0;
                next.source += "_HYBRID_FAK";
            } else {
                next.size_shares = remaining;
            }
            auto placed = submit(next, book_lookup);
            if (!placed.accepted)
                spdlog::warn("live: maker {} failed for {}: {}",
                             fallback ? "fallback" : "reprice",
                             snapshot.req.asset, placed.reject_reason);
        }
    }
    return changes;
}

void LiveExecutor::cancel_open_orders() {
    std::vector<OpenOrder> orders;
    {
        std::lock_guard<std::mutex> lk(open_mu_);
        for (const auto& [_, o] : open_orders_) orders.push_back(o);
    }
    for (const auto& o : orders) {
        if (!cancel_order(o.exchange_order_id)) continue;
        {
            std::lock_guard<std::mutex> lk(open_mu_);
            open_orders_.erase(o.exchange_order_id);
        }
        OrderResult u;
        u.client_order_id = o.client_order_id;
        u.exchange_order_id = o.exchange_order_id;
        u.accepted = true;
        u.terminal = true;
        u.status = "cancelled";
        u.order_price = o.price;
        u.order_shares = o.original_shares;
        if (journal_.on_order) journal_.on_order(o.req, u);
    }
}

} // namespace vp
