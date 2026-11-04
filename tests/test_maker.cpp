#include "exec/paper.hpp"
#include "core/clock.hpp"
#include "engine/recycler.hpp"
#include <fstream>
#include <filesystem>
#include "core/cockpit_command.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>
#include <unordered_set>

using namespace vp;

int main() {
    {
        const auto on = parse_cockpit_command(R"({"instance": "cb", "op": "autopilot", "enabled": true})");
        const auto off = parse_cockpit_command(R"({"instance":"cb","op":"autopilot","enabled":false})");
        assert(on && on->enabled && off && !off->enabled);
        assert(!parse_cockpit_command(R"({"instance":"cb","op":"autopilot","enabled":"true"})"));
        assert(!parse_cockpit_command(R"({"op":"autopilot","enabled":true})"));
        assert(!parse_cockpit_command("{"));
        const auto legacy = parse_cockpit_command(R"({"cmd":"force_sell","instance":"cb","condition_id":"market","side":"UP"})");
        assert(legacy && legacy->op == "force_sell");
    }

    Positions positions;
    std::vector<OrderResult> events;
    Journal journal{[&](const OrderRequest&, const OrderResult& r) { events.push_back(r); }};
    PaperExecutor exec(positions, journal, 100.0, 100.0, 100.0, 1.0, 5.0);

    OrderBook book;
    book.apply_snapshot({{0.50, 100}}, {{0.55, 100}});
    auto lookup = [&](const std::string&, ShareSide) -> const OrderBook* { return &book; };

    OrderRequest maker;
    maker.condition_id = "maker-test";
    maker.asset = "BTC";
    maker.horizon_s = 300;
    maker.token_id = "1";
    maker.side = ShareSide::UP;
    maker.dir = OrderDir::Buy;
    maker.size_usd = 5.0;
    maker.limit_price = 0.60;
    maker.tick_size = 0.01;
    maker.execution_mode = ExecutionMode::Maker;
    maker.maker_max_age_ms = 10'000;

    auto placed = exec.submit(maker, lookup);
    assert(placed.accepted && placed.resting && !placed.terminal);
    assert(placed.status == "acked");
    assert(std::fabs(placed.order_price - 0.50) < 1e-9);
    assert(positions.snapshot().empty());
    assert(exec.open_order_count() == 1);

    // A BBO touch alone is not a simulated fill; public prints must consume
    // the visible queue ahead of the resting order first.
    book.apply_snapshot({{0.49, 100}}, {{0.50, 100}});
    const auto after_touch_ms = Clock::wall_ns() / 1'000'000LL + 100;
    exec.maintain_open_orders(lookup, after_touch_ms);
    assert(exec.open_order_count() == 1);
    assert(positions.snapshot().empty());

    const auto trade_ms = Clock::wall_ns() / 1'000'000LL + 200;
    const auto maker_fills = exec.on_public_trade(
        "maker-test", "1", ShareSide::UP, OrderDir::Sell,
        0.50, 110.0, trade_ms, trade_ms);
    assert(maker_fills == 1);
    assert(exec.open_order_count() == 0);
    auto pos = positions.snapshot();
    assert(pos.size() == 1 && std::fabs(pos[0].shares - 10.0) < 1e-9);
    assert(events.back().status == "filled");

    // The isolated Coinbase confluence maker path must accept the exact
    // 20-share clip under its $18 order budget and remain fee-free.
    Positions cb_positions;
    std::vector<OrderResult> cb_events;
    PaperExecutor cb_exec(cb_positions,
        Journal{[&](const OrderRequest&, const OrderResult& r) {
            cb_events.push_back(r);
        }, {}, {}}, 18.0, 200.0, 50.0, 1.0, 5.0);
    OrderBook cb_book;
    cb_book.apply_snapshot({{0.88, 150}}, {{0.90, 150}});
    auto cb_lookup = [&](const std::string&, ShareSide) -> const OrderBook* {
        return &cb_book;
    };
    OrderRequest cb_maker;
    cb_maker.source = "COINBASE_CONFLUENCE_PAPER_MAKER_ENTRY";
    cb_maker.condition_id = "cb-maker-20";
    cb_maker.asset = "BTC";
    cb_maker.horizon_s = 900;
    cb_maker.window_close_ms = Clock::wall_ns() / 1'000'000LL + 900'000;
    cb_maker.token_id = "cb-token";
    cb_maker.side = ShareSide::UP;
    cb_maker.dir = OrderDir::Buy;
    cb_maker.execution_mode = ExecutionMode::Maker;
    cb_maker.size_usd = 18.0;
    cb_maker.size_shares = 20.0;
    cb_maker.limit_price = 0.89;
    cb_maker.tick_size = 0.01;
    cb_maker.fee_rate = 0.0;
    cb_maker.fee_exponent = 1;
    cb_maker.maker_fixed_price = true;
    cb_maker.maker_reprice_ms = 0;
    cb_maker.maker_max_age_ms = 5500;
    const auto cb_quote = cb_exec.submit(cb_maker, cb_lookup);
    assert(cb_quote.accepted && cb_quote.resting && cb_quote.order_shares == 20.0);
    assert(std::fabs(cb_quote.order_price - 0.89) < 1e-9);
    const auto cb_trade_ms = Clock::wall_ns() / 1'000'000LL + 5;
    assert(cb_exec.on_public_trade(cb_maker.condition_id, cb_maker.token_id,
        ShareSide::UP, OrderDir::Sell, 0.89, 20.0,
        cb_trade_ms, cb_trade_ms) == 1);
    const auto cb_pos = cb_positions.snapshot();
    assert(cb_pos.size() == 1 && std::fabs(cb_pos.front().shares - 20.0) < 1e-9);
    assert(std::fabs(cb_positions.total_deployed_usd() - 17.8) < 1e-9);
    assert(std::fabs(cb_events.back().fee_usd) < 1e-9);

    Positions cb_ttl_positions;
    PaperExecutor cb_ttl_exec(cb_ttl_positions, {}, 18.0, 200.0, 50.0, 1.0, 5.0);
    cb_maker.condition_id = "cb-maker-ttl";
    cb_maker.window_close_ms = Clock::wall_ns() / 1'000'000LL + 900'000;
    const auto cb_ttl_quote = cb_ttl_exec.submit(cb_maker, cb_lookup);
    assert(cb_ttl_quote.accepted && cb_ttl_quote.resting);
    cb_ttl_exec.maintain_open_orders(cb_lookup,
        Clock::wall_ns() / 1'000'000LL + 6000);
    assert(cb_ttl_exec.open_order_count() == 0);
    assert(cb_ttl_positions.snapshot().empty());

    // Hybrid rests first, then submits only its remainder as a taker after TTL.
    OrderBook book2;
    book2.apply_snapshot({{0.40, 100}}, {{0.45, 100}});
    auto lookup2 = [&](const std::string&, ShareSide) -> const OrderBook* { return &book2; };
    maker.condition_id = "hybrid-test";
    maker.token_id = "2";
    maker.execution_mode = ExecutionMode::Hybrid;
    maker.maker_max_age_ms = 1;
    auto hybrid = exec.submit(maker, lookup2);
    assert(hybrid.accepted && hybrid.resting);
    exec.maintain_open_orders(lookup2, Clock::wall_ns() / 1'000'000LL + 10);
    assert(exec.open_order_count() == 0);
    bool saw_cancel = false, saw_fak_fill = false;
    for (const auto& e : events) {
        saw_cancel |= e.status == "cancelled";
        saw_fak_fill |= e.status == "filled" && e.fill_price >= 0.45 - 1e-9;
    }
    assert(saw_cancel && saw_fak_fill);

    // A rising maker quote must not turn a $5 instruction into a larger fill.
    Positions capped_positions;
    PaperExecutor capped(capped_positions,{},100,100,100,1,5);
    maker.condition_id="reprice-budget"; maker.execution_mode=ExecutionMode::Maker;
    maker.maker_max_age_ms=10000; maker.maker_reprice_ms=1;
    book2.apply_snapshot({{.40,100}},{{.45,100}});
    auto quote=capped.submit(maker,lookup2);
    assert(quote.accepted);
    book2.apply_snapshot({{.54,100}},{{.55,100}});
    capped.maintain_open_orders(lookup2,Clock::wall_ns()/1'000'000LL+10);
    book2.apply_snapshot({{.53,100}},{{.54,100}});
    capped.maintain_open_orders(lookup2,Clock::wall_ns()/1'000'000LL+20);
    assert(capped_positions.total_deployed_usd()<=5.000001);
    assert(capped.cancel_order(quote.client_order_id));

    // An explicitly authorized cockpit force-sell uses only visible bid depth
    // and still honors its minimum limit price.
    auto sell=maker;
    sell.source="HUMAN_OVERRIDE";
    sell.manual_override_taker_authorized=true;
    sell.condition_id="maker-test";
    sell.token_id="1";
    sell.dir=OrderDir::Sell;
    sell.execution_mode=ExecutionMode::Taker;
    sell.size_shares=5;
    sell.limit_price=.50;
    book.apply_snapshot({{.49,2}},{{.50,3}});
    assert(!exec.submit(sell,lookup).accepted);
    sell.limit_price=.49;
    const auto sold=exec.submit(sell,lookup);
    assert(sold.accepted && sold.fill_shares==2);

    // A fixed-price maker entry stays at or below its configured cap even if
    // the ask moves down to it; a BBO touch alone still does not fill it.
    maker.condition_id="checkpoint-price";
    maker.source.clear();
    maker.dir=OrderDir::Buy;
    maker.execution_mode=ExecutionMode::Maker;
    maker.size_shares=0;
    maker.manual_override_taker_authorized=false;
    maker.maker_fixed_price=true;
    maker.limit_price=.60;
    book2.apply_snapshot({{.60,100}},{{.61,100}});
    const auto checkpoint=capped.submit(maker,lookup2);
    assert(checkpoint.accepted && checkpoint.resting && checkpoint.order_price==.60);
    book2.apply_snapshot({{.59,100}},{{.60,100}});
    capped.maintain_open_orders(lookup2,Clock::wall_ns()/1'000'000LL+20);
    const auto checkpoint_orders=capped.open_orders();
    assert(checkpoint_orders.size()==1);
    assert(checkpoint_orders.front().req.condition_id=="checkpoint-price");
    assert(checkpoint_orders.front().price<=maker.limit_price);
    assert(capped_positions.snapshot().empty());
    assert(capped.cancel_order(checkpoint.client_order_id));
    maker.maker_fixed_price=false;

    // A duplicated immutable intent must never create a second paper fill.
    Positions idempotent_positions;
    std::unordered_set<std::string> reserved_intents;
    int callbacks = 0;
    Journal durable_journal{
        [&](const OrderRequest&, const OrderResult&) { ++callbacks; },
        [&](const OrderRequest&, const std::string& id) { return reserved_intents.insert(id).second; }
    };
    PaperExecutor idempotent(idempotent_positions,durable_journal,100,100,100,1,5);
    auto repeated = maker;
    repeated.condition_id = "idempotent";
    repeated.intent_id = "11111111-1111-1111-1111-111111111111";
    const auto first_intent = idempotent.submit(repeated,lookup2);
    assert(first_intent.accepted && first_intent.resting && callbacks == 1);
    const auto duplicate_intent = idempotent.submit(repeated,lookup2);
    assert(!duplicate_intent.accepted && callbacks == 1);
    const auto reserved_orders=idempotent.open_orders();
    assert(reserved_orders.size()==1 &&
           reserved_orders.front().client_order_id==first_intent.client_order_id);
    Positions unavailable_positions;
    PaperExecutor unavailable(unavailable_positions,Journal{{},[](const OrderRequest&,const std::string&){return false;}},100,100,100,1,5);
    assert(!unavailable.submit(repeated,lookup2).accepted && unavailable_positions.snapshot().empty());

    // Minimum-size rounding cannot enlarge a user's dollar instruction.
    maker.condition_id="minimum-budget";maker.size_usd=1;
    assert(!capped.submit(maker,lookup2).accepted);

    Positions fee_loss;
    fee_loss.apply_fill("fees","BTC",300,"1",ShareSide::UP,OrderDir::Buy,.5,10,0,.1);
    fee_loss.apply_fill("fees","BTC",300,"1",ShareSide::UP,OrderDir::Sell,.51,10,0,.1);
    assert(fee_loss.trade_stats().wins==0 && fee_loss.trade_stats().losses==1);
    assert(std::abs(fee_loss.net_pnl_usd()+.1)<1e-9);

    // Passive exits reserve inventory, preserve target, fill partially from
    // public prints, and cannot oversell after cancellation.
    {
        Positions inv;std::vector<OrderResult> updates;
        PaperExecutor e(inv,Journal{[&](const auto&,const auto& r){updates.push_back(r);},{}},100,100,100,1,5);
        inv.apply_fill("cycle","BTC",900,"cy",ShareSide::UP,OrderDir::Buy,.4,12.5,0,0);
        OrderBook ob;ob.apply_snapshot({{.39,100}},{{.41,100}});
        auto lk=[&](const auto&,auto)->const OrderBook* {return &ob;};
        OrderRequest target;target.condition_id="cycle";target.token_id="cy";target.asset="BTC";
        target.horizon_s=900;target.dir=OrderDir::Sell;target.execution_mode=ExecutionMode::Maker;
        target.limit_price=.55;target.size_shares=12.5;target.maker_fixed_price=true;
        target.maker_max_age_ms=10000;
        auto rest=e.submit(target,lk);assert(rest.resting && std::abs(rest.order_price-.55)<1e-9);
        assert(!e.submit(target,lk).accepted); // all inventory already reserved
        auto hybrid_exit=target;hybrid_exit.condition_id="hybrid-sell";
        hybrid_exit.execution_mode=ExecutionMode::Hybrid;
        // A hybrid maker SELL expiry must not trigger a taker sell.
        inv.apply_fill("hybrid-sell","BTC",900,"cy2",ShareSide::UP,OrderDir::Buy,.4,10,0,0);
        hybrid_exit.token_id="cy2";hybrid_exit.size_shares=10;hybrid_exit.maker_max_age_ms=1;
        auto hrest=e.submit(hybrid_exit,lk);assert(hrest.resting);
        e.maintain_open_orders(lk,Clock::wall_ns()/1'000'000LL+10);
        assert(inv.snapshot().size()==2);
        assert(updates.back().status=="cancelled");
        e.cancel_order(hrest.client_order_id);
        inv.apply_fill("hybrid-sell","BTC",900,"cy2",ShareSide::UP,OrderDir::Sell,.5,10,0,0);
        ob.apply_snapshot({{.55,4}},{{.56,100}});
        const auto tm=Clock::wall_ns()/1'000'000LL;
        assert(e.on_public_trade("cycle","cy",ShareSide::UP,OrderDir::Buy,
                                 .55,104,tm+100,tm+100)==1);
        assert(updates.back().status=="partial" && updates.back().fill_shares==4);
        assert(std::abs(inv.snapshot()[0].shares-8.5)<1e-9);
        e.maintain_open_orders(lk,tm+200);
        assert(std::abs(inv.snapshot()[0].shares-8.5)<1e-9);
        assert(e.cancel_order(rest.client_order_id));assert(!e.cancel_order(rest.client_order_id));
        ob.apply_snapshot({{.55,100}},{{.56,100}});
        e.maintain_open_orders(lk,tm+300);assert(std::abs(inv.snapshot()[0].shares-8.5)<1e-9);
        target.execution_mode=ExecutionMode::Taker;
        target.source="HUMAN_OVERRIDE";
        target.manual_override_taker_authorized=true;
        assert(e.submit(target,lk).fill_shares==8.5 && inv.snapshot().empty());
    }
    // The strategy cycles only after a real simulated exit; partial buys first
    // cancel their remainder, fixed targets are preserved, and stale data never trades.
    {
        Recycler engine; StrategyHot cfg;cfg.recycler_mode="sniper";
        cfg.max_book_age_ms=1000;cfg.recycler_max_cycles=1;
        cfg.recycler_momentum_lookback_secs=1;cfg.recycler_min_momentum_ticks=5;
        cfg.recycler_target_profit_pct=37.5;
        cfg.maker_improve_ticks=1;
        int64_t tm=Clock::wall_ns()/1'000'000LL;
        RecyclerBook b;b.market.condition_id="recycle";b.market.token_id="rc";b.market.asset="BTC";
        b.market.horizon_s=900;b.market.window_close_ms=tm+800000;
        b.bid=.39;b.ask=.41;b.updated_ms=tm;b.generation=1;
        b.reference_price=100.0;b.reference_strike=100.0;b.reference_updated_ms=tm;
        b.board_forecast.ready=true;b.board_forecast.probability=.80;
        assert(engine.step(cfg,tm,{b},{},{},0,true).empty());
        b.bid=.42;b.ask=.44;b.updated_ms=tm+1000;b.reference_updated_ms=tm+1000;b.generation=2;
        assert(engine.step(cfg,tm+1000,{b},{},{},0,true).empty());
        b.bid=.48;b.ask=.50;b.updated_ms=tm+2000;b.reference_updated_ms=tm+2000;b.generation=3;
        auto a=engine.step(cfg,tm+2000,{b},{},{},0,true);
        assert(a.size()==1 && a[0].order.dir==OrderDir::Buy && a[0].order.execution_mode==ExecutionMode::Maker);
        Position p;p.condition_id="recycle";p.token_id="rc";p.asset="BTC";p.shares=10;
        p.avg_price=.4;p.window_close_ms=b.market.window_close_ms;p.opened_ms=tm;
        PaperExecutor::OpenOrderView entry{a[0].order,"entry",.4,2.5};
        b.updated_ms=tm+2500;
        a=engine.step(cfg,tm+2500,{b},{p},{entry},0,true);assert(a.size()==1 && a[0].cancel_id=="entry");
        a=engine.step(cfg,tm+2500,{b},{p},{},0,true);
        assert(a.size()==1 && a[0].order.dir==OrderDir::Sell && a[0].order.limit_price==.55);
        PaperExecutor::OpenOrderView exit{a[0].order,"target",.55,10};
        b.bid=.30;b.ask=.31;b.updated_ms=tm+3500;b.generation=2;
        a=engine.step(cfg,tm+3500,{b},{p},{exit},0,true);
        assert(a.empty()); // price loss alone does not cancel target or sell inventory
        a=engine.step(cfg,tm+3500,{b},{p},{},0,true);
        assert(a.size()==1 && a[0].order.source=="RECYCLER_PROFIT_TARGET");
        assert(engine.step(cfg,tm+4500,{b},{p},{exit},0,true).empty()); // unchanged target/depth
        b.updated_ms=tm+6500;
        engine.step(cfg,tm+6500,{b},{},{},-1,true);
        b.bid=.39;b.ask=.41;b.updated_ms=tm+9500;
        assert(engine.step(cfg,tm+9500,{b},{},{},-1,true).empty()); // per-market cycle cap
        assert(engine.completed_cycles==1);
        Recycler stale;
        assert(stale.step(cfg,tm+2000,{b},{},{},0,true).empty()); // future timestamp
        b.updated_ms=tm;
        assert(stale.step(cfg,tm+2000,{b},{},{},0,true).empty()); // stale timestamp
        b.updated_ms=tm+2000;
        assert(stale.step(cfg,tm+2000,{b},{},{},-11,true).empty()); // session loss
    }
    // Repeat after a completed target; entry pause still manages exits.
    {
        Recycler e;StrategyHot c;c.recycler_mode="sniper";
        c.recycler_momentum_lookback_secs=1;c.recycler_min_momentum_ticks=5;
        c.maker_improve_ticks=1;c.recycler_min_settlement_edge_pct=0;c.recycler_target_profit_pct=37.5;
        int64_t now=Clock::wall_ns()/1'000'000LL;
        RecyclerBook b;b.market.condition_id="repeat";b.market.token_id="repeat";b.market.asset="ETH";
        b.market.horizon_s=900;b.market.window_close_ms=now+800000;
        b.bid=.39;b.ask=.41;b.updated_ms=now;b.generation=1;
        b.reference_price=100;b.reference_strike=100;b.reference_updated_ms=now;
        b.board_forecast.ready=true;b.board_forecast.probability=.80;
        assert(e.step(c,now,{b},{},{},0,true).empty());
        b.bid=.42;b.ask=.44;b.updated_ms=now+1000;b.reference_updated_ms=now+1000;b.generation=2;
        assert(e.step(c,now+1000,{b},{},{},0,true).empty());
        b.bid=.48;b.ask=.50;b.updated_ms=now+2000;b.reference_updated_ms=now+2000;b.generation=3;
        assert(e.step(c,now+2000,{b},{},{},0,true).size()==1);
        Position p;p.condition_id="repeat";p.token_id="repeat";p.shares=12.5;p.avg_price=.4;
        p.opened_ms=now;p.window_close_ms=b.market.window_close_ms;
        b.updated_ms=now+2500;
        auto a=e.step(c,now+2500,{b},{p},{},0,false);assert(a.size()==1 && a[0].order.source=="RECYCLER_PROFIT_TARGET");
        e.step(c,now+3500,{b},{},{},1.875,true);
        b.bid=.50;b.ask=.52;b.updated_ms=now+20000;b.reference_updated_ms=now+20000;b.generation=4;
        assert(e.step(c,now+20000,{b},{},{},1.875,true).empty());
        b.bid=.55;b.ask=.57;b.updated_ms=now+21000;b.reference_updated_ms=now+21000;b.generation=5;
        assert(e.step(c,now+21000,{b},{},{},1.875,true).empty());
        b.bid=.61;b.ask=.63;b.updated_ms=now+22000;b.reference_updated_ms=now+22000;b.generation=6;
        a=e.step(c,now+22000,{b},{},{},1.875,true);
        assert(a.size()==1 && a[0].order.dir==OrderDir::Buy && e.completed_cycles==1);
        p.window_close_ms=now+65000;b.market.window_close_ms=p.window_close_ms;b.updated_ms=now+24500;b.generation=7;
        a=e.step(c,now+24500,{b},{p},{},1.875,false);
        assert(a.size()==1 && a[0].order.source=="RECYCLER_LATE_MAKER_EXIT" &&
               a[0].order.execution_mode==ExecutionMode::Maker);
        Recycler range;c.recycler_mode="range";c.recycler_lookback_secs=30;
        b.market.window_close_ms=now+800000;
        bool entered=false;
        for(int i=0;i<65;++i) {
            auto t=now+i*500;b.updated_ms=t;b.reference_updated_ms=t;b.generation=i+1;
            double mid=.30+.03*i;b.bid=mid-.01;b.ask=mid+.01;
            auto actions=range.step(c,t,{b},{},{},0,true);
            if(!actions.empty()){assert(actions[0].order.source=="RECYCLER_RANGE_ENTRY");entered=true;break;}
        }
        assert(entered);
    }
    // User-requested zero disables only the price trigger, including a deep dip.
    {
        Recycler e;StrategyHot c;c.recycler_mode="sniper";c.recycler_allow_taker_exit=true;c.recycler_stop_ticks=0;
        c.recycler_momentum_lookback_secs=1;c.recycler_min_momentum_ticks=5;
        c.maker_improve_ticks=1;c.recycler_min_settlement_edge_pct=0;
        const int64_t now=Clock::wall_ns()/1'000'000LL;
        RecyclerBook b;b.market.condition_id="no-stop";b.market.token_id="no-stop";b.market.asset="BTC";
        b.market.horizon_s=900;b.market.window_close_ms=now+700000;
        b.updated_ms=now;b.bid=.39;b.ask=.41;b.generation=1;
        b.reference_price=100;b.reference_strike=100;b.reference_updated_ms=now;
        b.board_forecast.ready=true;b.board_forecast.probability=.80;
        assert(e.step(c,now,{b},{},{},0,true).empty());
        b.updated_ms=now+1000;b.bid=.42;b.ask=.44;b.reference_updated_ms=now+1000;b.generation=2;
        assert(e.step(c,now+1000,{b},{},{},0,true).empty());
        b.updated_ms=now+2000;b.bid=.48;b.ask=.50;b.reference_updated_ms=now+2000;b.generation=3;
        auto a=e.step(c,now+2000,{b},{},{},0,true);assert(a.size()==1);
        Position p;p.condition_id="no-stop";p.token_id="no-stop";p.shares=12.5;p.avg_price=.4;
        p.opened_ms=now;p.window_close_ms=b.market.window_close_ms;
        b.bid=.1;b.ask=.11;b.updated_ms=now+3000;
        a=e.step(c,now+3000,{b},{p},{},0,true);
        assert(a.size()==1 && a[0].order.source=="RECYCLER_PROFIT_TARGET");
    }
    // Strict parser and mode boundary; test isolated minimal configs.
    {
        const auto path=std::filesystem::temp_directory_path()/"polysparq-recycler-parser.toml";
        auto parse=[&](const std::string& txt){std::ofstream f(path);f<<txt;f.close();return Config::load(path.string());};
        bool rejected=false;
        try {parse("[runtime]\nmode='live'\n[strategy.hot]\nrecycler_mode='sniper'\nrecycler_allow_taker_exit=true\n");} catch(...) {rejected=true;}
        assert(rejected);rejected=false;
        try {parse("[strategy.hot]\nrecycler_max_cycles='fifteen'\n");} catch(...) {rejected=true;}
        assert(rejected);rejected=false;
        try {parse("[strategy.hot]\nrecycler_entry_price=0.6\nrecycler_target_price=0.5\n");} catch(...) {rejected=true;}
        assert(rejected);
        assert(parse("[polymarket]\nuniverse_horizons_seconds=[900]\n"
                     "[strategy.hot]\nrecycler_mode='sniper'\nrecycler_entry_cutoff_secs=360\n")
                     .strategy.recycler_mode=="sniper");
        std::filesystem::remove(path);
    }

    std::cout << "maker lifecycle tests passed\n";
    return 0;
}
