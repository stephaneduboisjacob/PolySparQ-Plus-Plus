#include "engine/rules.hpp"
#include "tui/tui.hpp"
#include "engine/reference_forecasts.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

namespace {

vp::StrategyHot endgame_config() {
    vp::StrategyHot cfg;
    cfg.endgame_mode = true;
    cfg.endgame_secs_left = 20;
    cfg.endgame_min_price = 0.85;
    cfg.endgame_max_price = 0.92;
    cfg.endgame_min_profit_pct = 5.0;
    cfg.endgame_min_oracle_pct = 0.10;
    cfg.hard_share_price_ceil = 0.94;
    cfg.coinbase_veto_mode = true;
    cfg.coinbase_lead_min_delta_pct = 0.10;
    cfg.binance_book_confirm = true;
    cfg.binance_book_imb_min = 0.10;
    cfg.multi_source_vote = true;
    cfg.multi_source_quorum = 2;
    cfg.taker_fee_rate = 0.07;
    cfg.fee_exponent = 1;
    return cfg;
}

vp::WatchlistRow confirmed_up(int64_t now_ms) {
    vp::WatchlistRow row;
    row.asset = "ETH";
    row.horizon_s = 300;
    row.window_close_ms = now_ms + 10'000;
    row.up_ask = 0.90;
    row.up_bid = 0.89;
    row.down_ask = 0.11;
    row.down_bid = 0.10;
    row.reference_forecast = {0.99,0.98,0.995,1,true,"ready"};
    row.cb_data_ready = true;
    row.oracle_delta_pct = 0.20;
    row.cb_delta_pct = 0.18;
    row.bt_mid_delta_pct = 0.01;
    row.bt_imbalance = 0.20;
    return row;
}

} // namespace

int main() {
    constexpr int64_t now_ms = 1'000'000;
    const auto cfg = endgame_config();

    {
        auto row = confirmed_up(now_ms);
        const auto sig = vp::rules::evaluate(row, cfg, now_ms);
        assert(sig);
        assert(sig->side == vp::ShareSide::UP);
    }
    {
        auto row = confirmed_up(now_ms);
        row.cb_delta_pct = -0.18;
        assert(!vp::rules::evaluate(row, cfg, now_ms));
    }
    {
        auto row = confirmed_up(now_ms);
        row.cb_delta_pct = 0.0;
        row.bt_mid_delta_pct = 0.0;
        row.bt_imbalance = 0.0;
        assert(!vp::rules::evaluate(row, cfg, now_ms));
    }
    {
        auto row = confirmed_up(now_ms);
        auto expensive = cfg;
        expensive.endgame_min_profit_pct = 20.0;
        assert(!vp::rules::evaluate(row, expensive, now_ms));
    }
    {
        vp::StrategyHot latency;
        latency.digital_fv_mode = true;
        latency.digital_fv_use_coinbase = true;
        latency.digital_fv_min_edge_pct = 4.5;
        latency.digital_fv_fee_buffer = 1.5;
        latency.digital_fv_down_slip_pct = 1.0;
        latency.digital_fv_jump_z = 5.0;
        latency.coinbase_lead_min_delta_pct = 0.05;
        latency.hard_share_price_ceil = 0.70;
        latency.max_entry_share_price = 0.70;
        latency.min_secs_remaining_5m = 60;
        latency.max_secs_remaining_5m = 260;
        latency.min_entry_share_price = 0.30;
        latency.endgame_mode = false;

        vp::WatchlistRow row;
        row.asset = "BTC";
        row.horizon_s = 300;
        row.window_close_ms = now_ms + 150'000;
        row.up_ask = 0.53;
        row.up_bid = 0.52;
        row.down_ask = 0.48;
        row.down_bid = 0.47;
        row.reference_forecast = {0.65,0.61,0.7,1,true,"ready"};
        row.fast_forecast = row.reference_forecast;
        row.cb_data_ready = true;
        row.coin_open_price = 80'000.0;
        row.coin_now_price = 80'000.0; // Chainlink has not moved yet.
        row.cb_delta_pct = 0.10;       // Coinbase is already leading upward.
        row.oracle_vol_per_sec = 0.00010;

        const auto sig = vp::rules::evaluate(row, latency, now_ms);
        assert(sig);
        assert(sig->side == vp::ShareSide::UP);
        assert(sig->reason.rfind("DIGITALFV-CB", 0) == 0);

        row.cb_delta_pct = 0.02;       // Below the fast-move floor.
        assert(!vp::rules::evaluate(row, latency, now_ms));
        row.cb_delta_pct = 0.10;
        row.cb_data_ready = false;    // A stale CB quote cannot become a lead.
        assert(!vp::rules::evaluate(row, latency, now_ms));
        row.cb_data_ready = true;
        row.fast_forecast.low = 0.54; // Apparent edge disappears under stress/fees.
        assert(!vp::rules::evaluate(row, latency, now_ms));
        row.fast_forecast.low = 0.61;
        row.window_close_ms = now_ms+50'000; // Digital FV must honor configured timing.
        assert(!vp::rules::evaluate(row, latency, now_ms));
        row.window_close_ms = now_ms+150'000;
        row.reference_forecast.ready = false; // Missing/final-TWAP reference fails closed.
        assert(!vp::rules::evaluate(row, latency, now_ms));
    }
    {
        vp::StrategyHot cb;
        cb.oracle_lag_mode=true; cb.digital_fv_mode=false; cb.predictor_mode=false;
        cb.endgame_mode=false; cb.endgame_secs_left=20;
        cb.min_secs_remaining_5m=60; cb.max_secs_remaining_5m=260;
        cb.min_oracle_delta_pct=.04; cb.coinbase_veto_mode=true;
        cb.hard_share_price_ceil=.7; cb.oracle_max_token_price=.7;
        cb.min_entry_share_price=.3; cb.min_predictor_edge=.03;
        cb.binance_book_confirm=false; cb.multi_source_vote=false;
        cb.oracle_require_pred_agree=false;
        auto row=confirmed_up(now_ms);
        row.window_close_ms=now_ms+150'000;
        row.up_bid=.59; row.up_ask=.6;
        row.reference_forecast={.7,.66,.76,1,true,"ready"};
        assert(vp::rules::evaluate(row,cb,now_ms));
        row.reference_forecast.low=.62;
        assert(!vp::rules::evaluate(row,cb,now_ms));
        row.reference_forecast.low=.66; row.cb_data_ready=false;
        assert(!vp::rules::evaluate(row,cb,now_ms));

        // The configured Coinbase trigger may be below the oracle threshold.
        // Neither evaluate() nor the downstream phase gate may retest that
        // signal using the lagging oracle's smaller move.
        row.cb_data_ready=true;
        cb.coinbase_lead_mode=true;
        cb.coinbase_lead_min_delta_pct=.029;
        cb.min_oracle_delta_pct=.030;
        row.oracle_delta_pct=.001;
        row.cb_delta_pct=.0295;
        row.fast_forecast=row.reference_forecast;
        auto lead=vp::rules::evaluate(row,cb,now_ms);
        assert(lead && lead->reason.rfind("ORACLE+CB-LEAD",0)==0);
        assert(lead->trigger_delta_pct==row.cb_delta_pct);
        assert(lead->trigger_delta_pct >= vp::rules::oracle_trigger_threshold(cb,true));
        assert(row.oracle_delta_pct < vp::rules::oracle_trigger_threshold(cb,true));
        row.cb_delta_pct=.028;
        vp::rules::EvaluationDiagnostics diagnostic;
        assert(!vp::rules::evaluate(row,cb,now_ms,&diagnostic));
        assert(std::string(diagnostic.code)=="oracle_delta");
        row.cb_delta_pct=-.04; // an opposing fast feed must not become a lead
        assert(!vp::rules::evaluate(row,cb,now_ms));
    }
    {
        // Runtime integration: smoothed TWAP ticks used to trip the latency
        // volatility floor even with sufficient unaveraged underlying risk.
        constexpr int64_t base=3'600'000;
        constexpr int64_t current=base+40*60'000;
        vp::engine::CandleStore candles;
        for(int i=0;i<=40;++i)
            candles.on_tick("BTC", i%2 ? 100.1 : 100.0, 1, false, base+i*60'000);
        vp::PmMarket market;
        market.asset="BTC";
        market.horizon_seconds=300;
        market.window_open_ms=current-150'000;
        market.window_close_ms=current+150'000;
        market.settlement_window_s=60;
        vp::core::OracleBuffer oracle, coinbase;
        const double sec=current/1000.0;
        coinbase.add("BTC",sec-150,100);
        coinbase.add("BTC",sec,100.2);
        for(int i=0;i<=60;++i) {
            oracle.add("BTC:TWAP60",sec-60+i,100.0+i*0.000001);
            oracle.add("BTC:SPOT",sec-60+i,100.2);
        }
        auto row=confirmed_up(current);
        row.asset="BTC"; row.window_close_ms=market.window_close_ms;
        row.coin_open_price=100;
        row.up_bid=.59; row.up_ask=.60; row.down_bid=.39; row.down_ask=.40;
        vp::engine::populate_reference_forecasts(row,market,oracle,coinbase,candles,current);
        assert(row.fast_forecast.ready && row.cb_data_ready);
        vp::StrategyHot latency;
        latency.digital_fv_mode=true; latency.digital_fv_use_coinbase=true;
        latency.coinbase_veto_mode=true; latency.coinbase_lead_min_delta_pct=.05;
        latency.min_secs_remaining_5m=60; latency.max_secs_remaining_5m=260;
        latency.min_entry_share_price=.30; latency.max_entry_share_price=.70;
        latency.hard_share_price_ceil=.70; latency.min_predictor_edge=.03;
        latency.digital_fv_jump_z=100;
        latency.digital_fv_vol_floor=.00002;
        vp::rules::EvaluationDiagnostics diagnostic;
        assert(row.oracle_vol_per_sec > latency.digital_fv_vol_floor);
        assert(vp::rules::evaluate(row,latency,current,&diagnostic));
        row.oracle_vol_per_sec=oracle.realized_vol_per_sec(market.reference_key(),60);
        assert(row.oracle_vol_per_sec > 0 && row.oracle_vol_per_sec < latency.digital_fv_vol_floor);
        assert(!vp::rules::evaluate(row,latency,current,&diagnostic));
        assert(std::string(diagnostic.code)=="digital_vol_floor");

        // A repaired historical gap must invalidate the cached volatility
        // even when the newest closed bar's timestamp did not change.
        candles.on_tick("BTC",100,1,false,current+120'000);
        candles.on_tick("BTC",100.1,1,false,current+180'000);
        market.window_close_ms=current+330'000;
        row.window_close_ms=market.window_close_ms;
        oracle.add("BTC:SPOT",sec+180,100.2);
        oracle.add("BTC:TWAP60",sec+180,100.1);
        coinbase.add("BTC",sec+180,100.2);
        vp::engine::populate_reference_forecasts(row,market,oracle,coinbase,candles,current+180'000);
        assert(!row.reference_forecast.ready && std::string(row.reference_forecast.status)=="warming_up");
        const vp::engine::Candle missing{current+60'000,100.1,100.1,100,100.1,1};
        candles.merge_closed_candles("BTC",std::span(&missing,1),current+180'000);
        vp::engine::populate_reference_forecasts(row,market,oracle,coinbase,candles,current+180'000);
        assert(row.reference_forecast.ready);
    }
}
