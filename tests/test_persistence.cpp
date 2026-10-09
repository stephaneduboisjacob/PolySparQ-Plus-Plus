#include "db/pg.hpp"
#include "engine/candle_store.hpp"
#include "engine/forecast.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdlib>
#include <iostream>

int main() {
    const char* dsn=std::getenv("POLYPULSE_TEST_DSN");
    if (!dsn) return 77;
    vp::Pg db(dsn);
    {
        pqxx::work tx(db.conn());
        const auto name=tx.exec1("SELECT current_database()")[0].as<std::string>();
        if (name.rfind("polypulse_test_",0)!=0) return 2;
        tx.exec(R"(
            CREATE SCHEMA vp;
            CREATE TYPE vp.trade_mode AS ENUM ('paper','live','replay');
            CREATE TYPE vp.share_side AS ENUM ('UP','DOWN');
            CREATE TYPE vp.order_side AS ENUM ('buy','sell');
            CREATE TYPE vp.order_status AS ENUM ('filled','partial','acked','cancelled','rejected');
            CREATE TABLE vp.orders (
              client_order_id uuid PRIMARY KEY,exchange_order_id text,execution_mode text,
              post_only bool,mode vp.trade_mode,condition_id text,token_id text,
              side_label vp.share_side,direction vp.order_side,price float8,size_shares float8,
              notional_usd float8,status vp.order_status,source text,reject_reason text,
              ts_submitted timestamptz,ts_terminal timestamptz,session_id uuid,instance text);
            CREATE TABLE vp.fills (ts timestamptz,client_order_id uuid REFERENCES vp.orders,
              price float8,size_shares float8,fee_usd float8 CHECK(fee_usd>=0));
            CREATE TABLE vp.signals (ts timestamptz,condition_id text,side_label vp.share_side,
              direction vp.order_side,confidence float8,recommended text,rec_size_usd float8,
              rec_limit_price float8,reason text,model_version text,payload jsonb);
            CREATE TABLE vp.positions_history (ts timestamptz,mode vp.trade_mode,cash_usd float8,
              equity_usd float8,realized_pnl float8,unrealized_pnl float8,deployed_usd float8,
              open_positions jsonb,instance text,session_id uuid);
            CREATE TABLE vp.candles_1m (symbol text,ts timestamptz,open float8,high float8,
              low float8,close float8,volume float8,PRIMARY KEY(symbol,ts));
            INSERT INTO vp.candles_1m
              SELECT symbol,to_timestamp(3600+n*60),100,101,99,100,1
              FROM generate_series(0,61) n
              CROSS JOIN (VALUES ('BTCUSDT'),('ETHUSDT'),('SOLUSDT'),('XRPUSDT')) s(symbol)
              WHERE n<>60;
        )");
        tx.commit();
    }
    // The schema now exists; a new Pg connection can apply optional event
    // migrations without touching the primary order/fill projection.
    vp::Pg event_db(dsn);
    const std::string id="11111111-1111-1111-1111-111111111111";
    db.set_session_context(id,"regression");
    event_db.set_session_context(id,"regression");
    assert(!event_db.has_uncertain_live_order_for_instance());
    const std::string crash_intent="33333333-3333-3333-3333-333333333333";
    assert(event_db.create_order_intent(crash_intent,"live","c","token",vp::ShareSide::UP,
        vp::OrderDir::Buy,"maker",4.2,10,.42,"test"));
    assert(!event_db.create_order_intent(crash_intent,"live","c","token",vp::ShareSide::UP,
        vp::OrderDir::Buy,"maker",4.2,10,.42,"test"));
    assert(event_db.has_uncertain_live_order_for_instance());
    // Simulate crash before any outcome callback: a new process still sees it.
    {
        vp::Pg restarted(dsn);
        restarted.set_session_context(id,"regression");
        assert(restarted.has_uncertain_live_order_for_instance());
    }
    {
        pqxx::work tx(db.conn());
        tx.exec("UPDATE vp.order_intents SET state='rejected' WHERE intent_id='33333333-3333-3333-3333-333333333333'");
        tx.commit();
    }
    assert(!event_db.has_uncertain_live_order_for_instance());
    db.insert_signal("c",vp::ShareSide::UP,vp::OrderDir::Buy,.7,"BUY_UP",5,.6,
                     "test","test",R"({"ask":0.6})",id);
    db.insert_signal("empty",vp::ShareSide::DOWN,vp::OrderDir::Buy,.7,"BUY_DOWN",5,.6,"test");
    {
        pqxx::work tx(db.conn());
        const auto row=tx.exec1("SELECT payload->>'instance',payload->>'session_id',"
                               "payload->>'client_order_id',payload->>'ask' FROM vp.signals WHERE condition_id='c'");
        assert(row[0].as<std::string>()=="regression");
        assert(row[1].as<std::string>()==id && row[2].as<std::string>()==id);
        assert(row[3].as<double>()==.6);
        assert(tx.exec1("SELECT payload->>'instance' FROM vp.signals WHERE condition_id='empty'")[0].as<std::string>()=="regression");
    }
    // Failed fill must roll back its order too, not leave a phantom filled order.
    db.insert_order(id,"","taker","paper","c","token",vp::ShareSide::UP,vp::OrderDir::Sell,
                    0,7,0,"filled","settle","",0,7,-1);
    {
        pqxx::work tx(db.conn());
        assert(tx.exec1("SELECT count(*) FROM vp.orders")[0].as<int>()==0);
    }
    event_db.insert_order(id,"","taker","paper","c","token",vp::ShareSide::UP,vp::OrderDir::Sell,
                    0,7,0,"filled","settle","",0,7,0);
    const std::string uncertain_id="22222222-2222-2222-2222-222222222222";
    event_db.insert_order(uncertain_id,"","maker","live","c","token",
                          vp::ShareSide::UP,vp::OrderDir::Buy,
                          .42,10,4.2,"uncertain","test","transport outcome unknown");
    assert(event_db.has_uncertain_live_order_for_instance());
    event_db.set_session_context(id,"another-instance");
    assert(!event_db.has_uncertain_live_order_for_instance());
    event_db.set_session_context(id,"regression");
    db.snapshot_positions("paper",10,10,0,0,0,"[]");
    vp::engine::CandleStore candles;
    constexpr int64_t now_ms=3'600'000+62*60'000;
    candles.refresh_recent_from_pg(db,now_ms);
    assert(candles.copy_recent_closes("BTC",vp::engine::Timeframe::OneMinute,100).size()==1);
    {
        pqxx::work tx(db.conn());
        tx.exec("INSERT INTO vp.candles_1m SELECT symbol,to_timestamp(7200),100,101,99,100,1 "
                "FROM (VALUES ('BTCUSDT'),('ETHUSDT'),('SOLUSDT'),('XRPUSDT')) s(symbol)");
        tx.commit();
    }
    candles.refresh_recent_from_pg(db,now_ms);
    for(const auto* asset : {"BTC","ETH","SOL","XRP"}) {
        auto closes=candles.copy_recent_closes(asset,vp::engine::Timeframe::OneMinute,100);
        assert(closes.size()==62 && vp::engine::estimate_volatility(closes).samples==61);
    }
    {
        pqxx::work tx(db.conn());
        assert(tx.exec1("SELECT count(*) FROM vp.orders JOIN vp.fills USING(client_order_id)")[0].as<int>()==1);
        assert(tx.exec1("SELECT price FROM vp.fills")[0].as<double>()==0);
        assert(tx.exec1("SELECT instance FROM vp.positions_history")[0].as<std::string>()=="regression");
        assert(tx.exec1("SELECT session_id::text FROM vp.positions_history")[0].as<std::string>()==id);
        assert(tx.exec1("SELECT count(*) FROM vp.order_events")[0].as<int>()==2);
        const auto event=tx.exec1("SELECT instance,status,fill_delta_shares FROM vp.order_events "
                                  "WHERE client_order_id='11111111-1111-1111-1111-111111111111'");
        assert(event[0].as<std::string>()=="regression");
        assert(event[1].as<std::string>()=="filled");
        assert(event[2].as<double>()==7);
        assert(tx.exec1("SELECT status::text FROM vp.orders WHERE client_order_id='22222222-2222-2222-2222-222222222222'")[0].as<std::string>()=="uncertain");
        tx.exec("DROP SCHEMA vp CASCADE");
        tx.commit();
    }
    std::cout<<"atomic order/fill and scoped snapshot regressions passed\n";
}
