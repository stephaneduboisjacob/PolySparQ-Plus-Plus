// Polymarket REST: BTC/ETH up-down market discovery via Gamma API.
//
// We query /events through the next day so longer crypto windows are visible.
// Note: the /markets
// endpoint's time filters stopped returning updown markets; /events still works and
// embeds a full markets[] sub-array so a single request yields everything we need.
// Client-side filters recognize short-window epoch slugs and long-form hourly /
// daily crypto Up-or-Down slugs. The explicit duration in the contract wins;
// window open is validated against endDate and otherwise derived from duration.
#include "polymarket_rest.hpp"
#include "net/http_client.hpp"
#include <curl/curl.h>
#include <simdjson.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <regex>
#include <unordered_set>
namespace vp {
namespace {
size_t curl_write_cb(char* ptr, size_t sz, size_t nm, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, sz * nm);
    return sz * nm;
}
std::string http_get(const std::string& url) {
    CURL* h = curl_easy_init();
    if (!h) return {};
    std::string body; body.reserve(256 * 1024);
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(h, CURLOPT_USERAGENT, "vantage-pulse/0.1");
    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");
    long status = 0;
    CURLcode rc = curl_easy_perform(h);
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(h);
    if (rc != CURLE_OK) {
        spdlog::warn("curl GET {} failed: {}", url, curl_easy_strerror(rc));
        return {};
    }
    if (status / 100 != 2) {
        spdlog::warn("HTTP {} from {} ({} bytes)", status, url, body.size());
        return {};
    }
    return body;
}
std::int64_t iso_to_ms(std::string_view iso) {
    if (iso.empty()) return 0;
    int yr=0,mo=0,dy=0,hr=0,mi=0,se=0;
    if (std::sscanf(std::string(iso).c_str(), "%d-%d-%dT%d:%d:%d",
                    &yr,&mo,&dy,&hr,&mi,&se) < 6) return 0;
    std::tm tm{}; tm.tm_year=yr-1900; tm.tm_mon=mo-1; tm.tm_mday=dy;
    tm.tm_hour=hr; tm.tm_min=mi; tm.tm_sec=se;
    return std::int64_t(timegm(&tm)) * 1000;
}
std::vector<std::string> parse_json_string_array(std::string_view raw) {
    std::vector<std::string> out;
    std::string s(raw);
    if (s.size() >= 2 && s.front()=='"' && s.back()=='"') {
        s = s.substr(1, s.size()-2);
        std::string u; u.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i]=='\\' && i+1 < s.size()) { u += s[i+1]; ++i; }
            else u += s[i];
        }
        s.swap(u);
    }
    auto lb = s.find('['); auto rb = s.rfind(']');
    if (lb == std::string::npos || rb == std::string::npos || rb <= lb) return out;
    std::string inner = s.substr(lb+1, rb-lb-1);
    bool in_str = false; std::string cur;
    for (char c : inner) {
        if (c == '"') { in_str = !in_str; continue; }
        if (c == ',' && !in_str) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else if (in_str || !std::isspace((unsigned char)c)) {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
double parse_target_price(std::string_view question) {
    static const std::regex re(R"(\$\s*([0-9][0-9,]*)(?:\.[0-9]+)?)");
    std::cmatch m; std::string q(question);
    if (std::regex_search(q.c_str(), m, re)) {
        std::string n = m[1].str();
        n.erase(std::remove(n.begin(), n.end(), ','), n.end());
        try { return std::stod(n); } catch (...) {}
    }
    return 0.0;
}
// Match standard 5m/15m/1h/4h/24h crypto windows and the older Gamma hourly/daily
// slugs. Returns asset and horizon, or {"", 0} for unrelated events.
struct SlugInfo { std::string asset; int horizon = 0; };
SlugInfo classify_slug(std::string_view slug) {
    std::string s(slug);
    std::transform(s.begin(),s.end(),s.begin(),
                   [](unsigned char c){return static_cast<char>(std::tolower(c));});
    static const std::regex short_re(R"(^(btc|eth|sol|xrp)-updown-(5m|15m|1h|4h|24h)-\d+)" );
    std::smatch m;
    SlugInfo o;
    if(std::regex_search(s,m,short_re)) {
        const std::string a=m[1].str(), hz=m[2].str();
        o.asset=a=="btc"?"BTC":a=="eth"?"ETH":a=="sol"?"SOL":"XRP";
        o.horizon=hz=="5m"?300:hz=="15m"?900:hz=="1h"?3600:hz=="4h"?14400:86400;
        return o;
    }
    struct Alias { const char* slug; const char* asset; };
    static constexpr Alias aliases[]={{"bitcoin","BTC"},{"ethereum","ETH"},
                                      {"solana","SOL"},{"xrp","XRP"}};
    for(const auto& a:aliases) {
        const std::string prefix=std::string(a.slug)+"-up-or-down";
        if(s.rfind(prefix,0)!=0) continue;
        o.asset=a.asset;
        // Gamma's long-form "...-on-<date>" products are daily; the older
        // "...-<date>-<hour>-et" products are hourly.
        o.horizon=s.find("-on-",prefix.size())!=std::string::npos?86400:3600;
        return o;
    }
    return {};
}
} // namespace
PolymarketRest::PolymarketRest(std::string c, std::string g)
    : clob_url_(std::move(c)), gamma_url_(std::move(g)) {
    static std::once_flag once;
    std::call_once(once, []{ curl_global_init(CURL_GLOBAL_DEFAULT); });
    if (gamma_url_.empty()) gamma_url_ = "https://gamma-api.polymarket.com";
}
PolymarketRest::~PolymarketRest() = default;
std::vector<PmMarket> PolymarketRest::discover_updown_markets() {
    std::vector<PmMarket> out; out.reserve(64);
    std::unordered_set<std::string> seen;
    simdjson::dom::parser parser;
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    auto to_iso = [](int64_t ms) -> std::string {
        time_t t = (time_t)(ms / 1000);
        struct tm tms {};
        gmtime_r(&t, &tms);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tms);
        return buf;
    };
    std::string now_iso = to_iso(now_ms);
    // Gamma caps at 100 results per page, and BTC/ETH updown events are
    // alphabetically buried past page 1 among ~500+ events. Paginate through
    // pages to find them. Break when a page has fewer than 100 items (last page).
    std::string plus25h = to_iso(now_ms + 25LL * 60 * 60 * 1000);
    int diag = 0;
    for (int offset = 0; offset < 2000; offset += 100) {
        std::string url = gamma_url_
            + "/events?limit=100&offset=" + std::to_string(offset)
            + "&closed=false"
            + "&end_date_min=" + now_iso
            + "&end_date_max=" + plus25h;
        std::string body = http_get(url);
        if (body.empty()) break;

        simdjson::padded_string padded(body);
        simdjson::dom::element doc;
        if (parser.parse(padded).get(doc) || !doc.is_array()) break;

        auto arr = doc.get_array();
        // Check if page is empty
        int page_count = 0;
        for ([[maybe_unused]] auto _ : arr) ++page_count;
        if (page_count == 0) break;

        simdjson::padded_string padded2(body);
        simdjson::dom::element doc2;
        if (parser.parse(padded2).get(doc2) || !doc2.is_array()) break;

        for (auto ev : doc2.get_array()) {
            std::string_view ev_sv;
            if (ev["slug"].get(ev_sv)) continue;
            std::string slug(ev_sv);
            auto info = classify_slug(slug);
            if (info.asset.empty()) continue;

            bool ev_neg_risk = false;
            if (ev["negRisk"].get(ev_neg_risk) != simdjson::SUCCESS) ev_neg_risk = false;

            simdjson::dom::element markets_el;
            if (ev["markets"].get(markets_el) || !markets_el.is_array()) continue;

            for (auto m : markets_el.get_array()) {
                std::string_view sv;
                PmMarket pm;
                pm.slug           = slug;
                pm.asset          = info.asset;
                pm.horizon_seconds = info.horizon;
                pm.neg_risk       = ev_neg_risk;
                std::string_view resolution;
                if (m["resolutionSource"].get(resolution) == simdjson::SUCCESS &&
                    resolution.find("data.chain.link/") != std::string_view::npos) {
                    if (resolution.find("twap-60s") != std::string_view::npos) pm.settlement_window_s = 60;
                    else if (resolution.find("twap-30s") != std::string_view::npos) pm.settlement_window_s = 30;
                    else if (resolution.find("twap") == std::string_view::npos) pm.settlement_window_s = 0;
                } else if (m["resolutionSource"].get(resolution) == simdjson::SUCCESS &&
                           resolution.find("binance.com/") != std::string_view::npos) {
                    pm.reference_is_binance = true;
                    pm.settlement_window_s = 0;
                }

                if (m["conditionId"].get(sv) != simdjson::SUCCESS || sv.empty()) continue;
                pm.condition_id = std::string(sv);
                if (!seen.insert(pm.condition_id).second) continue;

                if (m["question"].get(sv) == simdjson::SUCCESS) pm.question = std::string(sv);

                double tick_size = 0.01;
                if (m["orderPriceMinTickSize"].get(tick_size) == simdjson::SUCCESS && tick_size > 0)
                    pm.tick_size = tick_size;

                std::vector<std::string> tokens, outcomes;
                if (m["clobTokenIds"].get(sv) == simdjson::SUCCESS)
                    tokens = parse_json_string_array(sv);
                if (m["outcomes"].get(sv) == simdjson::SUCCESS)
                    outcomes = parse_json_string_array(sv);
                if (tokens.size() < 2) continue;

                int up_idx = 0, down_idx = 1;
                for (size_t i = 0; i < outcomes.size() && i < 2; ++i) {
                    std::string lo = outcomes[i];
                    std::transform(lo.begin(), lo.end(), lo.begin(),
                                   [](unsigned char c){ return std::tolower(c); });
                    if (lo == "up")   up_idx   = (int)i;
                    if (lo == "down") down_idx = (int)i;
                }
                pm.yes_token_id = tokens[up_idx];
                pm.no_token_id  = tokens[down_idx];

                if (m["endDate"].get(sv) == simdjson::SUCCESS) pm.window_close_ms = iso_to_ms(sv);
                if (pm.window_close_ms > 0 && pm.window_close_ms < now_ms) continue;

                if (m["eventStartTime"].get(sv) == simdjson::SUCCESS) {
                    int64_t t = iso_to_ms(sv);
                    // Long-form hourly and daily events can be listed before
                    // their measurement window opens. Trust the event timestamp
                    // only when it agrees with the contract's duration.
                    const int64_t expected=static_cast<int64_t>(pm.horizon_seconds)*1000LL;
                    const int64_t tolerance=std::max<int64_t>(30000,expected/100);
                    if (t>0 && pm.window_close_ms>t &&
                        std::llabs((pm.window_close_ms-t)-expected)<=tolerance)
                        pm.window_open_ms=t;
                }
                if (pm.window_open_ms == 0 && pm.window_close_ms != 0 && pm.horizon_seconds>0)
                    pm.window_open_ms = pm.window_close_ms - (int64_t)pm.horizon_seconds * 1000LL;

                pm.target_price = parse_target_price(pm.question);

                if (diag++ < 4) {
                    int64_t close_in = (pm.window_close_ms - now_ms) / 1000;
                    int64_t open_ago = (now_ms - pm.window_open_ms) / 1000;
                    const char* hz_label = pm.horizon_seconds == 300  ? "5m"  :
                                           pm.horizon_seconds == 900  ? "15m" :
                                           pm.horizon_seconds == 3600 ? "1h"  :
                                           pm.horizon_seconds == 14400? "4h"  :
                                           pm.horizon_seconds == 86400? "24h" : "?";
                    spdlog::debug("rest mkt[{}] {} {} close_in={}s open_ago={}s",
                        diag - 1, pm.asset, hz_label, close_in, open_ago);
                }
                out.push_back(std::move(pm));
            }
        }
    }
    spdlog::info("gamma discovered {} up-down markets", out.size());
    if (out.empty()) {
        spdlog::warn("no up-down markets found — Polymarket may not have active "
                     "windows right now (they typically run during US market hours 13:30+ UTC). "
                     "The engine will retry on the next discovery cycle.");
    }
    return out;
}

std::optional<bool> PolymarketRest::parse_resolved_up_outcome(std::string_view json,
                                                           std::string_view condition_id) {
    try {
        simdjson::dom::parser parser;
        simdjson::padded_string padded(json);
        auto doc=parser.parse(padded);
        for (auto market : doc.get_array()) {
            std::string_view cid,status,outcomes,prices;
            bool closed=false;
            if (market["conditionId"].get(cid) || cid!=condition_id ||
                market["closed"].get(closed) || !closed ||
                market["umaResolutionStatus"].get(status) || status!="resolved" ||
                market["outcomes"].get(outcomes) || market["outcomePrices"].get(prices)) continue;
            auto sides=parse_json_string_array(outcomes), values=parse_json_string_array(prices);
            if (sides.size()!=2 || values.size()!=2) continue;
            int up=-1,down=-1;
            for (size_t i=0;i<2;++i) {
                // Only final 0/1 payouts are accepted, not market-price estimates.
                const int v=values[i]=="1" ? 1 : values[i]=="0" ? 0 : -1;
                if (sides[i]=="Up" || sides[i]=="UP") up=v;
                if (sides[i]=="Down" || sides[i]=="DOWN") down=v;
            }
            if (up>=0 && down>=0 && up+down==1) return up==1;
        }
    } catch (...) {}
    return std::nullopt;
}

std::optional<bool> PolymarketRest::resolved_up_outcome(const std::string& condition_id) {
    if (condition_id.size()!=66 || condition_id.substr(0,2)!="0x" ||
        !std::all_of(condition_id.begin()+2,condition_id.end(),
                     [](unsigned char c){return std::isxdigit(c);})) return std::nullopt;
    return parse_resolved_up_outcome(http_get(gamma_url_+
        "/markets?closed=true&condition_ids="+condition_id),condition_id);
}

std::vector<PolymarketMD::BookUpdate> PolymarketRest::fetch_orderbooks(const std::vector<std::string>& token_ids) {
    std::vector<PolymarketMD::BookUpdate> updates;
    if (token_ids.empty()) return updates;

    auto field_double = [](simdjson::dom::element e) -> double {
        std::string_view sv;
        if (e.get(sv) == simdjson::SUCCESS) {
            try { return std::stod(std::string(sv)); } catch (...) { return 0.0; }
        }
        double d = 0.0;
        if (e.get(d) == simdjson::SUCCESS) return d;
        int64_t i = 0;
        if (e.get(i) == simdjson::SUCCESS) return static_cast<double>(i);
        return 0.0;
    };
    auto field_i64 = [](simdjson::dom::element e) -> int64_t {
        std::string_view sv;
        if (e.get(sv) == simdjson::SUCCESS) {
            try { return std::stoll(std::string(sv)); } catch (...) { return 0; }
        }
        int64_t i = 0;
        if (e.get(i) == simdjson::SUCCESS) return i;
        uint64_t u = 0;
        if (e.get(u) == simdjson::SUCCESS) return static_cast<int64_t>(u);
        return 0;
    };
    auto parse_book = [&](simdjson::dom::element doc) {
        PolymarketMD::BookUpdate bu;
        bu.is_snapshot = true;
        std::string_view sv;

        if (doc["asset_id"].get(sv) == simdjson::SUCCESS) bu.token_id = std::string(sv);
        if (doc["market"].get(sv) == simdjson::SUCCESS)   bu.condition_id = std::string(sv);
        bu.exchange_ms = field_i64(doc["timestamp"]);

        simdjson::dom::array bids_arr, asks_arr;
        if (doc["bids"].get(bids_arr) == simdjson::SUCCESS) {
            for (auto lvl : bids_arr) {
                PriceLevel l;
                l.price = field_double(lvl["price"]);
                l.size  = field_double(lvl["size"]);
                if (l.price > 0) bu.bids.push_back(l);
            }
        }
        if (doc["asks"].get(asks_arr) == simdjson::SUCCESS) {
            for (auto lvl : asks_arr) {
                PriceLevel l;
                l.price = field_double(lvl["price"]);
                l.size  = field_double(lvl["size"]);
                if (l.price > 0) bu.asks.push_back(l);
            }
        }
        if (!bu.token_id.empty()) updates.push_back(std::move(bu));
    };

    simdjson::dom::parser parser;
    constexpr size_t kBatchSize = 100;
    for (size_t bs = 0; bs < token_ids.size(); bs += kBatchSize) {
        size_t be = std::min(bs + kBatchSize, token_ids.size());
        std::string req = "[";
        for (size_t i = bs; i < be; ++i) {
            if (i != bs) req += ',';
            req += R"({"token_id":")";
            req += token_ids[i];
            req += R"("})";
        }
        req += "]";

        auto resp = http::post(clob_url_ + "/books", req, {}, 10000);
        if (resp.status / 100 != 2 || resp.body.empty()) {
            spdlog::warn("POST /books failed status={} err={} tokens={}",
                         resp.status, resp.error, be - bs);
            continue;
        }
        simdjson::padded_string padded(resp.body);
        simdjson::dom::element doc;
        if (parser.parse(padded).get(doc) != simdjson::SUCCESS) continue;
        simdjson::dom::array arr;
        if (doc.get(arr) == simdjson::SUCCESS) {
            for (auto book : arr) parse_book(book);
        } else {
            parse_book(doc);
        }
    }
    return updates;
}

} // namespace vp
