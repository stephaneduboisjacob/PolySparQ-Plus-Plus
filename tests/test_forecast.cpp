#include "engine/forecast.hpp"
#include "engine/candle_store.hpp"
#include "core/oracle_buffer.hpp"
#include "feeds/polymarket_rest.hpp"
#include "db/persist_queue.hpp"
#include <future>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <limits>
#include <iostream>
int main() {
    using namespace vp::engine;
    ForecastInput in{100,100,300,0.001,1,1,240,60};
    auto f=forecast(in);
    assert(f.ready && f.probability==0.5 && f.low==f.high);
    in.spot=100.1; const auto above=forecast(in);
    assert(above.probability>0.5 && above.low<=above.probability && above.high>=above.probability);
    in.seconds_left=90; assert(forecast(in).probability>above.probability);
    in.seconds_left=60; assert(!forecast(in).ready && std::string(forecast(in).status)=="averaging_window");
    in.seconds_left=300; in.sigma_minute=0.002; assert(forecast(in).probability<above.probability);
    in.reference_age_s=11; assert(!forecast(in).ready);
    in.reference_age_s=-2; assert(!forecast(in).ready);
    in.reference_age_s=1; in.candle_age_s=121; assert(!forecast(in).ready);
    in.candle_age_s=1; in.twap_window_s=-1; assert(!forecast(in).ready);
    in.twap_window_s=60; in.strike=0; assert(!forecast(in).ready);
    in.strike=100; in.spot=std::numeric_limits<double>::quiet_NaN(); assert(!forecast(in).ready);
    auto edge=execution_edge(0.6,0.56,0.54,0.55,0.07,1);
    assert(edge.executable && edge.net>0 && edge.conservative<0); // apparent edge fails stress
    assert(!execution_edge(0.9,0.8,0.7,0.6,0.07,1).executable); // crossed
    assert(!execution_edge(0.9,0.8,0,0.6,0.07,1).executable); // missing book
    assert(!execution_edge(0.9,0.8,0.5,0.6,-1,1).executable); // missing fee
    std::vector<double> closes(241,100);
    assert(estimate_volatility(closes).samples==240);
    closes.back()=101; assert(estimate_volatility(closes).sigma_minute>0.001);
    closes.back()=0; assert(estimate_volatility(closes).samples==0);
    std::vector<double> trending(121,100);
    for(size_t i=1;i<trending.size();++i) trending[i]=trending[i-1]*1.0001;
    const auto trend_vol=estimate_volatility(trending);
    const auto drift=estimate_drift(trending,trend_vol.sigma_minute);
    assert(drift.samples==61 && drift.log_drift_minute>0);
    ForecastInput next{100,100,300,trend_vol.sigma_minute,1,1,
                       trend_vol.samples,60,drift.log_drift_minute};
    assert(forecast(next).ready && forecast(next).probability>0.5);
    std::reverse(trending.begin(),trending.end());
    const auto reverse_vol=estimate_volatility(trending);
    assert(estimate_drift(trending,reverse_vol.sigma_minute).log_drift_minute<0);
    // Correct closed OHLCV flows to ALL higher timeframes. New-minute ticks
    // must never become the close/volume of the previous aggregate.
    CandleStore store;
    constexpr int64_t base=3'600'000;
    for(int i=0;i<=60;++i) {
        store.on_tick("BTC",100+i,1,false,base+i*60000);
        if(i<60) store.on_tick("BTC",101+i,2,true,base+i*60000+30000);
    }
    auto m=store.copy_recent_bars("BTC",Timeframe::OneMinute,100);
    assert(m.size()==60 && m.front().open==100 && m.front().close==101 && m.front().volume==3);
    for (auto tf : {Timeframe::FiveMinutes,Timeframe::FifteenMinutes,Timeframe::SixtyMinutes}) {
        auto bars=store.copy_recent_bars("BTC",tf,100);
        int duration=int(tf);
        assert(bars.size()==size_t(60/duration));
        assert(bars.front().open==100 && bars.front().close==100+duration);
        assert(bars.front().volume==3*duration && bars.front().ts_ms==base);
    }
    assert(store.copy_recent_closes("BTC",Timeframe::OneMinute,-1).empty());
    assert(store.copy_recent_bars("ETH",Timeframe::SixtyMinutes,10).empty());
    store.on_tick("BTC",1000,100,false,base+59*60000+40000); // out of order
    store.on_tick("BTC",161,1,false,base+61*60000);
    assert(store.copy_recent_bars("BTC",Timeframe::OneMinute,1).back().close==160);
    CandleStore gap;
    gap.on_tick("BTC",100,1,false,base);
    gap.on_tick("BTC",101,1,false,base+3*60000);
    gap.on_tick("BTC",102,1,false,base+4*60000);
    gap.on_tick("BTC",103,1,false,base+5*60000);
    assert(gap.copy_recent_bars("BTC",Timeframe::FiveMinutes,5).empty());
    assert(gap.copy_recent_closes("BTC",Timeframe::OneMinute,10).size()==2);
    CandleStore reconnect;
    reconnect.on_tick("BTC",100,1,false,base+30000);
    reconnect.on_tick("BTC",101,1,false,base+60000);
    assert(reconnect.copy_recent_bars("BTC",Timeframe::OneMinute,10).empty());
    reconnect.on_tick("BTC",102,1,false,base+120000);
    assert(reconnect.copy_recent_bars("BTC",Timeframe::OneMinute,10).size()==1);
    // Archive/live handoff: skip a partial startup minute, then repair it
    // from complete exchange candles without fabricating prices or counting
    // an aggregate twice. Ignore open/future and malformed archive bars.
    std::vector<Candle> history;
    for (int i=0;i<60;++i)
        history.push_back({base+i*60000,100,101,99,100,3});
    CandleStore handoff;
    handoff.merge_closed_candles("BTC",history,base+60*60000);
    handoff.on_tick("BTC",100,1,false,base+60*60000+30000);
    handoff.on_tick("BTC",100,1,false,base+61*60000);
    handoff.on_tick("BTC",100,1,false,base+62*60000);
    assert(handoff.copy_recent_closes("BTC",Timeframe::OneMinute,100).size()==1);
    const Candle missing{base+60*60000,100,102,99,101,3};
    const auto revision=handoff.closed_revision();
    handoff.merge_closed_candles("BTC",std::span(&missing,1),base+62*60000);
    assert(handoff.closed_revision()>revision);
    assert(handoff.copy_recent_closes("BTC",Timeframe::OneMinute,100).size()==62);
    assert(handoff.copy_recent_bars("BTC",Timeframe::OneMinute,1).back().ts_ms==base+61*60000);
    const auto repaired_revision=handoff.closed_revision();
    handoff.merge_closed_candles("BTC",std::span(&missing,1),base+62*60000);
    assert(handoff.closed_revision()==repaired_revision); // idempotent
    Candle open_bar{base+62*60000,100,102,99,101,100};
    handoff.merge_closed_candles("BTC",std::span(&open_bar,1),base+62*60000+30000);
    assert(handoff.closed_revision()==repaired_revision);
    open_bar.ts_ms=base+60*60000; open_bar.close=0;
    handoff.merge_closed_candles("BTC",std::span(&open_bar,1),base+62*60000);
    assert(handoff.closed_revision()==repaired_revision);
    for(int i=63;i<=65;++i) handoff.on_tick("BTC",100,1,false,base+i*60000);
    auto repaired_five=handoff.copy_recent_bars("BTC",Timeframe::FiveMinutes,100);
    assert(repaired_five.size()==13 && repaired_five.back().ts_ms==base+60*60000);
    assert(repaired_five.back().high==102 && repaired_five.back().volume==7);
    auto corrected=missing; corrected.high=103; corrected.volume=4;
    handoff.merge_closed_candles("BTC",std::span(&corrected,1),base+65*60000);
    repaired_five=handoff.copy_recent_bars("BTC",Timeframe::FiveMinutes,100);
    assert(repaired_five.size()==13 && repaired_five.back().high==103 && repaired_five.back().volume==8);
    vp::core::OracleBuffer ref;
    ref.add("BTC:TWAP60",1000,100);
    // Historical reads are uncapped by default: the tick at-or-before the
    // target is the historical reference regardless of wall-clock time.
    // Freshness is measured from the queried timestamp to the selected tick.
    assert(ref.lookup_at_or_before("BTC:TWAP60",1011)==100);
    // Explicit freshness gates still work for callers that want them.
    assert(ref.lookup_at_or_before("BTC:TWAP60",1011,10)==0);
    assert(ref.lookup_at_or_before("BTC:TWAP60",1005)==100);
    assert(ref.lookup_at_or_before("BTC",1005)==0); // sources cannot alias
    const std::string resolved=R"([{"conditionId":"c","closed":true,"umaResolutionStatus":"resolved","outcomes":"[\"Down\",\"Up\"]","outcomePrices":"[\"1\",\"0\"]"}])";
    auto outcome=vp::PolymarketRest::parse_resolved_up_outcome(resolved,"c");
    assert(outcome.has_value() && !*outcome);
    assert(!vp::PolymarketRest::parse_resolved_up_outcome(resolved,"other"));
    assert(!vp::PolymarketRest::parse_resolved_up_outcome("[]","c"));
    auto unresolved=resolved;
    unresolved.replace(unresolved.find("resolved"),8,"proposed");
    assert(!vp::PolymarketRest::parse_resolved_up_outcome(unresolved,"c"));
    vp::BoundedQueue<int> queue(1);
    assert(queue.push(1));
    assert(!queue.push(99)); // research overflow is allowed
    auto critical=std::async(std::launch::async,[&]{return queue.push_wait(2);});
    int value=0;
    assert(queue.pop(value,0) && value==1);
    assert(critical.get());
    assert(queue.pop(value,0) && value==2); // accounting overflow waits, never drops
    queue.shutdown();
    assert(!queue.push_wait(3));
    std::cout << "forecast/reference/candle regressions passed\n";
}
