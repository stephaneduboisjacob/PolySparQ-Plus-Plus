#pragma once
// PAPER-only repeat-cycle strategy for 5- and 15-minute binary markets.
#include "config/config.hpp"
#include "exec/paper.hpp"
#include "engine/forecast.hpp"
#include <deque>
#include <map>
#include <set>
#include <algorithm>
#include <cmath>
#include <fmt/format.h>

namespace vp {
struct RecyclerBook {
    OrderRequest market;
    double bid=0, ask=0;
    int64_t updated_ms=0;
    uint64_t generation=0;
    int64_t window_open_ms=0;
    int settlement_window_s=-1;
    std::string reference_key;
    double reference_price=0, reference_strike=0, forecast_sigma_per_sec=0;
    int64_t reference_updated_ms=0;
    engine::Forecast board_forecast;
};
struct RecyclerAction {
    std::string cancel_id;
    OrderRequest order;
    std::string reason = {};
};
class Recycler {
    struct Window { int cycles=0; double loss=0; int64_t close=0; };
    struct TokenCycle { std::string condition; bool had_inventory=false; double entry_net=0; };
    std::map<std::string,Window> windows_;
    std::map<std::string,std::deque<std::pair<int64_t,double>>> history_;
    std::map<std::string,TokenCycle> token_cycles_;
    std::map<std::string,int64_t> last_entry_attempt_ms_;
    std::map<std::string,uint64_t> last_late_taker_generation_;
    std::map<std::string,int64_t> last_late_maker_attempt_ms_;
    std::set<std::string> late_risk_tokens_;
    std::set<std::string> settlement_entry_attempted_;
    std::set<std::string> settlement_hold_tokens_;
    int64_t retry_ms_=0;

    static bool rising(const std::deque<std::pair<int64_t,double>>& h,
                       int64_t now, int lookback_secs, int min_ticks,
                       double tick_size) {
        if (h.size()<3 || tick_size<=0 || now<h.back().first || now-h.back().first>1200) return false;
        const int64_t cutoff=now-static_cast<int64_t>(lookback_secs)*1000;
        auto it=std::find_if(h.rbegin(),h.rend(),[&](const auto& p){return p.first<=cutoff;});
        if(it==h.rend()) return false;
        return h.back().second-it->second+1e-9>=min_ticks*tick_size;
    }
public:
    bool engaged() const {
        if(!token_cycles_.empty()) return true;
        for(const auto& [_,h]:history_) if(!h.empty()) return true;
        return false;
    }
    std::string status="WARMING";
    int completed_cycles=0;
    double session_net=0;

    std::vector<RecyclerAction> step(const StrategyHot& c, int64_t now,
        const std::vector<RecyclerBook>& books,const std::vector<Position>& positions,
        const std::vector<PaperExecutor::OpenOrderView>& quotes,double net,bool entries_allowed,double maker_min_shares=5.0,
        double max_deployed_usd=0.0) {
        session_net=net;
        std::vector<RecyclerAction> out;
        auto cancel=[&](const auto& q){out.push_back({q.client_order_id,{}});};
        auto book_for=[&](const std::string& token)->const RecyclerBook* {
            for(const auto& b:books) if(b.market.token_id==token) return &b;
            return nullptr;
        };
        auto fresh=[&](const RecyclerBook& b) {
            return b.updated_ms>0 && now>=b.updated_ms && now-b.updated_ms<=c.max_book_age_ms &&
                   b.bid>0 && b.ask>b.bid && b.ask<1;
        };
        auto reference_fresh=[&](const RecyclerBook& b) {
            return b.reference_updated_ms>0 && now>=b.reference_updated_ms &&
                   now-b.reference_updated_ms<=c.recycler_reference_max_age_ms &&
                   b.reference_price>0 && b.reference_strike>0;
        };
        auto fair_side=[&](const RecyclerBook& b, ShareSide side, int64_t remaining_ms) {
            if(!reference_fresh(b) || !b.board_forecast.ready || remaining_ms<=0) return -1.0;
            return side==ShareSide::UP ? b.board_forecast.probability
                                       : 1.0-b.board_forecast.probability;
        };
        auto supported_horizon=[&](int horizon_s) {
            if(horizon_s==300 || horizon_s==900) return true;
            return c.recycler_maniac_mode &&
                (horizon_s==3600 || horizon_s==14400 || horizon_s==86400);
        };
        auto fee_per_share=[&](double price) {
            if(c.taker_fee_rate<=0 || c.fee_exponent<=0) return 0.0;
            const double p=std::clamp(price,0.0,1.0);
            return c.taker_fee_rate*std::pow(p,c.fee_exponent)*
                   std::pow(1.0-p,c.fee_exponent);
        };
        auto net_settlement_edge=[&](double fair,double entry) {
            const double target=std::min(.99,entry*(1.0+c.recycler_target_profit_pct/100.0));
            return fair-entry-fee_per_share(entry)-fee_per_share(target);
        };
        auto target_pct=[&](int horizon_s, int64_t remaining_ms) {
            if(c.recycler_maniac_mode) return c.recycler_target_profit_pct;
            if(horizon_s==300) {
                // 5m schedule: 20% for the first 3:30, then 15/10/5% in
                // successive 15s bands. At the passive-exit deadline there
                // is no profit target; the late-window exit takes over.
                if(remaining_ms>90000) return c.recycler_target_profit_pct;
                if(remaining_ms>75000) return c.recycler_late_target_6m_pct;
                if(remaining_ms>60000) return c.recycler_late_target_4m_pct;
                if(remaining_ms>static_cast<int64_t>(c.recycler_late_maker_secs)*1000LL)
                    return c.recycler_late_target_2m_pct;
                return 0.0;
            }
            if(remaining_ms<=360000) {
                if(remaining_ms>240000) return c.recycler_late_target_6m_pct;
                if(remaining_ms>120000) return c.recycler_late_target_4m_pct;
                return c.recycler_late_target_2m_pct;
            }
            return c.recycler_target_profit_pct;
        };

        for(const auto& b:books) {
            if(!fresh(b)) continue;
            auto& h=history_[b.market.token_id];
            while(!h.empty() && now-h.front().first>std::max(10,c.recycler_lookback_secs)*1000LL)h.pop_front();
            if(h.empty() || now-h.back().first>=250)h.emplace_back(now,(b.bid+b.ask)/2);
            windows_[b.market.condition_id].close=b.market.window_close_ms;
        }
        for(auto it=history_.begin();it!=history_.end();) {
            if(it->second.empty() || now-it->second.back().first>900000) it=history_.erase(it); else ++it;
        }
        for(auto it=windows_.begin();it!=windows_.end();) {
            if(it->second.close>0 && it->second.close<now-900000) it=windows_.erase(it); else ++it;
        }

        std::map<std::string,const Position*> held;
        std::set<std::string> occupied_conditions;
        double deployed_usd=0.0;
        for(const auto& p:positions) {
            held[p.token_id]=&p;
            occupied_conditions.insert(p.condition_id);
            deployed_usd += std::max(0.0,p.shares*p.avg_price);
        }
        double pending_buy_usd=0.0;
        for(const auto& q:quotes) if(q.req.dir==OrderDir::Buy) occupied_conditions.insert(q.req.condition_id);
        for(const auto& q:quotes) if(q.req.dir==OrderDir::Buy)
            pending_buy_usd += std::max(0.0,q.price*q.remaining_shares);
        for(auto& [token,state]:token_cycles_) {
            if(state.had_inventory && !held.contains(token)) {
                auto& w=windows_[state.condition];
                ++w.cycles; ++completed_cycles;
                w.loss+=std::max(0.0,state.entry_net-net);
                state.had_inventory=false;
                state.entry_net=net;
                retry_ms_=now+std::max(250,c.order_cooldown_ms);
            }
        }

        // Inventory policy is driven by remaining time in the market window.
        // The only exits before six minutes are the ordinary profit target.
        for(const auto& p:positions) {
            auto& state=token_cycles_[p.token_id];
            if(!state.had_inventory) { state.had_inventory=true; state.condition=p.condition_id; state.entry_net=net; }
            bool entry_quote=false, sell_quote=false;
            for(const auto& q:quotes) if(q.req.token_id==p.token_id) {
                if(q.req.dir==OrderDir::Buy) {
                    entry_quote=true;
                }
                else sell_quote=true;
            }
            const auto* b=book_for(p.token_id);
            if(!b || !fresh(*b)) { status="STALE_BOOK_HOLD"; continue; }
            const int64_t remaining_ms=b ? b->market.window_close_ms-now : 0;
            if(remaining_ms<=0) {
                for(const auto& q:quotes) if(q.req.token_id==p.token_id) cancel(q);
                status="AWAITING_SETTLEMENT";
                continue;
            }
            if(settlement_hold_tokens_.contains(p.token_id)) {
                if(entry_quote || sell_quote) {
                    for(const auto& q:quotes) if(q.req.token_id==p.token_id && q.req.dir==OrderDir::Sell) cancel(q);
                }
                status="SETTLEMENT_HOLD";
                continue;
            }
            if(entry_quote) { status="WAITING_FOR_ENTRY_FILL"; continue; }
            const double fair=fair_side(*b,p.side,remaining_ms);
            // Loss-triggered exits must not overlap the stepped profit-target
            // bands. They can arm only once the configured final passive-exit
            // phase begins: 120s for 15m profiles, 45s for 5m profiles.
            const int64_t risk_window_ms=
                static_cast<int64_t>(c.recycler_late_maker_secs)*1000LL;
            const bool late_risk_window=remaining_ms>0 && remaining_ms<=risk_window_ms;
            // Profit targets step down earlier: from 6m to 2m for 15m markets,
            // and during the final 90s before the 5m passive-exit phase.
            const bool late_profit_window=remaining_ms>0 &&
                ((p.horizon_s==900 && remaining_ms<=360000LL) ||
                 (p.horizon_s==300 && remaining_ms<=90000LL));
            const bool risk_condition=late_risk_window &&
                remaining_ms>static_cast<int64_t>(c.recycler_late_taker_secs)*1000LL &&
                c.recycler_late_risk_exit_enabled &&
                ((fair>=0 && fair<c.recycler_late_risk_min_prob) ||
                 (c.recycler_late_risk_bid_loss_pct>0 &&
                  b->bid<=p.avg_price*(1.0-c.recycler_late_risk_bid_loss_pct/100.0)));
            if(risk_condition) late_risk_tokens_.insert(p.token_id);
            const bool risk_trigger=late_risk_window && late_risk_tokens_.contains(p.token_id);

            if(remaining_ms>0 && remaining_ms<=c.recycler_late_taker_secs*1000LL &&
               c.recycler_late_taker_exit_enabled &&
               (c.recycler_late_taker_min_bid<=0.0 || b->bid>=c.recycler_late_taker_min_bid)) {
                if(sell_quote) {
                    for(const auto& q:quotes) if(q.req.token_id==p.token_id && q.req.dir==OrderDir::Sell) cancel(q);
                    status="LATE_TAKER_CANCEL_MAKER";
                    continue;
                }
                auto last=last_late_taker_generation_.find(p.token_id);
                if(last!=last_late_taker_generation_.end() && last->second==b->generation) {
                    status="LATE_TAKER_WAIT_BOOK";
                    continue;
                }
                OrderRequest req=b->market; req.dir=OrderDir::Sell; req.size_shares=p.shares;
                req.limit_price=b->bid; req.execution_mode=ExecutionMode::Taker;
                req.maker_fixed_price=false; req.source="RECYCLER_LATE_TAKER_EXIT";
                out.push_back({{},req});
                last_late_taker_generation_[p.token_id]=b->generation;
                status="LATE_TAKER_SUBMITTING";
                continue;
            }

            if(!c.recycler_maniac_mode && p.shares+1e-9<maker_min_shares &&
               !late_risk_window && !risk_trigger) {
                status="DUST_HOLD_TO_SETTLEMENT";
                continue;
            }
            if(c.recycler_maniac_mode && risk_trigger && b->bid>0 && b->bid<p.avg_price) {
                if(sell_quote) {
                    for(const auto& q:quotes) if(q.req.token_id==p.token_id && q.req.dir==OrderDir::Sell) cancel(q);
                    status="RISK_EXIT_CANCEL_PASSIVE";
                    continue;
                }
                auto req=b->market;
                req.dir=OrderDir::Sell;
                req.size_shares=p.shares;
                req.limit_price=b->bid;
                req.execution_mode=ExecutionMode::Taker;
                req.maker_fixed_price=false;
                req.maker_reprice_ms=0;
                req.maker_max_age_ms=0;
                req.maker_cancel_before_close_ms=0;
                req.source="RECYCLER_LATE_RISK_TAKER_EXIT";
                out.push_back({{},req});
                status="LATE_RISK_EXIT_SUBMITTING";
                continue;
            }
            const bool five_min_final_cleanup=p.horizon_s==300 && remaining_ms>0 &&
                remaining_ms<=c.recycler_late_maker_secs*1000LL;
            const bool forced_maker_exit=(remaining_ms>0 &&
                remaining_ms<=c.recycler_late_maker_secs*1000LL) || risk_trigger;
            const double pct=target_pct(p.horizon_s,remaining_ms);
            if(c.recycler_maniac_mode) {
                const double required_net_per_share=p.avg_price*(1.0+pct/100.0);
                const double tick=std::max(0.001,b->market.tick_size);
                double minimum_bid=std::ceil(required_net_per_share/tick-1e-9)*tick;
                while(minimum_bid<1.0 &&
                      minimum_bid-fee_per_share(minimum_bid)+1e-9<required_net_per_share)
                    minimum_bid+=tick;
                if(sell_quote) {
                    for(const auto& q:quotes) if(q.req.token_id==p.token_id && q.req.dir==OrderDir::Sell) cancel(q);
                    status="NET_TARGET_CANCEL_PASSIVE";
                    continue;
                }
                if(minimum_bid>=1.0 || minimum_bid-fee_per_share(minimum_bid)+1e-9<required_net_per_share) {
                    status="NET_TARGET_UNREACHABLE";
                    continue;
                }
                if(b->bid+1e-9<minimum_bid) {
                    status="WAITING_FOR_NET_TARGET";
                    continue;
                }
                auto req=b->market;
                req.dir=OrderDir::Sell;
                req.size_shares=p.shares;
                req.limit_price=minimum_bid;
                req.execution_mode=ExecutionMode::Taker;
                req.maker_fixed_price=false;
                req.maker_reprice_ms=0;
                req.maker_max_age_ms=0;
                req.maker_cancel_before_close_ms=0;
                req.source="RECYCLER_NET_TARGET_TAKER_EXIT";
                out.push_back({{},req});
                status="NET_TARGET_SUBMITTING";
                continue;
            }
            const double target=std::min(.99,p.avg_price*(1.0+pct/100.0));
            auto req=b->market; req.dir=OrderDir::Sell; req.size_shares=p.shares;
            if(forced_maker_exit) {
                // Stay one tick above the visible bid. This is a passive quote,
                // not a hidden taker fallback; fills remain uncertain.
                const double toward_touch=b->bid+b->market.tick_size;
                req.limit_price=(risk_trigger || five_min_final_cleanup)
                    ? std::min(b->ask,toward_touch)
                    : std::min(b->ask,std::max(toward_touch,target));
                req.source=(risk_trigger || five_min_final_cleanup)
                    ? "RECYCLER_RISK_LATE_MAKER_EXIT":"RECYCLER_LATE_MAKER_EXIT";
            } else {
                req.limit_price=std::ceil(target/b->market.tick_size-1e-9)*b->market.tick_size;
                req.source=late_profit_window?"RECYCLER_LATE_PROFIT_TARGET":"RECYCLER_PROFIT_TARGET";
            }
            if(req.limit_price<=b->bid+1e-9) {
                status="LATE_MAKER_NO_PASSIVE_TICK";
                continue;
            }
            req.execution_mode=ExecutionMode::Maker; req.maker_fixed_price=true; req.maker_reprice_ms=0;
            req.maker_max_age_ms=0; req.maker_cancel_before_close_ms=0;
            const auto existing=std::find_if(quotes.begin(),quotes.end(),[&](const auto& q){
                return q.req.token_id==p.token_id && q.req.dir==OrderDir::Sell;
            });
            if(existing!=quotes.end()) {
                const bool changed=existing->req.source!=req.source ||
                    std::abs(existing->price-req.limit_price)>=b->market.tick_size-1e-9;
                if(changed) {
                    cancel(*existing);
                    status="LATE_EXIT_REPRICE_CANCEL";
                } else {
                    status=forced_maker_exit?"LATE_MAKER_RESTING":"TARGET_RESTING";
                }
                continue;
            }
            if(forced_maker_exit) {
                const auto last=last_late_maker_attempt_ms_.find(p.token_id);
                const auto retry_after_ms=std::max<int64_t>(1000,c.order_cooldown_ms);
                if(last!=last_late_maker_attempt_ms_.end() &&
                   (now<last->second || now-last->second<retry_after_ms)) {
                    status="LATE_MAKER_RETRY_COOLDOWN";
                    continue;
                }
            }
            out.push_back({{},req});
            if(forced_maker_exit) last_late_maker_attempt_ms_[p.token_id]=now;
            status=forced_maker_exit?"LATE_MAKER_SUBMITTING":"TARGET_SUBMITTING";
        }
        for(auto it=late_risk_tokens_.begin();it!=late_risk_tokens_.end();) {
            if(!held.contains(*it)) it=late_risk_tokens_.erase(it); else ++it;
        }
        for(auto it=last_late_maker_attempt_ms_.begin();it!=last_late_maker_attempt_ms_.end();) {
            if(!held.contains(it->first)) it=last_late_maker_attempt_ms_.erase(it); else ++it;
        }
        if(!out.empty()) return out;

        // Manage resting entry quotes independently by token so all four assets
        // can recycle in parallel. Drop an entry when momentum/band/time invalidates.
        bool any_entry_quote=false;
        std::string entry_cancel_reason;
        for(const auto& q:quotes) if(q.req.dir==OrderDir::Buy) {
            any_entry_quote=true;
            const auto* b=book_for(q.req.token_id);
            const bool settlement=q.req.source=="RECYCLER_SETTLEMENT_ENTRY";
            const auto remaining=b ? b->market.window_close_ms-now : 0;
            const double fair=b ? fair_side(*b,q.req.side,remaining) : -1.0;
            std::string cancel_reason;
            const bool opposite_position=std::any_of(positions.begin(),positions.end(),[&](const Position& p){
                return p.condition_id==q.req.condition_id && p.side!=q.req.side;
            });
            const bool same_token_position=std::any_of(positions.begin(),positions.end(),[&](const Position& p){
                return p.condition_id==q.req.condition_id && p.token_id==q.req.token_id;
            });
            if(!entries_allowed) cancel_reason="entries_paused";
            else if(c.recycler_mode=="off") cancel_reason="strategy_disabled";
            else if(!b) cancel_reason="book_missing";
            else if(!fresh(*b)) cancel_reason="book_stale";
            else if(same_token_position) cancel_reason="position_open";
            else if(settlement) {
                const bool supported_asset=b->market.asset=="BTC" || b->market.asset=="ETH" ||
                    b->market.asset=="SOL" || b->market.asset=="XRP";
                if(!c.recycler_settlement_entry_enabled) cancel_reason="settlement_entry_disabled";
                else if(!supported_asset || !supported_horizon(b->market.horizon_s)) cancel_reason="settlement_market_unsupported";
                else if(remaining<=0 || remaining>c.recycler_settlement_entry_secs*1000LL)
                    cancel_reason="settlement_entry_time";
                else if(q.price<c.recycler_settlement_entry_min_price || q.price>c.recycler_settlement_entry_max_price)
                    cancel_reason="settlement_entry_price";
                else if(opposite_position) cancel_reason="opposite_position";
            } else if(remaining<=c.recycler_entry_cutoff_secs*1000LL)
                cancel_reason="entry_cutoff";
            else if(fair<0) cancel_reason="forecast_unavailable";
            // New entries must clear the configured edge floor. Once a passive
            // quote is resting, retain it while it still has non-negative net
            // settlement edge; requiring the original floor on every sweep
            // caused small forecast movements to churn/cancel useful quotes.
            else if(net_settlement_edge(fair,q.price)-0.005 < -1e-9)
                cancel_reason="edge_nonpositive";
            else if(remaining>static_cast<int64_t>(b->market.horizon_s)*1000LL)
                cancel_reason="market_horizon_invalid";
            else if(b->ask-b->bid>.06) cancel_reason="spread_wide";
            // The configured band is checked on the original submitted price
            // and on the current midpoint below. An adaptive passive bid may
            // move below the lower bound while the midpoint remains eligible;
            // cancelling on the repriced bid itself would again strand a
            // partially filled order for a condition the operator allowed.
            else if(q.req.limit_price<c.recycler_entry_min_price ||
                    q.req.limit_price>c.recycler_entry_max_price ||
                    q.price>c.recycler_entry_max_price)
                cancel_reason="quote_price_out_of_range";
            // Fixed quotes must be cancelled if the book moves through them.
            // Adaptive Recycler bids are repriced by the executor on its
            // configured interval; cancelling them here after every partial
            // fill defeats that behavior and strands the remaining notional.
            else if((q.req.maker_fixed_price || q.req.maker_reprice_ms<=0) && q.price>=b->ask)
                cancel_reason="quote_would_cross";
            else if((b->bid+b->ask)/2<c.recycler_entry_min_price ||
                    (b->bid+b->ask)/2>c.recycler_entry_max_price)
                cancel_reason="midpoint_out_of_range";
            else if(opposite_position) cancel_reason="opposite_position";
            else if(!settlement && settlement_entry_attempted_.contains(b->market.condition_id))
                cancel_reason="settlement_entry_already_attempted";
            const bool valid=cancel_reason.empty();
            // Momentum is the entry trigger, not an ongoing validity condition.
            // Requiring the midpoint to keep rising while a passive bid rests
            // cancels nearly every quote before its configured maker TTL expires.
            if(!valid) {
                out.push_back({q.client_order_id,{},cancel_reason});
                if(entry_cancel_reason.empty()) entry_cancel_reason=cancel_reason;
            }
        }
        if(!out.empty()) {
            status="ENTRY_CANCEL_PENDING["+entry_cancel_reason+"]";
            return out;
        }
        if(any_entry_quote && !c.recycler_maniac_mode) { status="ENTRY_RESTING"; return out; }

        const bool session_loss_limit_hit = c.recycler_max_session_loss>0.0 &&
                                            net<=-c.recycler_max_session_loss;
        const bool allowed=c.recycler_mode!="off" && entries_allowed && !session_loss_limit_hit;
        if(!allowed) { status=session_loss_limit_hit?"SESSION_LOSS_LIMIT":"ENTRIES_PAUSED"; return out; }
        if(now<retry_ms_) { status="COOLDOWN"; return out; }
        if(c.recycler_settlement_entry_enabled) {
            for(const auto& b:books) {
                const auto& m=b.market;
                const auto remaining=m.window_close_ms-now;
                const bool supported_asset=m.asset=="BTC" || m.asset=="ETH" || m.asset=="SOL" || m.asset=="XRP";
                if(!fresh(b) || !supported_asset || !supported_horizon(m.horizon_s) || remaining<=0 ||
                   remaining>c.recycler_settlement_entry_secs*1000LL || held.contains(m.token_id) ||
                   occupied_conditions.contains(m.condition_id) || settlement_entry_attempted_.contains(m.condition_id) ||
                   std::find(c.blocked_assets.begin(),c.blocked_assets.end(),m.asset)!=c.blocked_assets.end()) continue;
                double price=std::floor(std::min({b.bid+m.tick_size,b.ask-m.tick_size,c.recycler_settlement_entry_max_price})/m.tick_size+1e-9)*m.tick_size;
                if(price<c.recycler_settlement_entry_min_price || price>b.ask-m.tick_size+1e-9) continue;
                if(c.recycler_maniac_mode) {
                    const double fair=fair_side(b,m.side,remaining);
                    const double required=std::max(0.0,c.recycler_min_settlement_edge_pct/100.0);
                    // A high token price is not itself proof of a near-certain
                    // win. Require fresh settlement-specific fair value to clear
                    // the resting maker bid, then hold the fill to resolution.
                    if(fair<0 || fair-price+1e-9<required) continue;
                }
                double window_deployed=0.0;
                for(const auto& p:positions) if(p.condition_id==m.condition_id) window_deployed+=p.shares*p.avg_price;
                for(const auto& q:quotes) if(q.req.dir==OrderDir::Buy && q.req.condition_id==m.condition_id) window_deployed+=q.price*q.remaining_shares;
                if(c.max_per_window_usd>0 && window_deployed+c.per_click_size_usd>c.max_per_window_usd+1e-9) continue;
                if(max_deployed_usd>0 && deployed_usd+pending_buy_usd+c.per_click_size_usd>max_deployed_usd+1e-9) continue;
                auto req=m; req.dir=OrderDir::Buy; req.size_usd=c.per_click_size_usd;
                req.limit_price=price; req.execution_mode=ExecutionMode::Maker; req.maker_fixed_price=true;
                req.maker_reprice_ms=0; req.maker_max_age_ms=0; req.maker_cancel_before_close_ms=0;
                req.source="RECYCLER_SETTLEMENT_ENTRY";
                out.push_back({{},req});
                settlement_entry_attempted_.insert(m.condition_id);
                settlement_hold_tokens_.insert(m.token_id);
                occupied_conditions.insert(m.condition_id);
                retry_ms_=now+std::max(250,c.order_cooldown_ms);
                status="SETTLEMENT_ENTRY_SUBMITTING";
                return out;
            }
        }
        int market_candidates=0, time_ok=0, spread_ok=0, free_market=0;
        int midpoint_ok=0, momentum_ok=0, reference_ok=0, edge_ok=0, capacity_ok=0;
        int cycle_blocked=0, cooldown_blocked=0, deployment_blocked=0, grace_blocked=0;
        int ref_stale=0, ref_price_missing=0, ref_strike_missing=0, ref_vol_missing=0;
        struct EntryCandidate {
            const RecyclerBook* book=nullptr;
            double entry=0, point_fair=0, net_edge=0, midpoint=0, click_usd=0;
            int improve_ticks=0;
        };
        EntryCandidate best_candidate;
        std::map<std::string,EntryCandidate> best_by_condition;
        status="WAITING_FOR_TWO_SIDED_MOMENTUM";
        for(const auto& b:books) {
            const auto& m=b.market;
            if(!fresh(b) || !supported_horizon(m.horizon_s) ||
               (m.asset!="BTC" && m.asset!="ETH" && m.asset!="SOL" && m.asset!="XRP") ||
               std::find(c.blocked_assets.begin(),c.blocked_assets.end(),m.asset)!=c.blocked_assets.end()) continue;
            ++market_candidates;
            // The opening minutes chase noise before the oracle and book
            // establish; skip entries until the configured grace elapses.
            if(c.recycler_entry_open_grace_secs>0 && b.window_open_ms>0 &&
               now-b.window_open_ms<static_cast<int64_t>(c.recycler_entry_open_grace_secs)*1000LL) {
                ++grace_blocked;
                continue;
            }
            if(m.window_close_ms-now<=c.recycler_entry_cutoff_secs*1000LL ||
               m.window_close_ms-now>static_cast<int64_t>(m.horizon_s)*1000LL) continue;
            ++time_ok;
            if(b.ask-b.bid>.06) continue;
            ++spread_ok;
            if(held.contains(m.token_id) || occupied_conditions.contains(m.condition_id)) continue;
            ++free_market;
            const double mid=(b.bid+b.ask)/2;
            if(mid<c.recycler_entry_min_price || mid>c.recycler_entry_max_price) continue;
            ++midpoint_ok;
            const auto& h=history_[m.token_id];
            // Evaluate both UP and DOWN outcome tokens. A falling underlying
            // asset makes the DOWN-token probability rise, so the same signal
            // logic can cycle either side without taking both outcomes at once.
            if(!rising(h,now,c.recycler_momentum_lookback_secs,c.recycler_min_momentum_ticks,m.tick_size)) continue;
            ++momentum_ok;
            auto& w=windows_[m.condition_id];
            if(w.cycles>=c.recycler_max_cycles ||
               (c.recycler_max_window_loss>0.0 && w.loss>=c.recycler_max_window_loss)) {
                ++cycle_blocked;
                continue;
            }
            // A window's first cycle carries the worst expectancy (2026-09-28
            // study); scale its click down while re-entries trade full size.
            const double click_usd = w.cycles==0 && c.recycler_first_entry_scale_pct<100.0
                ? c.per_click_size_usd*std::max(1.0,c.recycler_first_entry_scale_pct)/100.0
                : c.per_click_size_usd;
            // Improve the best bid by the configured number of ticks, while
            // remaining at least one tick below the ask. If the requested
            // improvement would cross or lock the offer, skip the quote rather
            // than silently reducing improvement or taking liquidity.
            const int improve_ticks=std::max(0,c.maker_improve_ticks);
            if(improve_ticks<=0) continue;
            double entry=std::floor((b.bid+improve_ticks*m.tick_size)/m.tick_size+1e-9)*m.tick_size;
            if(entry>=b.ask-1e-9 || entry>b.ask-m.tick_size+1e-9) continue;
            if(entry<c.recycler_entry_min_price || entry>c.recycler_entry_max_price) continue;
            if(c.recycler_maniac_mode) {
                const double required_net=entry*(1.0+c.recycler_target_profit_pct/100.0);
                const double tick=std::max(0.001,m.tick_size);
                double minimum_exit=std::ceil(required_net/tick-1e-9)*tick;
                while(minimum_exit<1.0 &&
                      minimum_exit-fee_per_share(minimum_exit)+1e-9<required_net)
                    minimum_exit+=tick;
                if(minimum_exit>=1.0 ||
                   minimum_exit-fee_per_share(minimum_exit)+1e-9<required_net) continue;
            }
            const int64_t remaining_ms=m.window_close_ms-now;
            const double point_fair=fair_side(b,m.side,remaining_ms);
            if(point_fair<0) {
                if(b.reference_updated_ms<=0 || now<b.reference_updated_ms ||
                   now-b.reference_updated_ms>c.recycler_reference_max_age_ms) ++ref_stale;
                if(b.reference_price<=0) ++ref_price_missing;
                if(b.reference_strike<=0) ++ref_strike_missing;
                if(!b.board_forecast.ready) ++ref_vol_missing;
                status="REFERENCE_UNAVAILABLE";
                continue;
            }
            ++reference_ok;
            // Match the Prediction Board's point-forecast edge and 0.5¢
            // execution reserve while scoring this passive limit, not the ask.
            const double net_edge=net_settlement_edge(point_fair,entry)-0.005;
            if(net_edge+1e-9<c.recycler_min_settlement_edge_pct/100.0) {
                status="SETTLEMENT_EDGE_GATE";
                continue;
            }
            ++edge_ok;
            if(max_deployed_usd>0.0 && deployed_usd+pending_buy_usd+click_usd>max_deployed_usd+1e-9) {
                ++deployment_blocked;
                continue;
            }
            double window_deployed=0.0;
            for(const auto& p:positions) if(p.condition_id==m.condition_id)
                window_deployed+=std::max(0.0,p.shares*p.avg_price);
            for(const auto& q:quotes) if(q.req.dir==OrderDir::Buy && q.req.condition_id==m.condition_id)
                window_deployed+=std::max(0.0,q.price*q.remaining_shares);
            if(c.max_per_window_usd>0.0 && window_deployed+click_usd>c.max_per_window_usd+1e-9) {
                status="WINDOW_CAP";
                ++deployment_blocked;
                continue;
            }
            const int64_t retry_delay=c.recycler_maniac_mode
                ? std::max<int64_t>(250,c.order_cooldown_ms) : 20000;
            if (auto last=last_entry_attempt_ms_.find(m.condition_id);
                last!=last_entry_attempt_ms_.end() && now-last->second<retry_delay) {
                ++cooldown_blocked;
                continue;
            }
            ++capacity_ok;
            const EntryCandidate candidate{&b,entry,point_fair,net_edge,mid,click_usd,improve_ticks};
            if(!best_candidate.book || net_edge>best_candidate.net_edge) {
                best_candidate=candidate;
            }
            if(c.recycler_maniac_mode) {
                auto it=best_by_condition.find(m.condition_id);
                if(it==best_by_condition.end() || net_edge>it->second.net_edge)
                    best_by_condition[m.condition_id]=candidate;
            }
        }
        std::vector<EntryCandidate> selected;
        if(c.recycler_maniac_mode) {
            for(const auto& [_,candidate]:best_by_condition) selected.push_back(candidate);
            std::sort(selected.begin(),selected.end(),[](const EntryCandidate& a,const EntryCandidate& b){
                return a.net_edge>b.net_edge;
            });
        } else if(best_candidate.book) {
            selected.push_back(best_candidate);
        }
        double planned_deployed=deployed_usd+pending_buy_usd;
        std::map<std::string,double> planned_by_condition;
        for(const auto& candidate:selected) {
            if(!candidate.book) continue;
            const auto& b=*candidate.book;
            const auto& m=b.market;
            if(max_deployed_usd>0.0 &&
               planned_deployed+candidate.click_usd>max_deployed_usd+1e-9) {
                ++deployment_blocked;
                continue;
            }
            double window_deployed=planned_by_condition[m.condition_id];
            for(const auto& p:positions) if(p.condition_id==m.condition_id)
                window_deployed+=std::max(0.0,p.shares*p.avg_price);
            for(const auto& q:quotes) if(q.req.dir==OrderDir::Buy && q.req.condition_id==m.condition_id)
                window_deployed+=std::max(0.0,q.price*q.remaining_shares);
            if(c.max_per_window_usd>0.0 &&
               window_deployed+candidate.click_usd>c.max_per_window_usd+1e-9) {
                ++deployment_blocked;
                continue;
            }
            auto req=m; req.dir=OrderDir::Buy; req.size_usd=candidate.click_usd;
            req.limit_price=candidate.entry; req.execution_mode=ExecutionMode::Maker;
            // The initial price is best bid + configured ticks. As the book
            // moves, reprice passively within this original price ceiling so a
            // small partial fill does not strand the rest of the requested
            // notional or become an accidental taker order.
            req.maker_fixed_price=false;
            req.decision_fair_probability=candidate.point_fair;
            req.decision_edge=candidate.net_edge;
            req.decision_reference_price=b.reference_price;
            req.decision_reference_strike=b.reference_strike;
            req.decision_reference_vol_per_sec=b.forecast_sigma_per_sec;
            req.decision_model_up_probability=b.board_forecast.probability;
            req.decision_model_up_low=b.board_forecast.low;
            req.decision_model_up_high=b.board_forecast.high;
            req.decision_midpoint=candidate.midpoint;
            req.decision_reference_age_ms=now-b.reference_updated_ms;
            req.maker_improve_ticks=candidate.improve_ticks;
            req.maker_reprice_ms=std::max(0,c.maker_reprice_ms);
            req.maker_max_age_ms=0;
            req.maker_cancel_before_close_ms=c.recycler_entry_cutoff_secs*1000;
            req.source=c.recycler_maniac_mode?"RECYCLER_MANIAC_MAKER_ENTRY":
                (c.recycler_mode=="sniper"?"RECYCLER_SNIPER_ENTRY":"RECYCLER_RANGE_ENTRY");
            out.push_back({{},req});
            planned_deployed+=candidate.click_usd;
            planned_by_condition[m.condition_id]+=candidate.click_usd;
            last_entry_attempt_ms_[m.condition_id]=now;
            token_cycles_[m.token_id].condition=m.condition_id;
            retry_ms_=now+std::max(250,c.order_cooldown_ms);
        }
        if(!out.empty() && !c.recycler_maniac_mode) {
            const auto& candidate=selected.front();
            const auto& m=candidate.book->market;
            status=fmt::format("ENTRY_SUBMITTING[reference-v3,side={},p={:.3f},edge={:.3f}]",
                m.side==ShareSide::UP?"UP":"DOWN",candidate.point_fair,candidate.net_edge);
        } else if(!out.empty()) {
            status=fmt::format("MANIAC_ENTRIES_SUBMITTING[count={},one_side_per_condition=true]",out.size());
        }
        if(out.empty()) {
            std::string reason="ENTRY_EVALUATION_BLOCKED";
            if(market_candidates==0) reason="NO_FRESH_RECYCLER_MARKETS";
            else if(time_ok==0 && grace_blocked>0) reason="OPEN_GRACE";
            else if(time_ok==0) reason="ENTRY_WINDOW_CUTOFF";
            else if(spread_ok==0) reason="SPREAD_TOO_WIDE";
            else if(free_market==0) reason="MARKET_OCCUPIED";
            else if(midpoint_ok==0) reason="MIDPOINT_OUT_OF_RANGE";
            else if(momentum_ok==0) reason="WAITING_FOR_RISING_PRICE";
            else if(cycle_blocked>0 && reference_ok==0) reason="CYCLE_LIMIT";
            else if(reference_ok==0) reason="REFERENCE_UNAVAILABLE";
            else if(edge_ok==0) reason="SETTLEMENT_EDGE_GATE";
            else if(deployment_blocked>0) reason="DEPLOYMENT_CAP";
            else if(cooldown_blocked>0) reason="ENTRY_RETRY_COOLDOWN";
            else if(capacity_ok==0) reason="RISK_OR_CYCLE_CAP";
            status=reason+"[mkt="+std::to_string(market_candidates)+
                ",grace="+std::to_string(grace_blocked)+
                ",time="+std::to_string(time_ok)+
                ",spread="+std::to_string(spread_ok)+
                ",free="+std::to_string(free_market)+
                ",mid="+std::to_string(midpoint_ok)+
                ",rise="+std::to_string(momentum_ok)+
                ",ref="+std::to_string(reference_ok)+
                ",ref_stale="+std::to_string(ref_stale)+
                ",ref_px0="+std::to_string(ref_price_missing)+
                ",ref_strike0="+std::to_string(ref_strike_missing)+
                ",ref_vol0="+std::to_string(ref_vol_missing)+
                ",edge="+std::to_string(edge_ok)+
                ",cycle="+std::to_string(cycle_blocked)+
                ",cool="+std::to_string(cooldown_blocked)+
                ",cap="+std::to_string(capacity_ok)+"]";
        }
        return out;
    }
};
} // namespace vp
