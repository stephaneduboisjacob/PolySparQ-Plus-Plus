#include "config.hpp"
#include "known_config_keys.hpp"
#include "third_party/toml.hpp"
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <simdjson.h>
#include <stdexcept>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
namespace vp {
namespace {
TradeMode parse_mode(std::string_view s) {
    if (s == "live")   return TradeMode::Live;
    if (s == "paper")  return TradeMode::Paper;
    if (s == "replay") return TradeMode::Replay;
    throw std::runtime_error("invalid runtime.mode: " + std::string{s});
}
void warn_unknown_config_keys(const toml::table& table, const std::string& prefix = {}) {
    for (const auto& [key, node] : table) {
        const std::string path = prefix.empty() ? std::string(key.str())
                                                : prefix + "." + std::string(key.str());
        if (const auto* nested = node.as_table()) {
            warn_unknown_config_keys(*nested, path);
        } else if (std::find(known_config_keys.begin(), known_config_keys.end(),
                             std::string_view(path)) == known_config_keys.end()) {
            spdlog::warn("config unknown key: {} (ignored)", path);
        }
    }
}
template <class T>
T get_or(const toml::table* t, std::string_view key, T defv) {
    if (!t) return defv;
    if (auto v = (*t)[key].value<T>()) return *v;
    if ((*t)[key]) spdlog::warn("config invalid type for key: {} (default used)", key);
    return defv;
}
std::vector<int> get_int_array_or(const toml::table* t,
                                  std::string_view key,
                                  std::vector<int> defv) {
    if (!t) return defv;
    auto arr = (*t)[key].as_array();
    if (!arr) {
        if ((*t)[key]) spdlog::warn("config invalid array for key: {} (default used)", key);
        return defv;
    }
    std::vector<int> out;
    for (auto& v : *arr) {
        if (auto n = v.value<int>()) out.push_back(*n);
        else spdlog::warn("config invalid integer in array: {} (element ignored)", key);
    }
    return out.empty() ? defv : out;
}
} // namespace
Config Config::load(const std::string& path) {
    auto tbl = toml::parse_file(path);
    warn_unknown_config_keys(tbl);
    Config c;
    auto* rt = tbl["runtime"].as_table();
    c.runtime.mode        = parse_mode(get_or<std::string>(rt, "mode", "paper"));
    c.runtime.log_level   = get_or<std::string>(rt, "log_level",   "info");
    c.runtime.instance_label = get_or<std::string>(rt, "instance_label", "");
    c.runtime.journal_dir = get_or<std::string>(rt, "journal_dir", "./journal");
    c.runtime.config_hot_reload_interval_ms =
        get_or<int>(rt, "config_hot_reload_interval_ms",
                    c.runtime.config_hot_reload_interval_ms);
    c.runtime.cockpit_command_poll_ms =
        get_or<int>(rt, "cockpit_command_poll_ms",
                    c.runtime.cockpit_command_poll_ms);
    c.runtime.wallet_balance_refresh_ms =
        get_or<int>(rt, "wallet_balance_refresh_ms",
                    c.runtime.wallet_balance_refresh_ms);
    c.runtime.live_heartbeat_ms =
        get_or<int>(rt, "live_heartbeat_ms",
                    c.runtime.live_heartbeat_ms);
    c.runtime.position_snapshot_interval_ms =
        get_or<int>(rt, "position_snapshot_interval_ms",
                    c.runtime.position_snapshot_interval_ms);
    auto* pg = tbl["postgres"].as_table();
    c.postgres.conninfo     = get_or<std::string>(pg, "conninfo",     "");
    c.postgres.read_replica = get_or<std::string>(pg, "read_replica", "");
    auto* pm = tbl["polymarket"].as_table();
    c.polymarket.clob_rest_url    = get_or<std::string>(pm, "clob_rest_url",    "");
    c.polymarket.gamma_rest_url   = get_or<std::string>(pm, "gamma_rest_url",   "");
    c.polymarket.clob_ws_url      = get_or<std::string>(pm, "clob_ws_url",      "");
    c.polymarket.clob_user_ws_url = get_or<std::string>(pm, "clob_user_ws_url", "");
    c.polymarket.rtds_ws_url      = get_or<std::string>(pm, "rtds_ws_url",
                                        "wss://ws-live-data.polymarket.com");
    c.polymarket.coinbase_ws_url  = get_or<std::string>(pm, "coinbase_ws_url",
                                        "wss://ws-feed.exchange.coinbase.com");
    c.polymarket.poll_seconds     = get_or<int>        (pm, "poll_seconds",     60);
    c.polymarket.universe_horizons_seconds =
        get_int_array_or(pm, "universe_horizons_seconds",
                         c.polymarket.universe_horizons_seconds);
    c.polymarket.universe_max_windows_per_series =
        get_or<int>(pm, "universe_max_windows_per_series",
                    c.polymarket.universe_max_windows_per_series);
    c.polymarket.universe_max_markets =
        get_or<int>(pm, "universe_max_markets",
                    c.polymarket.universe_max_markets);
    c.polymarket.universe_prewarm_ahead_minutes =
        get_or<int>(pm, "universe_prewarm_ahead_minutes",
                    c.polymarket.universe_prewarm_ahead_minutes);
    c.polymarket.market_prune_interval_ms =
        get_or<int>(pm, "market_prune_interval_ms",
                    c.polymarket.market_prune_interval_ms);
    c.polymarket.market_prune_grace_ms =
        get_or<int>(pm, "market_prune_grace_ms",
                    c.polymarket.market_prune_grace_ms);
    c.polymarket.closed_market_display_grace_ms =
        get_or<int>(pm, "closed_market_display_grace_ms",
                    c.polymarket.closed_market_display_grace_ms);
    c.polymarket.book_health_log_interval_ms =
        get_or<int>(pm, "book_health_log_interval_ms",
                    c.polymarket.book_health_log_interval_ms);
    c.polymarket.book_health_stale_ms =
        get_or<int>(pm, "book_health_stale_ms",
                    c.polymarket.book_health_stale_ms);
    c.polymarket.rest_book_seed_interval_ms =
        get_or<int>(pm, "rest_book_seed_interval_ms",
                    c.polymarket.rest_book_seed_interval_ms);
    c.polymarket.rest_book_seed_log_interval_ms =
        get_or<int>(pm, "rest_book_seed_log_interval_ms",
                    c.polymarket.rest_book_seed_log_interval_ms);
    c.polymarket.rest_book_seed_min_age_ms =
        get_or<int>(pm, "rest_book_seed_min_age_ms",
                    c.polymarket.rest_book_seed_min_age_ms);
    c.polymarket.proxy_address    = get_or<std::string>(pm, "proxy_address",    "");
    c.polymarket.signer_address   = get_or<std::string>(pm, "signer_address",   "");
    c.polymarket.rtds_enabled     = get_or<bool>       (pm, "rtds_enabled",     true);
    c.polymarket.blocked_horizons_seconds =
        get_int_array_or(pm, "blocked_horizons_seconds",
                         c.polymarket.blocked_horizons_seconds);
    c.polymarket.block_5m  = get_or<bool>(pm, "block_5m",  c.polymarket.block_5m);
    c.polymarket.block_15m = get_or<bool>(pm, "block_15m", c.polymarket.block_15m);
    if (c.polymarket.block_5m)
        c.polymarket.blocked_horizons_seconds.push_back(300);
    if (c.polymarket.block_15m)
        c.polymarket.blocked_horizons_seconds.push_back(900);
    auto* bn = tbl["binance"].as_table();
    c.binance.ws_url = get_or<std::string>(bn, "ws_url", "");
    c.binance.futures_ws_url = get_or<std::string>(bn, "futures_ws_url", "");
    if (bn) {
        if (auto arr = (*bn)["streams"].as_array()) {
            for (auto& v : *arr)
                if (auto s = v.value<std::string>()) c.binance.streams.push_back(*s);
        }
    }
    auto* po = tbl["polygon"].as_table();
    c.polygon.rpc_url = get_or<std::string>(po, "rpc_url", "");
    auto* lm = tbl["llm"].as_table();
    c.llm.url             = get_or<std::string>(lm, "url",             std::string{});
    c.llm.model           = get_or<std::string>(lm, "model",           std::string{"gpt-5-mini"});
    c.llm.timeout_seconds = get_or<int>        (lm, "timeout_seconds", 50);
    c.llm.parallel_max    = get_or<int>        (lm, "parallel_max",    10);
    c.llm.api_key_file    = get_or<std::string>(lm, "api_key_file",    std::string{});
    c.llm.enabled         = get_or<bool>       (lm, "enabled",         true);
    auto* st = tbl["strategy"]["hot"].as_table();
    auto& s = c.strategy;
    s.max_entry_share_price   = get_or<double>(st, "max_entry_share_price",   s.max_entry_share_price);
    s.ideal_entry_share_price = get_or<double>(st, "ideal_entry_share_price", s.ideal_entry_share_price);
    s.hard_share_price_ceil   = get_or<double>(st, "hard_share_price_ceil",   s.hard_share_price_ceil);
    // max_entry_share_price used to be display-only while rule paths consulted
    // hard_share_price_ceil. Make it the universal operator cap while preserving
    // any stricter hard ceiling.
    s.hard_share_price_ceil   = std::min(s.hard_share_price_ceil,
                                         s.max_entry_share_price);
    s.max_gap_btc_usd         = get_or<double>(st, "max_gap_btc_usd",         s.max_gap_btc_usd);
    s.max_gap_eth_usd         = get_or<double>(st, "max_gap_eth_usd",         s.max_gap_eth_usd);
    s.min_secs_remaining_5m   = get_or<int>   (st, "min_secs_remaining_5m",   s.min_secs_remaining_5m);
    s.min_secs_remaining_15m  = get_or<int>   (st, "min_secs_remaining_15m",  s.min_secs_remaining_15m);
    s.min_secs_remaining_60m  = get_or<int>   (st, "min_secs_remaining_60m",  s.min_secs_remaining_60m);
    s.max_secs_remaining_5m   = get_or<int>   (st, "max_secs_remaining_5m",   s.max_secs_remaining_5m);
    s.max_secs_remaining_15m  = get_or<int>   (st, "max_secs_remaining_15m",  s.max_secs_remaining_15m);
    s.max_secs_remaining_60m  = get_or<int>   (st, "max_secs_remaining_60m",  s.max_secs_remaining_60m);
    s.per_click_size_usd      = get_or<double>(st, "per_click_size_usd",      s.per_click_size_usd);
    s.min_order_usd           = get_or<double>(st, "min_order_usd",           s.min_order_usd);
    s.max_per_window_usd      = get_or<double>(st, "max_per_window_usd",      s.max_per_window_usd);
    // Autopilot order-size clamp [min_size_usd, max_size_usd]. These were never
    // read from the TOML, so they stayed pinned at the 5.00 default and the
    // autopilot clamped every order to $5 regardless of per_click_size_usd.
    s.min_size_usd            = get_or<double>(st, "min_size_usd",            s.min_size_usd);
    s.max_size_usd            = get_or<double>(st, "max_size_usd",            s.max_size_usd);
    s.equity_sizing_enabled   = get_or<bool>  (st, "equity_sizing_enabled",   s.equity_sizing_enabled);
    s.equity_order_pct        = get_or<double>(st, "equity_order_pct",        s.equity_order_pct);
    s.equity_max_deployed_pct = get_or<double>(st, "equity_max_deployed_pct", s.equity_max_deployed_pct);
    s.autopilot_conf_floor    = get_or<double>(st, "autopilot_conf_floor",    s.autopilot_conf_floor);
    s.autopilot_throttle_ms   = get_or<int>   (st, "autopilot_throttle_ms",   s.autopilot_throttle_ms);
    s.autopilot_max_per_sweep = get_or<int>   (st, "autopilot_max_per_sweep", s.autopilot_max_per_sweep);
    s.max_entries_per_window  = get_or<int>   (st, "max_entries_per_window",  s.max_entries_per_window);
    s.max_same_side_positions = get_or<int>   (st, "max_same_side_positions", s.max_same_side_positions);
    s.trend_block_pct         = get_or<double>(st, "trend_block_pct",         s.trend_block_pct);
    s.trend_lookback_secs     = get_or<double>(st, "trend_lookback_secs",     s.trend_lookback_secs);
    s.entry_cross_ticks       = get_or<double>(st, "entry_cross_ticks",       s.entry_cross_ticks);
    s.recycler_mode = get_or<std::string>(st, "recycler_mode", s.recycler_mode);
    if (st && (*st)["recycler_mode"] && !(*st)["recycler_mode"].value<std::string>()) throw std::runtime_error("invalid type: recycler_mode");
    s.recycler_maniac_mode = get_or<bool>(st, "recycler_maniac_mode", s.recycler_maniac_mode);
    if (st && (*st)["recycler_maniac_mode"] && !(*st)["recycler_maniac_mode"].value<bool>()) throw std::runtime_error("invalid type: recycler_maniac_mode");
    s.recycler_entry_price = get_or<double>(st, "recycler_entry_price", s.recycler_entry_price);
    if (st && (*st)["recycler_entry_price"] && !(*st)["recycler_entry_price"].value<double>()) throw std::runtime_error("invalid type: recycler_entry_price");
    s.recycler_target_price = get_or<double>(st, "recycler_target_price", s.recycler_target_price);
    if (st && (*st)["recycler_target_price"] && !(*st)["recycler_target_price"].value<double>()) throw std::runtime_error("invalid type: recycler_target_price");
    s.recycler_entry_min_price = get_or<double>(st, "recycler_entry_min_price", s.recycler_entry_min_price);
    if (st && (*st)["recycler_entry_min_price"] && !(*st)["recycler_entry_min_price"].value<double>()) throw std::runtime_error("invalid type: recycler_entry_min_price");
    s.recycler_entry_max_price = get_or<double>(st, "recycler_entry_max_price", s.recycler_entry_max_price);
    if (st && (*st)["recycler_entry_max_price"] && !(*st)["recycler_entry_max_price"].value<double>()) throw std::runtime_error("invalid type: recycler_entry_max_price");
    s.recycler_target_profit_pct = get_or<double>(st, "recycler_target_profit_pct", s.recycler_target_profit_pct);
    if (st && (*st)["recycler_target_profit_pct"] && !(*st)["recycler_target_profit_pct"].value<double>()) throw std::runtime_error("invalid type: recycler_target_profit_pct");
    s.recycler_momentum_lookback_secs = get_or<int>(st, "recycler_momentum_lookback_secs", s.recycler_momentum_lookback_secs);
    if (st && (*st)["recycler_momentum_lookback_secs"] && !(*st)["recycler_momentum_lookback_secs"].value<int>()) throw std::runtime_error("invalid type: recycler_momentum_lookback_secs");
    s.recycler_min_momentum_ticks = get_or<int>(st, "recycler_min_momentum_ticks", s.recycler_min_momentum_ticks);
    if (st && (*st)["recycler_min_momentum_ticks"] && !(*st)["recycler_min_momentum_ticks"].value<int>()) throw std::runtime_error("invalid type: recycler_min_momentum_ticks");
    s.recycler_stop_ticks = get_or<int>(st, "recycler_stop_ticks", s.recycler_stop_ticks);
    if (st && (*st)["recycler_stop_ticks"] && !(*st)["recycler_stop_ticks"].value<int>()) throw std::runtime_error("invalid type: recycler_stop_ticks");
    s.recycler_max_hold_secs = get_or<int>(st, "recycler_max_hold_secs", s.recycler_max_hold_secs);
    if (st && (*st)["recycler_max_hold_secs"] && !(*st)["recycler_max_hold_secs"].value<int>()) throw std::runtime_error("invalid type: recycler_max_hold_secs");
    s.recycler_entry_cutoff_secs = get_or<int>(st, "recycler_entry_cutoff_secs", s.recycler_entry_cutoff_secs);
    if (st && (*st)["recycler_entry_cutoff_secs"] && !(*st)["recycler_entry_cutoff_secs"].value<int>()) throw std::runtime_error("invalid type: recycler_entry_cutoff_secs");
    s.recycler_max_cycles = get_or<int>(st, "recycler_max_cycles", s.recycler_max_cycles);
    if (st && (*st)["recycler_max_cycles"] && !(*st)["recycler_max_cycles"].value<int>()) throw std::runtime_error("invalid type: recycler_max_cycles");
    s.recycler_lookback_secs = get_or<int>(st, "recycler_lookback_secs", s.recycler_lookback_secs);
    if (st && (*st)["recycler_lookback_secs"] && !(*st)["recycler_lookback_secs"].value<int>()) throw std::runtime_error("invalid type: recycler_lookback_secs");
    s.recycler_min_range = get_or<double>(st, "recycler_min_range", s.recycler_min_range);
    if (st && (*st)["recycler_min_range"] && !(*st)["recycler_min_range"].value<double>()) throw std::runtime_error("invalid type: recycler_min_range");
    s.recycler_max_window_loss = get_or<double>(st, "recycler_max_window_loss", s.recycler_max_window_loss);
    if (st && (*st)["recycler_max_window_loss"] && !(*st)["recycler_max_window_loss"].value<double>()) throw std::runtime_error("invalid type: recycler_max_window_loss");
    s.recycler_max_session_loss = get_or<double>(st, "recycler_max_session_loss", s.recycler_max_session_loss);
    if (st && (*st)["recycler_max_session_loss"] && !(*st)["recycler_max_session_loss"].value<double>()) throw std::runtime_error("invalid type: recycler_max_session_loss");
    s.recycler_min_settlement_edge_pct = get_or<double>(st, "recycler_min_settlement_edge_pct", s.recycler_min_settlement_edge_pct);
    if (st && (*st)["recycler_min_settlement_edge_pct"] && !(*st)["recycler_min_settlement_edge_pct"].value<double>()) throw std::runtime_error("invalid type: recycler_min_settlement_edge_pct");
    s.recycler_reference_max_age_ms = get_or<int>(st, "recycler_reference_max_age_ms", s.recycler_reference_max_age_ms);
    if (st && (*st)["recycler_reference_max_age_ms"] && !(*st)["recycler_reference_max_age_ms"].value<int>()) throw std::runtime_error("invalid type: recycler_reference_max_age_ms");
    s.recycler_late_risk_exit_enabled = get_or<bool>(st, "recycler_late_risk_exit_enabled", s.recycler_late_risk_exit_enabled);
    if (st && (*st)["recycler_late_risk_exit_enabled"] && !(*st)["recycler_late_risk_exit_enabled"].value<bool>()) throw std::runtime_error("invalid type: recycler_late_risk_exit_enabled");
    s.recycler_late_risk_min_prob = get_or<double>(st, "recycler_late_risk_min_prob", s.recycler_late_risk_min_prob);
    if (st && (*st)["recycler_late_risk_min_prob"] && !(*st)["recycler_late_risk_min_prob"].value<double>()) throw std::runtime_error("invalid type: recycler_late_risk_min_prob");
    s.recycler_late_risk_bid_loss_pct = get_or<double>(st, "recycler_late_risk_bid_loss_pct", s.recycler_late_risk_bid_loss_pct);
    if (st && (*st)["recycler_late_risk_bid_loss_pct"] && !(*st)["recycler_late_risk_bid_loss_pct"].value<double>()) throw std::runtime_error("invalid type: recycler_late_risk_bid_loss_pct");
    s.recycler_late_target_6m_pct = get_or<double>(st, "recycler_late_target_6m_pct", s.recycler_late_target_6m_pct);
    if (st && (*st)["recycler_late_target_6m_pct"] && !(*st)["recycler_late_target_6m_pct"].value<double>()) throw std::runtime_error("invalid type: recycler_late_target_6m_pct");
    s.recycler_late_target_4m_pct = get_or<double>(st, "recycler_late_target_4m_pct", s.recycler_late_target_4m_pct);
    if (st && (*st)["recycler_late_target_4m_pct"] && !(*st)["recycler_late_target_4m_pct"].value<double>()) throw std::runtime_error("invalid type: recycler_late_target_4m_pct");
    s.recycler_late_target_2m_pct = get_or<double>(st, "recycler_late_target_2m_pct", s.recycler_late_target_2m_pct);
    if (st && (*st)["recycler_late_target_2m_pct"] && !(*st)["recycler_late_target_2m_pct"].value<double>()) throw std::runtime_error("invalid type: recycler_late_target_2m_pct");
    s.recycler_late_maker_secs = get_or<int>(st, "recycler_late_maker_secs", s.recycler_late_maker_secs);
    if (st && (*st)["recycler_late_maker_secs"] && !(*st)["recycler_late_maker_secs"].value<int>()) throw std::runtime_error("invalid type: recycler_late_maker_secs");
    s.recycler_late_taker_secs = get_or<int>(st, "recycler_late_taker_secs", s.recycler_late_taker_secs);
    if (st && (*st)["recycler_late_taker_secs"] && !(*st)["recycler_late_taker_secs"].value<int>()) throw std::runtime_error("invalid type: recycler_late_taker_secs");
    s.recycler_late_taker_exit_enabled = get_or<bool>(st, "recycler_late_taker_exit_enabled", s.recycler_late_taker_exit_enabled);
    if (st && (*st)["recycler_late_taker_exit_enabled"] && !(*st)["recycler_late_taker_exit_enabled"].value<bool>()) throw std::runtime_error("invalid type: recycler_late_taker_exit_enabled");
    s.recycler_settlement_entry_enabled = get_or<bool>(st, "recycler_settlement_entry_enabled", s.recycler_settlement_entry_enabled);
    if (st && (*st)["recycler_settlement_entry_enabled"] && !(*st)["recycler_settlement_entry_enabled"].value<bool>()) throw std::runtime_error("invalid type: recycler_settlement_entry_enabled");
    s.recycler_settlement_entry_secs = get_or<int>(st, "recycler_settlement_entry_secs", s.recycler_settlement_entry_secs);
    if (st && (*st)["recycler_settlement_entry_secs"] && !(*st)["recycler_settlement_entry_secs"].value<int>()) throw std::runtime_error("invalid type: recycler_settlement_entry_secs");
    s.recycler_settlement_entry_min_price = get_or<double>(st, "recycler_settlement_entry_min_price", s.recycler_settlement_entry_min_price);
    if (st && (*st)["recycler_settlement_entry_min_price"] && !(*st)["recycler_settlement_entry_min_price"].value<double>()) throw std::runtime_error("invalid type: recycler_settlement_entry_min_price");
    s.recycler_settlement_entry_max_price = get_or<double>(st, "recycler_settlement_entry_max_price", s.recycler_settlement_entry_max_price);
    if (st && (*st)["recycler_settlement_entry_max_price"] && !(*st)["recycler_settlement_entry_max_price"].value<double>()) throw std::runtime_error("invalid type: recycler_settlement_entry_max_price");
    s.recycler_settlement_only = get_or<bool>(st, "recycler_settlement_only", s.recycler_settlement_only);
    if (st && (*st)["recycler_settlement_only"] && !(*st)["recycler_settlement_only"].value<bool>()) throw std::runtime_error("invalid type: recycler_settlement_only");
    s.recycler_entry_open_grace_secs = get_or<int>(st, "recycler_entry_open_grace_secs", s.recycler_entry_open_grace_secs);
    if (st && (*st)["recycler_entry_open_grace_secs"] && !(*st)["recycler_entry_open_grace_secs"].value<int>()) throw std::runtime_error("invalid type: recycler_entry_open_grace_secs");
    s.recycler_first_entry_scale_pct = get_or<double>(st, "recycler_first_entry_scale_pct", s.recycler_first_entry_scale_pct);
    if (st && (*st)["recycler_first_entry_scale_pct"] && !(*st)["recycler_first_entry_scale_pct"].value<double>()) throw std::runtime_error("invalid type: recycler_first_entry_scale_pct");
    s.recycler_late_taker_min_bid = get_or<double>(st, "recycler_late_taker_min_bid", s.recycler_late_taker_min_bid);
    if (st && (*st)["recycler_late_taker_min_bid"] && !(*st)["recycler_late_taker_min_bid"].value<double>()) throw std::runtime_error("invalid type: recycler_late_taker_min_bid");
    if (s.recycler_mode!="off" && s.recycler_mode!="sniper" && s.recycler_mode!="range")
        throw std::runtime_error("recycler_mode must be off, sniper, or range");
    if (s.recycler_mode!="off" && c.runtime.mode!=TradeMode::Paper)
        throw std::runtime_error("Recycler strategies support PAPER only; LIVE passive sells/reconciliation unavailable");
    const int recycler_max_cycles = s.recycler_maniac_mode ? 100 : 15;
    const int recycler_max_cutoff = s.recycler_maniac_mode ? 86400 : 600;
    const int recycler_max_open_grace = s.recycler_maniac_mode ? 3600 : 600;
    if (!(s.recycler_entry_price>=0.10 && s.recycler_entry_price<s.recycler_target_price && s.recycler_target_price<=0.90) ||
        !(s.recycler_entry_min_price>=0.01 && s.recycler_entry_min_price<s.recycler_entry_max_price && s.recycler_entry_max_price<=0.99) ||
        !(s.recycler_target_profit_pct>=5 && s.recycler_target_profit_pct<=50) ||
        s.recycler_late_target_6m_pct>s.recycler_target_profit_pct ||
        s.recycler_momentum_lookback_secs<1 || s.recycler_momentum_lookback_secs>10 ||
        s.recycler_min_momentum_ticks<1 || s.recycler_min_momentum_ticks>10 ||
        s.recycler_stop_ticks<0 || s.recycler_stop_ticks>30 || s.recycler_max_cycles<1 || s.recycler_max_cycles>recycler_max_cycles ||
        s.recycler_entry_cutoff_secs<1 || s.recycler_entry_cutoff_secs>recycler_max_cutoff ||
        s.recycler_max_hold_secs<0 || s.recycler_max_hold_secs>600 || s.recycler_lookback_secs<10 || s.recycler_lookback_secs>300 ||
        !(s.recycler_min_range>=0.02 && s.recycler_min_range<=0.40) ||
        !(s.recycler_max_window_loss>=0 && s.recycler_max_window_loss<=100) ||
        !(s.recycler_max_session_loss>=0 && s.recycler_max_session_loss<=1000) ||
        !(s.recycler_min_settlement_edge_pct>=0 && s.recycler_min_settlement_edge_pct<=50) ||
        s.recycler_reference_max_age_ms<100 || s.recycler_reference_max_age_ms>30000 ||
        !(s.recycler_late_risk_min_prob>=0 && s.recycler_late_risk_min_prob<=1) ||
        !(s.recycler_late_risk_bid_loss_pct>=0 && s.recycler_late_risk_bid_loss_pct<=100) ||
        !(s.recycler_late_target_6m_pct>=0 && s.recycler_late_target_6m_pct<=50) ||
        !(s.recycler_late_target_4m_pct>=0 && s.recycler_late_target_4m_pct<=s.recycler_late_target_6m_pct) ||
        !(s.recycler_late_target_2m_pct>=0 && s.recycler_late_target_2m_pct<=s.recycler_late_target_4m_pct) ||
        s.recycler_late_taker_secs<10 || s.recycler_late_maker_secs<=s.recycler_late_taker_secs ||
        s.recycler_late_maker_secs>360 || s.recycler_late_taker_secs>120 ||
        s.recycler_settlement_entry_secs<1 || s.recycler_settlement_entry_secs>120 ||
        s.recycler_entry_open_grace_secs<0 || s.recycler_entry_open_grace_secs>recycler_max_open_grace ||
        !(s.recycler_first_entry_scale_pct>=1 && s.recycler_first_entry_scale_pct<=100) ||
        !(s.recycler_late_taker_min_bid>=0 && s.recycler_late_taker_min_bid<=0.99) ||
        !(s.recycler_settlement_entry_min_price>=0.85 && s.recycler_settlement_entry_min_price<s.recycler_settlement_entry_max_price && s.recycler_settlement_entry_max_price<=0.98) ||
        (s.recycler_max_window_loss>0 && s.recycler_max_session_loss>0 &&
         s.recycler_max_session_loss<s.recycler_max_window_loss))
        throw std::runtime_error("invalid recycler price, timing, cycle, or loss constraints");
    if(s.recycler_mode!="off") {
        std::vector<int> recycler_horizons;
        for(const int horizon : c.polymarket.universe_horizons_seconds) {
            const bool blocked = std::find(c.polymarket.blocked_horizons_seconds.begin(),
                                           c.polymarket.blocked_horizons_seconds.end(),
                                           horizon) != c.polymarket.blocked_horizons_seconds.end();
            const bool supported = horizon==300 || horizon==900 ||
                (s.recycler_maniac_mode && (horizon==3600 || horizon==14400 || horizon==86400));
            if(!blocked && supported)
                recycler_horizons.push_back(horizon);
        }
        std::sort(recycler_horizons.begin(),recycler_horizons.end());
        recycler_horizons.erase(std::unique(recycler_horizons.begin(),recycler_horizons.end()),
                                recycler_horizons.end());
        if(s.recycler_settlement_only) {
            if(recycler_horizons.empty() || recycler_horizons.size()>2 ||
               std::any_of(recycler_horizons.begin(),recycler_horizons.end(),
                           [](int h){return h!=300 && h!=900;}))
                throw std::runtime_error("settlement-only profiles must select one or both 5m/15m horizons");
        } else if(recycler_horizons.size()!=1)
            throw std::runtime_error(s.recycler_maniac_mode
                ? "active Maniac Maker Recycler profiles must select exactly one unblocked 5m, 15m, 1h, 4h, or 24h horizon"
                : "active Recycler profiles must select exactly one unblocked 5m or 15m horizon");
        if(!s.recycler_settlement_only) {
            const int horizon_secs=recycler_horizons.front();
            if(s.recycler_entry_cutoff_secs>=horizon_secs)
                throw std::runtime_error("Recycler entry cutoff would block the entire configured window");
            if(!s.recycler_maniac_mode && horizon_secs==900 && s.recycler_entry_cutoff_secs<360)
                throw std::runtime_error("15m Recycler profiles must stop entries with at least 360 seconds remaining");
            if(!s.recycler_maniac_mode && horizon_secs==300 && s.recycler_late_maker_secs>=s.recycler_entry_cutoff_secs)
                throw std::runtime_error("5m Recycler late maker exit must begin after the entry cutoff");
        }
        if(s.recycler_maniac_mode && s.recycler_settlement_entry_enabled &&
           s.recycler_entry_cutoff_secs<=s.recycler_settlement_entry_secs)
            throw std::runtime_error("Maniac Maker cycle-entry cutoff must leave a separate late settlement-entry phase");
    }
    if (s.recycler_settlement_entry_enabled && s.recycler_mode=="off")
        throw std::runtime_error("recycler settlement entry requires recycler_mode sniper or range");
    if (s.recycler_settlement_only && !s.recycler_settlement_entry_enabled)
        throw std::runtime_error("settlement-only profile requires recycler_settlement_entry_enabled=true");
    if (s.recycler_mode!="off" && s.recycler_stop_ticks>0)
        spdlog::warn("recycler price stop is disabled by operator policy; recycler_stop_ticks={} is retained but ignored",s.recycler_stop_ticks);
    s.entry_order_mode        = get_or<std::string>(st, "entry_order_mode",   s.entry_order_mode);
    if (s.entry_order_mode != "taker" && s.entry_order_mode != "maker" &&
        s.entry_order_mode != "hybrid")
        throw std::runtime_error("strategy.entry_order_mode must be taker, maker, or hybrid");
    if (s.entry_order_mode != "maker")
        spdlog::warn("strategy.entry_order_mode={} ignored: this build routes all orders as post-only maker",
                     s.entry_order_mode);
    s.entry_order_mode = "maker";
    s.maker_improve_ticks     = get_or<int>(st, "maker_improve_ticks", s.maker_improve_ticks);
    s.maker_reprice_ms        = get_or<int>(st, "maker_reprice_ms", s.maker_reprice_ms);
    s.maker_poll_ms           = get_or<int>(st, "maker_poll_ms", s.maker_poll_ms);
    s.maker_max_age_ms        = get_or<int>(st, "maker_max_age_ms", s.maker_max_age_ms);
    s.maker_cancel_before_close_ms = get_or<int>(
        st, "maker_cancel_before_close_ms", s.maker_cancel_before_close_ms);
    s.manual_buy_slippage_pct = get_or<double>(st, "manual_buy_slippage_pct", s.manual_buy_slippage_pct);
    s.manual_buy_slippage_extra =
        get_or<double>(st, "manual_buy_slippage_extra", s.manual_buy_slippage_extra);
    s.order_cooldown_ms       = get_or<int>   (st, "order_cooldown_ms",       s.order_cooldown_ms);
    s.token_quarantine_payload_ms =
        get_or<int>(st, "token_quarantine_payload_ms", s.token_quarantine_payload_ms);
    s.token_quarantine_no_match_ms =
        get_or<int>(st, "token_quarantine_no_match_ms", s.token_quarantine_no_match_ms);
    s.min_position_shares     = get_or<double>(st, "min_position_shares",     s.min_position_shares);
    s.dust_position_shares    = get_or<double>(st, "dust_position_shares",    s.dust_position_shares);
    s.price_history_max_points =
        get_or<int>(st, "price_history_max_points", s.price_history_max_points);
    s.price_velocity_lookback_ms =
        get_or<int>(st, "price_velocity_lookback_ms", s.price_velocity_lookback_ms);
    s.sparkline_points =
        get_or<int>(st, "sparkline_points", s.sparkline_points);
    s.tick_buffer_max_age_us =
        get_or<int64_t>(st, "tick_buffer_max_age_us", s.tick_buffer_max_age_us);
    s.tick_buffer_max_entries =
        get_or<int>(st, "tick_buffer_max_entries", s.tick_buffer_max_entries);
    s.consecutive_loss_pause  = get_or<int>   (st, "consecutive_loss_pause",  s.consecutive_loss_pause);
    s.consecutive_pause_secs  = get_or<int>   (st, "consecutive_pause_secs",  s.consecutive_pause_secs);
    s.oracle_lag_mode         = get_or<bool>  (st, "oracle_lag_mode",         s.oracle_lag_mode);
    s.min_oracle_delta_pct    = get_or<double>(st, "min_oracle_delta_pct",    s.min_oracle_delta_pct);
    s.coinbase_lead_mode      = get_or<bool>  (st, "coinbase_lead_mode",      s.coinbase_lead_mode);
    s.coinbase_veto_mode      = get_or<bool>  (st, "coinbase_veto_mode",      s.coinbase_veto_mode);
    s.coinbase_lead_min_delta_pct = get_or<double>(st, "coinbase_lead_min_delta_pct", s.coinbase_lead_min_delta_pct);
    s.binance_book_confirm      = get_or<bool>  (st, "binance_book_confirm",     s.binance_book_confirm);
    s.binance_book_imb_min      = get_or<double>(st, "binance_book_imb_min",    s.binance_book_imb_min);
    s.entry_persist_gate        = get_or<bool>  (st, "entry_persist_gate",       s.entry_persist_gate);
    s.entry_persist_ms          = get_or<int>   (st, "entry_persist_ms",         s.entry_persist_ms);
    s.entry_early_edge_mult     = get_or<double>(st, "entry_early_edge_mult",    s.entry_early_edge_mult);
    s.entry_mid_edge_mult       = get_or<double>(st, "entry_mid_edge_mult",      s.entry_mid_edge_mult);
    s.entry_late_edge_mult      = get_or<double>(st, "entry_late_edge_mult",     s.entry_late_edge_mult);
    s.digital_fv_mode           = get_or<bool>  (st, "digital_fv_mode",          s.digital_fv_mode);
    s.digital_fv_use_coinbase   = get_or<bool>  (st, "digital_fv_use_coinbase",  s.digital_fv_use_coinbase);
    s.digital_fv_min_edge_pct   = get_or<double>(st, "digital_fv_min_edge_pct",  s.digital_fv_min_edge_pct);
    s.digital_fv_fee_buffer     = get_or<double>(st, "digital_fv_fee_buffer",    s.digital_fv_fee_buffer);
    s.digital_fv_down_slip_pct  = get_or<double>(st, "digital_fv_down_slip_pct", s.digital_fv_down_slip_pct);
    s.digital_fv_vol_floor      = get_or<double>(st, "digital_fv_vol_floor",     s.digital_fv_vol_floor);
    s.digital_fv_vol_ceil       = get_or<double>(st, "digital_fv_vol_ceil",      s.digital_fv_vol_ceil);
    s.digital_fv_jump_z         = get_or<double>(st, "digital_fv_jump_z",        s.digital_fv_jump_z);
    s.multi_source_vote         = get_or<bool>  (st, "multi_source_vote",        s.multi_source_vote);
    s.multi_source_quorum       = get_or<int>   (st, "multi_source_quorum",      s.multi_source_quorum);
    s.midcycle_mode             = get_or<bool>  (st, "midcycle_mode",             s.midcycle_mode);
    s.midcycle_trigger_secs     = get_or<int>   (st, "midcycle_trigger_secs",     s.midcycle_trigger_secs);
    s.midcycle_min_distance_bps = get_or<double>(st, "midcycle_min_distance_bps", s.midcycle_min_distance_bps);
    s.midcycle_max_share_price  = get_or<double>(st, "midcycle_max_share_price",  s.midcycle_max_share_price);
    s.midcycle_window_s         = get_or<int>   (st, "midcycle_window_s",         s.midcycle_window_s);
    s.book_imb_filter           = get_or<bool>  (st, "book_imb_filter",           s.book_imb_filter);
    s.book_imb_min              = get_or<double>(st, "book_imb_min",              s.book_imb_min);
    s.smart_exit_mode           = get_or<bool>  (st, "smart_exit_mode",           s.smart_exit_mode);
    s.conviction_min_delta_pct  = get_or<double>(st, "conviction_min_delta_pct",  s.conviction_min_delta_pct);
    s.smart_stop_max_votes      = get_or<int>   (st, "smart_stop_max_votes",      s.smart_stop_max_votes);
    s.hard_stop_pct             = get_or<double>(st, "hard_stop_pct",             s.hard_stop_pct);
    s.ride_to_settle_secs       = get_or<int>   (st, "ride_to_settle_secs",       s.ride_to_settle_secs);
    s.oracle_max_token_price  = get_or<double>(st, "oracle_max_token_price",  s.oracle_max_token_price);
    s.blocked_assets.clear();
    if (st) {
        if (auto arr = (*st)["blocked_assets"].as_array()) {
            for (auto& v : *arr)
                if (auto a = v.value<std::string>()) s.blocked_assets.push_back(*a);
        }
    }
    s.autopilot_on_start      = get_or<bool>  (st, "autopilot_on_start",      s.autopilot_on_start);
    s.autopilot_startup_delay_secs = get_or<int> (st, "autopilot_startup_delay_secs", s.autopilot_startup_delay_secs);
    s.predictor_mode          = get_or<bool>  (st, "predictor_mode",          s.predictor_mode);
    s.min_predictor_edge      = get_or<double>(st, "min_predictor_edge",      s.min_predictor_edge);
    s.min_predictor_conf      = get_or<double>(st, "min_predictor_conf",      s.min_predictor_conf);
    s.predictor_max_token_price = get_or<double>(st, "predictor_max_token_price", s.predictor_max_token_price);
    s.predictor_no_oracle_conf= get_or<double>(st, "predictor_no_oracle_conf",s.predictor_no_oracle_conf);
    s.profit_take_mode        = get_or<bool>  (st, "profit_take_mode",        s.profit_take_mode);
    s.profit_take_min_pct     = get_or<double>(st, "profit_take_min_pct",     s.profit_take_min_pct);
    s.profit_take_momentum_max= get_or<double>(st, "profit_take_momentum_max",s.profit_take_momentum_max);
    s.profit_take_price_thresh= get_or<double>(st, "profit_take_price_thresh",s.profit_take_price_thresh);
    s.stop_loss_mode          = get_or<bool>  (st, "stop_loss_mode",          s.stop_loss_mode);
    s.stop_loss_pct           = get_or<double>(st, "stop_loss_pct",           s.stop_loss_pct);
    s.stop_loss_price         = get_or<double>(st, "stop_loss_price",         s.stop_loss_price);
    s.timed_exit_secs         = get_or<int>   (st, "timed_exit_secs",         s.timed_exit_secs);
    s.endgame_mode            = get_or<bool>  (st, "endgame_mode",            s.endgame_mode);
    s.endgame_secs_left       = get_or<int>   (st, "endgame_secs_left",       s.endgame_secs_left);
    s.endgame_min_profit_pct  = get_or<double>(st, "endgame_min_profit_pct",  s.endgame_min_profit_pct);
    s.endgame_max_price       = get_or<double>(st, "endgame_max_price",       s.endgame_max_price);
    s.endgame_min_price       = get_or<double>(st, "endgame_min_price",       s.endgame_min_price);
    s.endgame_min_oracle_pct  = get_or<double>(st, "endgame_min_oracle_pct",  s.endgame_min_oracle_pct);
    s.taker_fee_rate          = get_or<double>(st, "taker_fee_rate",          s.taker_fee_rate);
    s.fee_exponent            = get_or<int>   (st, "fee_exponent",            s.fee_exponent);
    s.scalp_v2_enabled              = get_or<bool>  (st, "scalp_v2_enabled",               s.scalp_v2_enabled);
    s.scalp_v2_velocity_thresh      = get_or<double>(st, "scalp_v2_velocity_thresh",       s.scalp_v2_velocity_thresh);
    s.scalp_v2_min_price            = get_or<double>(st, "scalp_v2_min_price",             s.scalp_v2_min_price);
    s.scalp_v2_max_price            = get_or<double>(st, "scalp_v2_max_price",             s.scalp_v2_max_price);
    s.scalp_v2_min_secs             = get_or<int>   (st, "scalp_v2_min_secs",              s.scalp_v2_min_secs);
    s.scalp_v2_max_secs             = get_or<int>   (st, "scalp_v2_max_secs",              s.scalp_v2_max_secs);
    s.scalp_v2_max_entries_per_window = get_or<int>(st, "scalp_v2_max_entries_per_window", s.scalp_v2_max_entries_per_window);
    s.scalp_v2_per_entry_usd        = get_or<double>(st, "scalp_v2_per_entry_usd",         s.scalp_v2_per_entry_usd);
    s.scalp_v2_reversal_exit        = get_or<bool>  (st, "scalp_v2_reversal_exit",         s.scalp_v2_reversal_exit);
    s.scalp_v2_reversal_thresh      = get_or<double>(st, "scalp_v2_reversal_thresh",       s.scalp_v2_reversal_thresh);
    s.scalp_v2_reversal_window_us   = get_or<int>   (st, "scalp_v2_reversal_window_us",    s.scalp_v2_reversal_window_us);
    s.scalp_v2_trailing_stop        = get_or<bool>  (st, "scalp_v2_trailing_stop",         s.scalp_v2_trailing_stop);
    s.scalp_v2_trail_start_loss     = get_or<double>(st, "scalp_v2_trail_start_loss",      s.scalp_v2_trail_start_loss);
    s.scalp_v2_trail_ratchet_pct    = get_or<double>(st, "scalp_v2_trail_ratchet_pct",     s.scalp_v2_trail_ratchet_pct);
    s.scalp_v2_trail_min_distance   = get_or<double>(st, "scalp_v2_trail_min_distance",    s.scalp_v2_trail_min_distance);
    s.scalp_v2_profit_take          = get_or<bool>  (st, "scalp_v2_profit_take",           s.scalp_v2_profit_take);
    s.scalp_v2_profit_target        = get_or<double>(st, "scalp_v2_profit_target",         s.scalp_v2_profit_target);
    s.scalp_v2_min_hold_ms          = get_or<int>   (st, "scalp_v2_min_hold_ms",           s.scalp_v2_min_hold_ms);
    s.scalp_v3_enabled              = get_or<bool>  (st, "scalp_v3_enabled",               s.scalp_v3_enabled);
    s.scalp_v3_require_oracle_agree = get_or<bool>  (st, "scalp_v3_require_oracle_agree",  s.scalp_v3_require_oracle_agree);
    s.scalp_v3_blocked_assets.clear();
    if (st) {
        if (auto arr = (*st)["scalp_v3_blocked_assets"].as_array()) {
            for (auto& v : *arr)
                if (auto a = v.value<std::string>()) s.scalp_v3_blocked_assets.push_back(*a);
        }
    }
    s.scalp_v3_velocity_thresh      = get_or<double>(st, "scalp_v3_velocity_thresh",       s.scalp_v3_velocity_thresh);
    s.scalp_v3_accel_min            = get_or<double>(st, "scalp_v3_accel_min",             s.scalp_v3_accel_min);
    s.scalp_v3_dominance_thresh     = get_or<double>(st, "scalp_v3_dominance_thresh",      s.scalp_v3_dominance_thresh);
    s.scalp_v3_min_confidence       = get_or<double>(st, "scalp_v3_min_confidence",        s.scalp_v3_min_confidence);
    s.scalp_v3_min_price            = get_or<double>(st, "scalp_v3_min_price",             s.scalp_v3_min_price);
    s.scalp_v3_max_price            = get_or<double>(st, "scalp_v3_max_price",             s.scalp_v3_max_price);
    s.scalp_v3_min_secs             = get_or<int>   (st, "scalp_v3_min_secs",              s.scalp_v3_min_secs);
    s.scalp_v3_max_secs             = get_or<int>   (st, "scalp_v3_max_secs",              s.scalp_v3_max_secs);
    s.scalp_v3_max_entries_per_window = get_or<int>(st, "scalp_v3_max_entries_per_window", s.scalp_v3_max_entries_per_window);
    s.scalp_v3_per_entry_usd        = get_or<double>(st, "scalp_v3_per_entry_usd",         s.scalp_v3_per_entry_usd);
    s.scalp_v3_cooldown_ms          = get_or<int>   (st, "scalp_v3_cooldown_ms",           s.scalp_v3_cooldown_ms);
    s.scalp_v3_max_spread_pct       = get_or<double>(st, "scalp_v3_max_spread_pct",        s.scalp_v3_max_spread_pct);
    s.scalp_v3_max_book_age_ms      = get_or<int>   (st, "scalp_v3_max_book_age_ms",       s.scalp_v3_max_book_age_ms);
    s.scalp_v3_reversal_exit        = get_or<bool>  (st, "scalp_v3_reversal_exit",         s.scalp_v3_reversal_exit);
    s.scalp_v3_reversal_thresh      = get_or<double>(st, "scalp_v3_reversal_thresh",       s.scalp_v3_reversal_thresh);
    s.scalp_v3_reversal_window_us   = get_or<int>   (st, "scalp_v3_reversal_window_us",    s.scalp_v3_reversal_window_us);
    s.scalp_v3_reversal_min_profit  = get_or<double>(st, "scalp_v3_reversal_min_profit",   s.scalp_v3_reversal_min_profit);
    s.scalp_v3_min_hold_ms          = get_or<int>   (st, "scalp_v3_min_hold_ms",           s.scalp_v3_min_hold_ms);
    s.scalp_v3_trailing_stop        = get_or<bool>  (st, "scalp_v3_trailing_stop",         s.scalp_v3_trailing_stop);
    s.scalp_v3_trail_start_loss     = get_or<double>(st, "scalp_v3_trail_start_loss",      s.scalp_v3_trail_start_loss);
    s.scalp_v3_trail_ratchet_pct    = get_or<double>(st, "scalp_v3_trail_ratchet_pct",     s.scalp_v3_trail_ratchet_pct);
    s.scalp_v3_trail_min_distance   = get_or<double>(st, "scalp_v3_trail_min_distance",    s.scalp_v3_trail_min_distance);
    s.scalp_v3_trail_stop_pct       = get_or<double>(st, "scalp_v3_trail_stop_pct",         s.scalp_v3_trail_stop_pct);
    s.scalp_v3_profit_take          = get_or<bool>  (st, "scalp_v3_profit_take",           s.scalp_v3_profit_take);
    s.scalp_v3_profit_target        = get_or<double>(st, "scalp_v3_profit_target",         s.scalp_v3_profit_target);
    s.scalp_v3_profit_target_pct    = get_or<double>(st, "scalp_v3_profit_target_pct",     s.scalp_v3_profit_target_pct);
    s.scalp_v3_runner_enabled       = get_or<bool>  (st, "scalp_v3_runner_enabled",        s.scalp_v3_runner_enabled);
    s.scalp_v3_runner_min_bid       = get_or<double>(st, "scalp_v3_runner_min_bid",        s.scalp_v3_runner_min_bid);
    s.scalp_v3_runner_min_profit    = get_or<double>(st, "scalp_v3_runner_min_profit",     s.scalp_v3_runner_min_profit);
    s.scalp_v3_runner_max_secs_left = get_or<int>   (st, "scalp_v3_runner_max_secs_left",  s.scalp_v3_runner_max_secs_left);
    s.scalp_v3_runner_velocity_floor= get_or<double>(st, "scalp_v3_runner_velocity_floor", s.scalp_v3_runner_velocity_floor);
    s.lead_lag_scalp_enabled = get_or<bool>(st, "lead_lag_scalp_enabled", s.lead_lag_scalp_enabled);
    s.lead_lag_scalp_impulse_only = get_or<bool>(
        st, "lead_lag_scalp_impulse_only", s.lead_lag_scalp_impulse_only);
    s.lead_lag_scalp_impulse_require_value_edge = get_or<bool>(
        st, "lead_lag_scalp_impulse_require_value_edge",
        s.lead_lag_scalp_impulse_require_value_edge);
    s.lead_lag_scalp_model_value_enabled = get_or<bool>(
        st, "lead_lag_scalp_model_value_enabled", s.lead_lag_scalp_model_value_enabled);
    s.lead_lag_scalp_horizon_s = get_or<int>(st, "lead_lag_scalp_horizon_s", s.lead_lag_scalp_horizon_s);
    s.lead_lag_scalp_min_signal_confidence = get_or<double>(
        st, "lead_lag_scalp_min_signal_confidence", s.lead_lag_scalp_min_signal_confidence);
    s.lead_lag_scalp_min_model_confidence = get_or<double>(
        st, "lead_lag_scalp_min_model_confidence", s.lead_lag_scalp_min_model_confidence);
    s.lead_lag_scalp_min_cb_move_bps = get_or<double>(st, "lead_lag_scalp_min_cb_move_bps", s.lead_lag_scalp_min_cb_move_bps);
    s.lead_lag_scalp_min_cb_lead_bps = get_or<double>(st, "lead_lag_scalp_min_cb_lead_bps", s.lead_lag_scalp_min_cb_lead_bps);
    s.lead_lag_scalp_max_oracle_counter_bps = get_or<double>(st, "lead_lag_scalp_max_oracle_counter_bps", s.lead_lag_scalp_max_oracle_counter_bps);
    s.lead_lag_scalp_min_net_edge_pct = get_or<double>(st, "lead_lag_scalp_min_net_edge_pct", s.lead_lag_scalp_min_net_edge_pct);
    s.lead_lag_scalp_min_price = get_or<double>(st, "lead_lag_scalp_min_price", s.lead_lag_scalp_min_price);
    s.lead_lag_scalp_max_price = get_or<double>(st, "lead_lag_scalp_max_price", s.lead_lag_scalp_max_price);
    s.lead_lag_scalp_max_spread_pct = get_or<double>(st, "lead_lag_scalp_max_spread_pct", s.lead_lag_scalp_max_spread_pct);
    s.lead_lag_scalp_max_book_age_ms = get_or<int>(st, "lead_lag_scalp_max_book_age_ms", s.lead_lag_scalp_max_book_age_ms);
    s.lead_lag_scalp_stop_entry_secs = get_or<int>(st, "lead_lag_scalp_stop_entry_secs", s.lead_lag_scalp_stop_entry_secs);
    const int default_stop_5m = s.lead_lag_scalp_horizon_s == 300
        ? s.lead_lag_scalp_stop_entry_secs : 90;
    const int default_stop_15m = s.lead_lag_scalp_horizon_s == 900
        ? s.lead_lag_scalp_stop_entry_secs : 270;
    s.lead_lag_scalp_stop_entry_5m_secs = get_or<int>(
        st, "lead_lag_scalp_stop_entry_5m_secs", default_stop_5m);
    s.lead_lag_scalp_stop_entry_15m_secs = get_or<int>(
        st, "lead_lag_scalp_stop_entry_15m_secs", default_stop_15m);
    s.lead_lag_scalp_max_secs_remaining_5m = get_or<int>(
        st, "lead_lag_scalp_max_secs_remaining_5m", 0);
    s.lead_lag_scalp_max_secs_remaining_15m = get_or<int>(
        st, "lead_lag_scalp_max_secs_remaining_15m", 0);
    s.lead_lag_scalp_flatten_secs = get_or<int>(st, "lead_lag_scalp_flatten_secs", s.lead_lag_scalp_flatten_secs);
    // A multi-horizon profile (horizon_s == 0) must retain the per-window
    // safety defaults. The legacy shared flatten setting only overrides the
    // selected horizon for a single-horizon profile.
    const int default_flatten_5m = s.lead_lag_scalp_horizon_s == 300
        ? s.lead_lag_scalp_flatten_secs : 90;
    const int default_flatten_15m = s.lead_lag_scalp_horizon_s == 900
        ? s.lead_lag_scalp_flatten_secs : 270;
    s.lead_lag_scalp_flatten_5m_secs = get_or<int>(
        st, "lead_lag_scalp_flatten_5m_secs", default_flatten_5m);
    s.lead_lag_scalp_flatten_15m_secs = get_or<int>(
        st, "lead_lag_scalp_flatten_15m_secs", default_flatten_15m);
    s.lead_lag_scalp_max_entries_per_window = get_or<int>(st, "lead_lag_scalp_max_entries_per_window", s.lead_lag_scalp_max_entries_per_window);
    s.lead_lag_scalp_order_usd = get_or<double>(st, "lead_lag_scalp_order_usd", s.lead_lag_scalp_order_usd);
    s.lead_lag_scalp_profit_target_pct = get_or<double>(st, "lead_lag_scalp_profit_target_pct", s.lead_lag_scalp_profit_target_pct);
    s.lead_lag_scalp_hard_stop_pct = get_or<double>(st, "lead_lag_scalp_hard_stop_pct", s.lead_lag_scalp_hard_stop_pct);
    s.lead_lag_scalp_hard_stop_ticks = get_or<int>(st, "lead_lag_scalp_hard_stop_ticks", s.lead_lag_scalp_hard_stop_ticks);
    s.lead_lag_scalp_reversal_exit_enabled = get_or<bool>(st, "lead_lag_scalp_reversal_exit_enabled", s.lead_lag_scalp_reversal_exit_enabled);
    s.lead_lag_scalp_reversal_oracle_confirm_bps = get_or<double>(st, "lead_lag_scalp_reversal_oracle_confirm_bps", s.lead_lag_scalp_reversal_oracle_confirm_bps);
    s.lead_lag_scalp_rearm_bps = get_or<double>(st, "lead_lag_scalp_rearm_bps", s.lead_lag_scalp_rearm_bps);
    s.late_window_sniper_enabled = get_or<bool>(st, "late_window_sniper_enabled", s.late_window_sniper_enabled);
    s.late_window_sniper_min_secs = get_or<int>(st, "late_window_sniper_min_secs", s.late_window_sniper_min_secs);
    s.late_window_sniper_max_secs = get_or<int>(st, "late_window_sniper_max_secs", s.late_window_sniper_max_secs);
    s.late_window_sniper_min_secs_5m = get_or<int>(
        st, "late_window_sniper_min_secs_5m", s.late_window_sniper_min_secs);
    s.late_window_sniper_max_secs_5m = get_or<int>(
        st, "late_window_sniper_max_secs_5m", s.late_window_sniper_max_secs);
    s.late_window_sniper_min_secs_15m = get_or<int>(
        st, "late_window_sniper_min_secs_15m", s.late_window_sniper_min_secs);
    s.late_window_sniper_max_secs_15m = get_or<int>(
        st, "late_window_sniper_max_secs_15m", s.late_window_sniper_max_secs);
    s.late_window_sniper_min_price = get_or<double>(st, "late_window_sniper_min_price", s.late_window_sniper_min_price);
    s.late_window_sniper_max_price = get_or<double>(st, "late_window_sniper_max_price", s.late_window_sniper_max_price);
    s.late_window_sniper_min_net_edge_pct = get_or<double>(st, "late_window_sniper_min_net_edge_pct", s.late_window_sniper_min_net_edge_pct);
    s.late_window_sniper_order_usd = get_or<double>(st, "late_window_sniper_order_usd", s.late_window_sniper_order_usd);
    s.late_window_sniper_max_entries_per_window = get_or<int>(st, "late_window_sniper_max_entries_per_window", s.late_window_sniper_max_entries_per_window);
    s.late_window_sniper_hard_stop_ticks = get_or<int>(
        st, "late_window_sniper_hard_stop_ticks", s.late_window_sniper_hard_stop_ticks);
    s.simulation_strategy_enabled = get_or<bool>(
        st, "simulation_strategy_enabled", s.simulation_strategy_enabled);
    s.momentum_scalp_enabled        = get_or<bool>  (st, "momentum_scalp_enabled",         s.momentum_scalp_enabled);
    s.trend_filter_enabled          = get_or<bool>  (st, "trend_filter_enabled",           s.trend_filter_enabled);
    s.trend_filter_windows          = get_or<int>   (st, "trend_filter_windows",           s.trend_filter_windows);
    s.trend_filter_strength         = get_or<double>(st, "trend_filter_strength",          s.trend_filter_strength);
    s.oracle_reversal_exit          = get_or<bool>  (st, "oracle_reversal_exit",           s.oracle_reversal_exit);
    s.oracle_reversal_min_hold_secs = get_or<int>   (st, "oracle_reversal_min_hold_secs",  s.oracle_reversal_min_hold_secs);
    s.oracle_panic_exit             = get_or<bool>  (st, "oracle_panic_exit",              s.oracle_panic_exit);
    s.oracle_panic_delta_pct        = get_or<double>(st, "oracle_panic_delta_pct",         s.oracle_panic_delta_pct);
    s.oracle_panic_min_hold_secs    = get_or<int>   (st, "oracle_panic_min_hold_secs",     s.oracle_panic_min_hold_secs);
    s.stop_loss_reverse             = get_or<bool>  (st, "stop_loss_reverse",              s.stop_loss_reverse);
    s.min_entry_share_price         = get_or<double>(st, "min_entry_share_price",          s.min_entry_share_price);
    s.oracle_require_pred_agree     = get_or<bool>  (st, "oracle_require_pred_agree",      s.oracle_require_pred_agree);
    s.pred_agree_margin             = get_or<double>(st, "pred_agree_margin",              s.pred_agree_margin);
    s.pred_agree_min_conf           = get_or<double>(st, "pred_agree_min_conf",            s.pred_agree_min_conf);
    s.momentum_flip_exit            = get_or<bool>  (st, "momentum_flip_exit",             s.momentum_flip_exit);
    s.momentum_flip_margin          = get_or<double>(st, "momentum_flip_margin",           s.momentum_flip_margin);
    s.momentum_flip_min_conf        = get_or<double>(st, "momentum_flip_min_conf",         s.momentum_flip_min_conf);
    s.momentum_flip_reverse         = get_or<bool>  (st, "momentum_flip_reverse",          s.momentum_flip_reverse);
    s.momentum_flip_min_secs        = get_or<int>   (st, "momentum_flip_min_secs",         s.momentum_flip_min_secs);
    s.momentum_flip_min_hold_ms     = get_or<int>   (st, "momentum_flip_min_hold_ms",      s.momentum_flip_min_hold_ms);
    s.max_book_age_ms               = get_or<int>   (st, "max_book_age_ms",                s.max_book_age_ms);
    s.predictor_refresh_ms          = get_or<int>   (st, "predictor_refresh_ms",           s.predictor_refresh_ms);
    s.predict_budget_per_sweep      = get_or<int>   (st, "predict_budget_per_sweep",       s.predict_budget_per_sweep);
    s.predictor_recent_1m_bars      = get_or<int>   (st, "predictor_recent_1m_bars",       s.predictor_recent_1m_bars);
    s.predictor_recent_1m_ohlcv_bars =
        get_or<int>(st, "predictor_recent_1m_ohlcv_bars", s.predictor_recent_1m_ohlcv_bars);
    s.predictor_long_60m_bars       = get_or<int>   (st, "predictor_long_60m_bars",        s.predictor_long_60m_bars);
    s.predictor_5m_closes           = get_or<int>   (st, "predictor_5m_closes",            s.predictor_5m_closes);
    s.predictor_15m_closes          = get_or<int>   (st, "predictor_15m_closes",           s.predictor_15m_closes);
    s.predictor_5m_bars             = get_or<int>   (st, "predictor_5m_bars",              s.predictor_5m_bars);
    s.predictor_15m_bars            = get_or<int>   (st, "predictor_15m_bars",             s.predictor_15m_bars);
    s.predictor_60m_bars            = get_or<int>   (st, "predictor_60m_bars",             s.predictor_60m_bars);
    s.reversal_cooldown_secs        = get_or<int>   (st, "reversal_cooldown_secs",         s.reversal_cooldown_secs);
    s.max_reversals_per_window      = get_or<int>   (st, "max_reversals_per_window",       s.max_reversals_per_window);
    s.stop_loss_cooldown_secs       = get_or<int>   (st, "stop_loss_cooldown_secs",        s.stop_loss_cooldown_secs);
    s.reverse_min_secs_left         = get_or<int>   (st, "reverse_min_secs_left",          s.reverse_min_secs_left);
    s.reverse_slippage_pct          = get_or<double>(st, "reverse_slippage_pct",           s.reverse_slippage_pct);
    s.reverse_slippage_extra        = get_or<double>(st, "reverse_slippage_extra",         s.reverse_slippage_extra);
    s.max_entry_spread_pct          = get_or<double>(st, "max_entry_spread_pct",           s.max_entry_spread_pct);
    s.stop_loss_min_hold_secs       = get_or<int>   (st, "stop_loss_min_hold_secs",        s.stop_loss_min_hold_secs);
    s.timed_exit_min_bid            = get_or<double>(st, "timed_exit_min_bid",             s.timed_exit_min_bid);
    s.timed_exit_throttle_ms        = get_or<int>   (st, "timed_exit_throttle_ms",         s.timed_exit_throttle_ms);
    s.settlement_grace_ms           = get_or<int>   (st, "settlement_grace_ms",            s.settlement_grace_ms);
    s.window_predictor_mode         = get_or<bool>  (st, "window_predictor_mode",          s.window_predictor_mode);
    s.window_predictor_fractions.clear();
    if (st) {
        if (auto arr = (*st)["window_predictor_fractions"].as_array()) {
            for (auto& v : *arr)
                if (auto a = v.value<double>()) s.window_predictor_fractions.push_back(*a);
        }
    }
    s.window_predictor_amounts_usd.clear();
    if (st) {
        if (auto arr = (*st)["window_predictor_amounts_usd"].as_array()) {
            for (auto& v : *arr)
                if (auto a = v.value<double>()) s.window_predictor_amounts_usd.push_back(*a);
        }
    }
    s.window_predictor_buy_mode     = get_or<std::string>(st, "window_predictor_buy_mode",     s.window_predictor_buy_mode);
    s.window_predictor_price_min    = get_or<double>(st, "window_predictor_price_min",     s.window_predictor_price_min);
    s.window_predictor_price_max    = get_or<double>(st, "window_predictor_price_max",     s.window_predictor_price_max);
    s.window_predictor_skip_late    = get_or<bool>  (st, "window_predictor_skip_late",     s.window_predictor_skip_late);
    s.window_predictor_late_grace_ms= get_or<int>   (st, "window_predictor_late_grace_ms", s.window_predictor_late_grace_ms);
    s.window_predictor_min_secs_left= get_or<int>   (st, "window_predictor_min_secs_left", s.window_predictor_min_secs_left);
    s.window_predictor_source       = get_or<std::string>(st, "window_predictor_source",       s.window_predictor_source);
    s.window_predictor_assets.clear();
    if (st) {
        if (auto arr = (*st)["window_predictor_assets"].as_array()) {
            for (auto& v : *arr)
                if (auto a = v.value<std::string>()) s.window_predictor_assets.push_back(*a);
        }
    }
    s.window_predictor_horizons.clear();
    if (st) {
        if (auto arr = (*st)["window_predictor_horizons"].as_array()) {
            for (auto& v : *arr)
                if (auto a = v.value<int64_t>()) s.window_predictor_horizons.push_back((int)*a);
        }
    }
    s.window_predictor_pairs.clear();
    if (st) {
        if (auto arr = (*st)["window_predictor_pairs"].as_array()) {
            for (auto& v : *arr)
                if (auto a = v.value<std::string>()) s.window_predictor_pairs.push_back(*a);
        }
    }
    s.window_predictor_pm_flow      = get_or<bool>  (st, "window_predictor_pm_flow",       s.window_predictor_pm_flow);
    s.window_predictor_depth_topn   = get_or<int>   (st, "window_predictor_depth_topn",    s.window_predictor_depth_topn);
    s.window_predictor_vote_mode    = get_or<bool>  (st, "window_predictor_vote_mode",     s.window_predictor_vote_mode);
    s.window_predictor_vote_min_delta_pct = get_or<double>(st, "window_predictor_vote_min_delta_pct", s.window_predictor_vote_min_delta_pct);
    s.window_predictor_require_unanimous = get_or<bool>(st, "window_predictor_require_unanimous", s.window_predictor_require_unanimous);
    s.window_predictor_min_abs_oracle_delta_pct = get_or<double>(st, "window_predictor_min_abs_oracle_delta_pct", s.window_predictor_min_abs_oracle_delta_pct);
    s.window_predictor_min_win_return_pct = get_or<double>(st, "window_predictor_min_win_return_pct", s.window_predictor_min_win_return_pct);
    s.window_predictor_min_net_edge = get_or<double>(st, "window_predictor_min_net_edge", s.window_predictor_min_net_edge);
    s.window_predictor_size_max_mult = get_or<double>(st, "window_predictor_size_max_mult", s.window_predictor_size_max_mult);
    s.window_predictor_calibration_min_samples = get_or<int>(st, "window_predictor_calibration_min_samples", s.window_predictor_calibration_min_samples);
    s.window_predictor_calibration_margin = get_or<double>(st, "window_predictor_calibration_margin", s.window_predictor_calibration_margin);
    s.adaptive_mode                 = get_or<bool>  (st, "adaptive_mode",                  s.adaptive_mode);
    s.adaptive_target_conf          = get_or<double>(st, "adaptive_target_conf",           s.adaptive_target_conf);
    s.adaptive_min_secs_left        = get_or<int>   (st, "adaptive_min_secs_left",         s.adaptive_min_secs_left);
    s.adaptive_max_secs_left        = get_or<int>   (st, "adaptive_max_secs_left",         s.adaptive_max_secs_left);
    s.adaptive_trajectory_window_ms = get_or<int>   (st, "adaptive_trajectory_window_ms",  s.adaptive_trajectory_window_ms);
    s.adaptive_slope_threshold      = get_or<double>(st, "adaptive_slope_threshold",       s.adaptive_slope_threshold);
    s.adaptive_conf_trend_min       = get_or<double>(st, "adaptive_conf_trend_min",        s.adaptive_conf_trend_min);
    s.adaptive_trajectory_samples   = get_or<int>   (st, "adaptive_trajectory_samples",    s.adaptive_trajectory_samples);
    s.adaptive_require_predictor_agree = get_or<bool>(st, "adaptive_require_predictor_agree", s.adaptive_require_predictor_agree);
    s.adaptive_oracle_agree_margin  = get_or<double>(st, "adaptive_oracle_agree_margin",  s.adaptive_oracle_agree_margin);
    s.adaptive_use_wpred_payload    = get_or<bool>  (st, "adaptive_use_wpred_payload",     s.adaptive_use_wpred_payload);

    if (s.lead_lag_scalp_enabled) {
        if (s.lead_lag_scalp_horizon_s != 0 &&
            s.lead_lag_scalp_horizon_s != 300 &&
            s.lead_lag_scalp_horizon_s != 900)
            throw std::runtime_error("lead_lag_scalp_horizon_s must be 0, 300, or 900");
        if (!(s.lead_lag_scalp_min_signal_confidence >= 0.55 &&
              s.lead_lag_scalp_min_signal_confidence <= 0.92))
            throw std::runtime_error("lead_lag_scalp_min_signal_confidence must be 0.55..0.92");
        if (!(s.lead_lag_scalp_min_model_confidence >= 0.0 &&
              s.lead_lag_scalp_min_model_confidence <= 0.5))
            throw std::runtime_error("lead_lag_scalp_min_model_confidence must be 0..0.5");
        const std::vector<int> required_horizons = s.lead_lag_scalp_horizon_s == 0
            ? std::vector<int>{300, 900}
            : std::vector<int>{s.lead_lag_scalp_horizon_s};
        for (const int horizon : required_horizons)
            if (std::find(c.polymarket.universe_horizons_seconds.begin(),
                          c.polymarket.universe_horizons_seconds.end(), horizon) ==
                c.polymarket.universe_horizons_seconds.end())
                throw std::runtime_error("lead/lag scalp horizon must be enabled in polymarket.universe_horizons_seconds");
        if (!(s.lead_lag_scalp_min_cb_move_bps > 0.0 && s.lead_lag_scalp_min_cb_move_bps <= 100.0) ||
            !(s.lead_lag_scalp_min_cb_lead_bps > 0.0 && s.lead_lag_scalp_min_cb_lead_bps <= 100.0) ||
            !(s.lead_lag_scalp_max_oracle_counter_bps >= 0.0 && s.lead_lag_scalp_max_oracle_counter_bps <= 100.0) ||
            !(s.lead_lag_scalp_min_net_edge_pct >= 0.0 && s.lead_lag_scalp_min_net_edge_pct <= 50.0) ||
            !(s.lead_lag_scalp_min_price >= 0.03 && s.lead_lag_scalp_min_price < s.lead_lag_scalp_max_price && s.lead_lag_scalp_max_price <= 0.97) ||
            !(s.lead_lag_scalp_max_spread_pct > 0.0 && s.lead_lag_scalp_max_spread_pct <= 100.0) ||
            s.lead_lag_scalp_max_book_age_ms < 100 || s.lead_lag_scalp_max_book_age_ms > 10000 ||
            s.lead_lag_scalp_stop_entry_secs < 15 || s.lead_lag_scalp_stop_entry_secs >= 900 ||
            s.lead_lag_scalp_stop_entry_5m_secs < 15 || s.lead_lag_scalp_stop_entry_5m_secs >= 300 ||
            s.lead_lag_scalp_stop_entry_15m_secs < 15 || s.lead_lag_scalp_stop_entry_15m_secs >= 900 ||
            (s.lead_lag_scalp_max_secs_remaining_5m != 0 &&
             (s.lead_lag_scalp_max_secs_remaining_5m <= s.lead_lag_scalp_stop_entry_5m_secs ||
              s.lead_lag_scalp_max_secs_remaining_5m >= 300)) ||
            (s.lead_lag_scalp_max_secs_remaining_15m != 0 &&
             (s.lead_lag_scalp_max_secs_remaining_15m <= s.lead_lag_scalp_stop_entry_15m_secs ||
              s.lead_lag_scalp_max_secs_remaining_15m >= 900)) ||
            s.lead_lag_scalp_flatten_secs <= 0 ||
            s.lead_lag_scalp_flatten_5m_secs < 15 ||
            s.lead_lag_scalp_flatten_5m_secs > s.lead_lag_scalp_stop_entry_5m_secs ||
            s.lead_lag_scalp_flatten_15m_secs < 15 ||
            s.lead_lag_scalp_flatten_15m_secs > s.lead_lag_scalp_stop_entry_15m_secs ||
            s.lead_lag_scalp_max_entries_per_window < 1 || s.lead_lag_scalp_max_entries_per_window > 100 ||
            !(s.lead_lag_scalp_order_usd > 0.0) ||
            !(s.lead_lag_scalp_profit_target_pct >= 1.0 && s.lead_lag_scalp_profit_target_pct <= 100.0) ||
            !(s.lead_lag_scalp_hard_stop_pct >= 0.0 && s.lead_lag_scalp_hard_stop_pct <= 90.0) ||
            s.lead_lag_scalp_hard_stop_ticks < 0 || s.lead_lag_scalp_hard_stop_ticks > 1000 ||
            !(s.lead_lag_scalp_rearm_bps >= 0.0 && s.lead_lag_scalp_rearm_bps < s.lead_lag_scalp_min_cb_lead_bps))
            throw std::runtime_error("invalid Coinbase lead/lag scalp timing, edge, sizing, price, or risk settings");
        if (!s.predictor_mode)
            throw std::runtime_error("Coinbase lead/lag scalp requires predictor_mode=true for independent model confirmation");
    }
    if (s.late_window_sniper_enabled) {
        if (!(s.late_window_sniper_min_secs >= 1 &&
              s.late_window_sniper_min_secs < s.late_window_sniper_max_secs &&
              s.late_window_sniper_max_secs <= 45) ||
            !(s.late_window_sniper_min_secs_5m >= 1 &&
              s.late_window_sniper_min_secs_5m < s.late_window_sniper_max_secs_5m &&
              s.late_window_sniper_max_secs_5m <= 45) ||
            !(s.late_window_sniper_min_secs_15m >= 1 &&
              s.late_window_sniper_min_secs_15m < s.late_window_sniper_max_secs_15m &&
              s.late_window_sniper_max_secs_15m <= 45) ||
            !(s.late_window_sniper_min_price >= 0.82 &&
              s.late_window_sniper_min_price < s.late_window_sniper_max_price &&
              s.late_window_sniper_max_price <= 0.97) ||
            !(s.late_window_sniper_min_net_edge_pct >= 0.0 &&
              s.late_window_sniper_min_net_edge_pct <= 25.0) ||
            !(s.late_window_sniper_order_usd > 0.0) ||
            s.late_window_sniper_max_entries_per_window < 1 ||
            s.late_window_sniper_max_entries_per_window > 10 ||
            s.late_window_sniper_hard_stop_ticks < 0 ||
            s.late_window_sniper_hard_stop_ticks > 1000)
            throw std::runtime_error("invalid late-window settlement sniper settings (entry cap must be 1..10 per asset/window)");
        const bool has_supported_horizon = std::any_of(
            c.polymarket.universe_horizons_seconds.begin(),
            c.polymarket.universe_horizons_seconds.end(),
            [](int h) { return h == 300 || h == 900; });
        if (!has_supported_horizon)
            throw std::runtime_error("late-window sniper requires a 5m or 15m market universe");
    }
    if (s.simulation_strategy_enabled) {
        if (c.runtime.mode != TradeMode::Paper)
            throw std::runtime_error("simulation_strategy_enabled is paper-only");
        if (std::find(c.polymarket.universe_horizons_seconds.begin(),
                      c.polymarket.universe_horizons_seconds.end(), 300) ==
            c.polymarket.universe_horizons_seconds.end())
            throw std::runtime_error("simulation_strategy_enabled requires a 5m market universe");
    }
    auto* rs = tbl["risk"].as_table();
    auto& r = c.risk;
    r.max_order_usd             = get_or<double>(rs, "max_order_usd",             r.max_order_usd);
    r.max_deployed_usd          = get_or<double>(rs, "max_deployed_usd",          r.max_deployed_usd);
    r.max_per_window_usd        = get_or<double>(rs, "max_per_window_usd",        r.max_per_window_usd);
    r.min_order_usd             = get_or<double>(rs, "min_order_usd",             r.min_order_usd);
    r.clob_min_shares_fallback  = get_or<double>(rs, "clob_min_shares_fallback",  r.clob_min_shares_fallback);
    r.wallet_balance_warn_above = get_or<double>(rs, "wallet_balance_warn_above", r.wallet_balance_warn_above);
    r.daily_loss_kill_pct       = get_or<double>(rs, "daily_loss_kill_pct",       r.daily_loss_kill_pct);
    if (r.daily_loss_kill_pct != 0.0) {
        spdlog::warn("risk.daily_loss_kill_pct={} is deprecated and disabled fleet-wide; forcing it to 0",
                     r.daily_loss_kill_pct);
        r.daily_loss_kill_pct = 0.0;
    }
    r.daily_kill_grace_secs     = get_or<int>   (rs, "daily_kill_grace_secs",     r.daily_kill_grace_secs);
    r.daily_kill_check_interval_ms =
        get_or<int>(rs, "daily_kill_check_interval_ms", r.daily_kill_check_interval_ms);
    r.autopilot_heartbeat_secs  = get_or<int>   (rs, "autopilot_heartbeat_secs",  r.autopilot_heartbeat_secs);
    auto* tu = tbl["tui"].as_table();
    c.tui.fps_ac          = get_or<int> (tu, "fps_ac",          c.tui.fps_ac);
    c.tui.fps_battery     = get_or<int> (tu, "fps_battery",     c.tui.fps_battery);
    c.tui.truecolor       = get_or<bool>(tu, "truecolor",       c.tui.truecolor);
    c.tui.confirm_default = get_or<bool>(tu, "confirm_default", c.tui.confirm_default);
    // Wallet (optional — all empty strings if section absent; required for live mode)
    auto* wl = tbl["wallet"].as_table();
    c.wallet.credentials_file = get_or<std::string>(
        wl, "credentials_file", c.wallet.credentials_file);
    c.wallet.private_key_file = get_or<std::string>(wl, "private_key_file", "");
    c.wallet.api_key          = get_or<std::string>(wl, "api_key",          "");
    c.wallet.api_secret       = get_or<std::string>(wl, "api_secret",       "");
    c.wallet.api_passphrase   = get_or<std::string>(wl, "api_passphrase",   "");
    c.wallet.neg_risk         = get_or<bool>       (wl, "neg_risk",         true);
    c.wallet.signature_type   = get_or<int64_t>    (wl, "signature_type",   1);
    c.wallet.starting_balance_usd = get_or<double> (wl, "starting_balance_usd", 0.0);
    c.wallet.builder_code         = get_or<std::string>(wl, "builder_code", "");

    // Data collection config (research-grade persistence)
    auto* dc = tbl["data_collection"].as_table();
    c.data_collection.oracle_ticks_enabled       = get_or<bool>(dc, "oracle_ticks_enabled",       true);
    c.data_collection.binance_ticks_enabled      = get_or<bool>(dc, "binance_ticks_enabled",      true);
    c.data_collection.coinbase_ticks_enabled     = get_or<bool>(dc, "coinbase_ticks_enabled",     true);
    c.data_collection.window_events_enabled      = get_or<bool>(dc, "window_events_enabled",      true);
    c.data_collection.features_enabled           = get_or<bool>(dc, "features_enabled",           true);
    c.data_collection.features_write_interval_ms = get_or<int>(dc,  "features_write_interval_ms", 1000);
    c.data_collection.book_snapshots_enabled     = get_or<bool>(dc, "book_snapshots_enabled",     true);
    c.data_collection.pm_trades_enabled          = get_or<bool>(dc, "pm_trades_enabled",          true);
    c.data_collection.log_max_bytes              = get_or<int>(dc, "log_max_bytes",              c.data_collection.log_max_bytes);
    c.data_collection.log_keep_files             = get_or<int>(dc, "log_keep_files",             c.data_collection.log_keep_files);
    c.data_collection.disk_space_warn_mb         = get_or<int>(dc, "disk_space_warn_mb",         c.data_collection.disk_space_warn_mb);
    c.data_collection.disk_space_kill_mb         = get_or<int>(dc, "disk_space_kill_mb",         c.data_collection.disk_space_kill_mb);
    c.data_collection.disk_check_interval_ms     = get_or<int>(dc, "disk_check_interval_ms",     c.data_collection.disk_check_interval_ms);
    c.data_collection.disk_recovery_multiplier   = get_or<double>(dc, "disk_recovery_multiplier", c.data_collection.disk_recovery_multiplier);
    c.data_collection.candle_store_max_bars_per_asset_timeframe =
        get_or<int>(dc, "candle_store_max_bars_per_asset_timeframe",
                    c.data_collection.candle_store_max_bars_per_asset_timeframe);
    c.data_collection.trade_flow_max_records_per_symbol =
        get_or<int>(dc, "trade_flow_max_records_per_symbol",
                    c.data_collection.trade_flow_max_records_per_symbol);

    // Override-from-secrets: if the configured credential bundle exists,
    // it WINS over the TOML wallet block. This lets users keep secrets
    // out of the config file. The file is produced by
    // `npm run gen-creds` (see scripts/gen_polymarket_creds.mjs).
    namespace fs = std::filesystem;
    const std::string& creds_path = c.wallet.credentials_file;
    if (!creds_path.empty() && fs::exists(creds_path)) {
        try {
            std::ifstream f(creds_path);
            std::stringstream ss; ss << f.rdbuf();
            std::string body = ss.str();
            simdjson::ondemand::parser parser;
            auto pad = body + "    ";
            auto doc = parser.iterate(pad);
            auto get_str = [&](const char* k) -> std::string {
                std::string_view v;
                auto err = doc[k].get_string().get(v);
                if (err) return {};
                return std::string(v);
            };
            std::string ak = get_str("apiKey");
            std::string sk = get_str("secret");
            std::string pp = get_str("passphrase");
            std::string fa = get_str("funderAddress");
            std::string sa = get_str("signerAddress");
            if (!ak.empty()) c.wallet.api_key        = ak;
            if (!sk.empty()) c.wallet.api_secret     = sk;
            if (!pp.empty()) c.wallet.api_passphrase = pp;
            int64_t sig_type = -1;
            auto sig_err = doc["signatureType"].get_int64().get(sig_type);
            if (!sig_err && sig_type >= 0) c.wallet.signature_type = (int)sig_type;

            // Treat the credential JSON as one coherent identity. Mixing API
            // keys from this file with a stale TOML proxy/signature_type creates
            // authenticated but unfillable live orders.
            if (!fa.empty()) c.polymarket.proxy_address = fa;
            if (!sa.empty()) c.polymarket.signer_address = sa;
            if (c.polymarket.signer_address.empty())
                c.polymarket.signer_address = c.polymarket.proxy_address;
            spdlog::info("config: loaded L2 creds from {} (apiKey configured sigType={} funder={})",
                         creds_path, c.wallet.signature_type,
                         c.polymarket.proxy_address);
        } catch (const std::exception& e) {
            spdlog::warn("config: failed to parse {}: {}", creds_path, e.what());
        }
    }
    return c;
}
void Config::dump_summary() const {
    spdlog::info("=== PolySparQ++ config ===");
    spdlog::info("  build        : {} {}  (verify TUI changes are live)", __DATE__, __TIME__);
    spdlog::info("  mode         : {}",  to_string(runtime.mode));
    spdlog::info("  postgres     : configured");
    spdlog::info("  pm clob ws   : configured={}", !polymarket.clob_ws_url.empty());
    spdlog::info("  binance ws   : configured={} ({} streams)", !binance.ws_url.empty(), binance.streams.size());
    spdlog::info("  llm          : endpoint_configured={} (model={}, enabled={})", !llm.url.empty(), llm.model, llm.enabled);
    spdlog::info("  limits       : ${:.2f}/order  ${:.2f}/window  ${:.2f} deployed  share<={:.2f}",
                 risk.max_order_usd, risk.max_per_window_usd, risk.max_deployed_usd, strategy.hard_share_price_ceil);
    std::string blocked;
    for (const auto& a : strategy.blocked_assets) {
        if (!blocked.empty()) blocked += ",";
        blocked += a;
    }
    if (blocked.empty()) blocked = "<none>";
    spdlog::info("  sizing      : equity={} order_pct={:.1f}% size=[${:.2f},${:.2f}] deploy_pct={:.1f}%",
                 strategy.equity_sizing_enabled ? "ON" : "OFF",
                 strategy.equity_order_pct * 100.0,
                 strategy.min_size_usd, strategy.max_size_usd,
                 strategy.equity_max_deployed_pct * 100.0);
    spdlog::info("  entries     : price=[{:.2f},{:.2f}] blocked_assets={}",
                 strategy.min_entry_share_price, strategy.oracle_max_token_price, blocked);
    spdlog::info("  execution   : entry={} improve={}t reprice={}ms poll={}ms ttl={}ms close_guard={}ms",
                 strategy.entry_order_mode, strategy.maker_improve_ticks,
                 strategy.maker_reprice_ms, strategy.maker_poll_ms,
                 strategy.maker_max_age_ms,
                 strategy.maker_cancel_before_close_ms);
    spdlog::info("  strategy     : oracle={} digital_fv={}{} predictor={} endgame={} scalp_v2={} scalp_v3={} momentum_scalp={}",
                 strategy.oracle_lag_mode ? "ON" : "OFF",
                 strategy.digital_fv_mode ? "ON" : "OFF",
                 strategy.digital_fv_use_coinbase ? "(CB)" : "",
                 strategy.predictor_mode ? "ON" : "OFF",
                 strategy.endgame_mode ? "ON" : "OFF",
                 strategy.scalp_v2_enabled ? "ON" : "OFF",
                 strategy.scalp_v3_enabled ? "ON" : "OFF",
                 strategy.momentum_scalp_enabled ? "ON" : "OFF");
    spdlog::info("  auto exits   : profit_take={} stop_loss={} timed={}s oracle_rev={} oracle_panic={} momentum_flip={} reverse={}",
                 strategy.profit_take_mode ? "ON" : "OFF",
                 strategy.stop_loss_mode ? "ON" : "OFF",
                 strategy.timed_exit_secs,
                 strategy.oracle_reversal_exit ? "ON" : "OFF",
                 strategy.oracle_panic_exit ? "ON" : "OFF",
                 strategy.momentum_flip_exit ? "ON" : "OFF",
                 (strategy.stop_loss_reverse || strategy.momentum_flip_reverse) ? "ON" : "OFF");
    spdlog::info("  scalp exits  : reversal={} trailing_stop={} profit_take={} runner={}",
                 ((strategy.scalp_v2_enabled && strategy.scalp_v2_reversal_exit) ||
                  (strategy.scalp_v3_enabled && strategy.scalp_v3_reversal_exit)) ? "ON" : "OFF",
                 ((strategy.scalp_v2_enabled && strategy.scalp_v2_trailing_stop) ||
                  (strategy.scalp_v3_enabled && strategy.scalp_v3_trailing_stop)) ? "ON" : "OFF",
                 ((strategy.scalp_v2_enabled && strategy.scalp_v2_profit_take) ||
                  (strategy.scalp_v3_enabled && strategy.scalp_v3_profit_take)) ? "ON" : "OFF",
                 (strategy.scalp_v3_enabled && strategy.scalp_v3_runner_enabled) ? "ON" : "OFF");
    if (strategy.window_predictor_mode) {
        std::string fr, am, pairs;
        for (size_t i = 0; i < strategy.window_predictor_fractions.size(); ++i) {
            fmt::format_to(std::back_inserter(fr), "{}{:.2f}",
                           i ? "," : "", strategy.window_predictor_fractions[i]);
            double a = (i < strategy.window_predictor_amounts_usd.size())
                           ? strategy.window_predictor_amounts_usd[i] : 0.0;
            fmt::format_to(std::back_inserter(am), "{}${:.2f}", i ? "," : "", a);
        }
        for (size_t i = 0; i < strategy.window_predictor_pairs.size(); ++i)
            fmt::format_to(std::back_inserter(pairs), "{}{}", i ? "," : "",
                           strategy.window_predictor_pairs[i]);
        spdlog::info("  wpred        : fractions=[{}] amounts=[{}] pairs=[{}] band=[{:.2f},{:.2f}] "
                     "unanimous={} min_oracle={:.4f}% min_win_return={:.1f}% min_net_edge={:.3f} "
                     "late_grace={}ms pm_flow={}",
                     fr.empty() ? "<none>" : fr, am,
                     pairs.empty() ? "assets x horizons" : pairs,
                     strategy.window_predictor_price_min, strategy.window_predictor_price_max,
                     strategy.window_predictor_require_unanimous ? "ON" : "OFF",
                     strategy.window_predictor_min_abs_oracle_delta_pct,
                     strategy.window_predictor_min_win_return_pct,
                     strategy.window_predictor_min_net_edge,
                     strategy.window_predictor_late_grace_ms,
                     strategy.window_predictor_pm_flow ? "ON" : "OFF");
    }
    if (strategy.adaptive_mode) {
        spdlog::info("  adaptive     : target_conf={:.2f} min_secs={} max_secs={} "
                     "traj_window={}ms slope_thresh={:+.4f} conf_trend_min={:+.4f} "
                     "samples={} require_agree={}",
                     strategy.adaptive_target_conf,
                     strategy.adaptive_min_secs_left,
                     strategy.adaptive_max_secs_left,
                     strategy.adaptive_trajectory_window_ms,
                     strategy.adaptive_slope_threshold,
                     strategy.adaptive_conf_trend_min,
                     strategy.adaptive_trajectory_samples,
                     strategy.adaptive_require_predictor_agree ? "ON" : "OFF");
    }
    bool live_armed = !wallet.api_key.empty()
                   && !wallet.api_secret.empty()
                   && !wallet.api_passphrase.empty()
                   && !polymarket.proxy_address.empty();
    spdlog::info("  live creds   : {} (mode={}, funder={})",
                 live_armed ? "PRESENT" : "absent",
                 to_string(runtime.mode),
                 polymarket.proxy_address.empty() ? "<none>" : polymarket.proxy_address);
    spdlog::info("  data_collect : oracle={} binance={} coinbase={} events={} features={} ({}ms)",
                 data_collection.oracle_ticks_enabled ? "ON" : "OFF",
                 data_collection.binance_ticks_enabled ? "ON" : "OFF",
                 data_collection.coinbase_ticks_enabled ? "ON" : "OFF",
                 data_collection.window_events_enabled ? "ON" : "OFF",
                 data_collection.features_enabled ? "ON" : "OFF",
                 data_collection.features_write_interval_ms);
}
} // namespace vp
